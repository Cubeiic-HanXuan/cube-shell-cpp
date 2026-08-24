#include "ssh_terminal_widget.h"

#include <QDebug>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QThread>
#include <QVBoxLayout>

#include "qtermwidget.h"
#include "Session.h"

#include "ssh/SshBridge.h"
#include "ssh/SshClient.h"

#include "config/GlobalState.h"

#include "terminal_command_suggest.h"
#include "terminal_prompt.h"
#include "terminal_theme_util.h"

namespace cubeshell {

SshTerminalWidget::SshTerminalWidget(const DeviceEntry &device, QWidget *parent)
    : QWidget(parent)
    , m_device(device)
{
    // startnow=0: do NOT spawn a local shell. The Session is created and the
    // view attached, but the terminal idles until the SSH channel feeds it.
    m_term = new QTermWidget(0, this);
    // 应用 theme.json 配置的字体(font/font_size)。
    // 对应Python: cube-shell.py::setup_code_font
    QFont font(GlobalState::instance().fontFamily(), GlobalState::instance().fontSize());
    if (font.family().isEmpty())
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_term->setTerminalFont(font);
    // Ctrl/Cmd+滚轮缩放后同步字号到内存主题，新开终端沿用。
    // 对应Python: zoom_in/zoom_out 中 util.THEME['font_size'] = size
    connect(m_term, &QTermWidget::fontSizeChanged, this,
            [](int size) { GlobalState::instance().setFontSize(size); });
    // 从 theme.json 读取终端配色方案(对应 Python current_theme_name)
    m_term->setColorScheme(GlobalState::instance().terminalTheme());
    // 右键菜单切换配色后：持久化到 theme.json 并同步到所有已打开终端。
    connect(m_term, &QTermWidget::colorSchemeChanged, this,
            [this](const QString &name) { applyTerminalThemeEverywhere(name, this); });
    // 回滚缓冲行数（同时决定“查找”能检索到多久以前的输出）。
    m_term->setHistorySize(GlobalState::instance().scrollbackLines());
    m_term->setScrollBarPosition(ScrollBarRight);

    // 终端命令智能提示（候选弹窗 + 历史记录），历史按配置名分组。
    // 本 widget 仅用于 SSH 会话（RDP 走 RdpPanel，不经过这里），无需协议判断。
    // 对应Python: cube-shell.py::SSHQTermWidget 提示相关初始化 (L7322-7359)
    m_suggest = new TerminalCommandSuggest(m_term, m_device.name, this);

    // 终端内就地读一行（连接时问密码）。
    m_prompt = new TerminalPrompt(m_term, this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_term);
}

// 析构时等待连接 worker 退出的上限：m_pendingClient->shutdownSocket() 之后
// 在途握手/认证会立刻带错误返回，正常远小于此值。超时兑底不阻塞 UI——
// worker 的回跳全部有 QPointer 守卫，逃逸后摸到的是空指针而不是悬垂 this。
static constexpr int kConnectWorkerJoinTimeoutMs = 5000;

SshTerminalWidget::~SshTerminalWidget()
{
    // 停机标志最先置位：worker 的 MFA 回调（BlockingQueuedConnection）看到它
    // 直接返回空应答，不会往正在析构的 UI 线程上死等。
    m_teardown = true;
    // 打断仍在进行的连接（connectToHost 可能正阻塞在 TCP/SSH 握手上），
    // 否则下面的 join 要等满网络超时。
    if (m_pendingClient)
        m_pendingClient->shutdownSocket();
    // Stop the bridge's read thread and close the channel before teardown.
    if (m_bridge)
        m_bridge->stop();
    // 有限等待连接 worker：它可能还在 connectToHost/openShell 里，而它的
    // 回跳 lambda 捕获的是本组件（QPointer 守卫）。等干净了再走 ~QObject，
    // 杜绝"worker 事后 invokeMethod 悬垂 this"的 SIGSEGV。
    if (m_connectWorker) {
        m_connectWorker->quit();
        if (!m_connectWorker->wait(kConnectWorkerJoinTimeoutMs))
            qWarning() << "SshTerminalWidget: connect worker did not finish within"
                       << kConnectWorkerJoinTimeoutMs << "ms";
    }
}

void SshTerminalWidget::connectToHost()
{
    if (m_started)
        return;
    m_started = true;

    // 密码没预存（且不是私钥登录）：先在终端画面里问一次，拿到了才建连。
    // 不能带着空密码去连——SshClient::authenticate() 会跳过 password 认证
    // 落到 keyboard-interactive，那条路的回调 promptForMfa() 是个对话框。
    if (!m_device.usesKey() && m_device.password.isEmpty()) {
        promptForPassword(QString());
        return;
    }
    startConnect(m_device.password);
}

void SshTerminalWidget::promptForPassword(const QString &notice)
{
    if (!m_prompt) {
        emit connectionFailed(tr("终端未就绪，无法输入密码。"));
        return;
    }

    if (!notice.isEmpty())
        m_prompt->write(notice + QLatin1Char('\n'));

    // 输密码期间停掉命令提示：这一次 Enter 会让它把提示符那行垃圾当成命令
    // 写进历史，候选弹窗也不该在密码上方冒出来。
    if (m_suggest)
        m_suggest->setPaused(true);

    const QString label = tr("%1@%2 的密码：")
                              .arg(m_device.username, m_device.hostPort().host);
    QPointer<SshTerminalWidget> self(this);
    m_prompt->ask(label, false, [self](bool ok, const QString &text) {
        if (!self || self->m_teardown)
            return;
        if (!ok) {
            // Ctrl+C/Ctrl+D：放弃连接。走 disconnected() 而不是
            // connectionFailed()——后者会弹警告框并关掉标签页，而用户只是
            // 取消了这一次输入，不该被"报错"打断。
            if (self->m_suggest)
                self->m_suggest->setPaused(false);
            self->m_prompt->write(tr("已取消连接。可关闭本标签页后重新连接。")
                                  + QLatin1Char('\n'));
            // 闸门复位：这次没建成连接，connectToHost() 应当还能再来一遍
            //（当前 UI 没有 SSH 标签页内重连入口，但别让状态自己把路堵死）。
            self->m_started = false;
            self->m_authAttempt = 0;
            emit self->disconnected();
            return;
        }
        if (text.isEmpty()) {
            // 空回车不算一次尝试（还没发出去），直接再问。
            self->promptForPassword(QString());
            return;
        }
        if (self->m_suggest)
            self->m_suggest->setPaused(false);
        self->m_prompt->write(tr("正在连接 %1…").arg(self->m_device.hostPort().host)
                              + QLatin1Char('\n'));
        self->startConnect(text);
    });

    // 状态栏还挂着 openSshSession() 设的"正在连接…"，纠正成等输入。
    emit awaitingPassword();
}

void SshTerminalWidget::startConnect(const QString &password)
{
    ++m_authAttempt;

    const DeviceEntry device = m_device;

    // The client object lives on the heap (shared_ptr) and is only touched from
    // this one worker until it is handed to the bridge back on the UI thread.
    const HostPort hp = device.hostPort();
    auto client = std::make_shared<SshClient>();
    client->setHost(hp.host, hp.port);
    client->setUsername(device.username);
    if (device.usesKey())
        client->setPrivateKey(device.keyType, device.keyFile);
    else
        client->setPassword(password);

    // Keyboard-interactive MFA: libssh2 invokes this callback from the worker
    // thread, but it must show a dialog on the UI thread. We bounce through the
    // widget with a BlockingQueuedConnection (the worker is blocked in connect
    // anyway, so this is safe).
    //
    // 全程 QPointer 守卫 + m_teardown 短路：组件析构后（关标签页）回调直接
    // 返回空应答让认证失败、连接中止——绝不能对悬垂 this 调 thread()/
    // BlockingQueuedConnection（后者还会和析构里的 join 互相死等）。
    QPointer<SshTerminalWidget> self(this);
    SshPromptCallback promptCb = [self](const QString &prompt, bool echo) -> QString {
        if (!self || self->m_teardown)
            return QString();
        if (QThread::currentThread() == self->thread()) {
            QString resp;
            self->promptForMfa(prompt, echo, resp);
            return resp;
        }
        QString resp;
        // Fire and forget the actual dialog call; run it on the UI thread and wait.
        QMetaObject::invokeMethod(self, [self, prompt, echo, &resp]() {
            if (self && !self->m_teardown)
                self->promptForMfa(prompt, echo, resp);
        }, Qt::BlockingQueuedConnection);
        return resp;
    };

    // 登记在途连接，供析构时打断握手（见 ~SshTerminalWidget）。
    m_pendingClient = client;

    // worker 全程用 QPointer 回跳：组件可能（关标签页）先于连接完成析构，
    // 裸 this 的 invokeMethod 会直接 SIGSEGV（崩溃报告实录）。
    QThread *worker = QThread::create([self, client, promptCb]() {
        SshError err;
        if (!client->connectToHost(promptCb, err)) {
            if (self) {
                QMetaObject::invokeMethod(self, [self, msg = err.message,
                                                 authFailed = err.authFailed]() {
                    if (!self)
                        return;
                    self->m_pendingClient.reset();
                    // 密码不对（含预存的旧密码）就在终端里重问，同 ssh 的
                    // 三次机会。私钥登录没得重试，非认证类失败（网络不通、
                    // 主机不存在）重问也没意义，都直接报错。
                    if (authFailed && !self->m_device.usesKey()
                        && self->m_authAttempt < kMaxAuthAttempts) {
                        self->promptForPassword(tr("认证失败，请重试。"));
                        return;
                    }
                    emit self->connectionFailed(msg);
                }, Qt::QueuedConnection);
            }
            return;
        }
        // 连接成功但组件已没了：client 随 lambda 销毁，不再回跳。
        if (!self)
            return;

        // Open the pty shell at the terminal's current size.
        const QSize sz = self->m_term->session() ? self->m_term->session()->size() : QSize(80, 24);
        if (!client->openShell("xterm-256color", sz.width(), sz.height(), err)) {
            if (self) {
                QMetaObject::invokeMethod(self, [self, msg = err.message]() {
                    if (self) {
                        self->m_pendingClient.reset();
                        emit self->connectionFailed(msg);
                    }
                }, Qt::QueuedConnection);
            }
            return;
        }

        // Inject the shell-integration hook IMMEDIATELY on the worker thread,
        // before the QueuedConnection hop to the UI thread.  This mirrors the
        // Python version's timing where invoke_shell() and inject_shell_integration()
        // execute back-to-back.  Without this, the event-loop delay causes the
        // initial prompt to be buffered and read SEPARATELY from the hook echo,
        // resulting in two visible prompts.
        //
        // hook 由 SshBridge::shellHookCommand() 统一生成：执行后会把自己从
        // 远端 shell 历史里删掉（HISTCONTROL=ignorespace 不可依赖——CentOS 默认
        // 只有 ignoredups，没有 ignorespace）。hook 进了历史，按上下键就会翻出
        // 含 __cs_osc7 的那一行，readline 重绘（\r + 提示符 + 历史 + \e[K，
        // 不带换行）显示层无法在不破坏重绘的前提下过滤——这正是上/下键显示
        // 异常的根因，必须让它根本不进历史。
        client->writeChannel(SshBridge::shellHookCommand());

        QMetaObject::invokeMethod(self, [self, client]() {
            if (!self)
                return;
            self->m_pendingClient.reset();
            self->m_client = client;   // take ownership (unique_ptr from shared copy)
            self->m_bridge = new SshBridge(self->m_term->session(), self->m_client.get(), self);
            connect(self->m_bridge, &SshBridge::channelClosed, self, &SshTerminalWidget::onDisconnected);
            connect(self->m_bridge, &SshBridge::shellMfaPromptDetected, self, &SshTerminalWidget::onMfaPrompt);
            // cwdChanged 从读线程发射 → 显式 QueuedConnection 切回 UI 线程。
            connect(self->m_bridge, &SshBridge::cwdChanged,
                    self, &SshTerminalWidget::cwdChanged, Qt::QueuedConnection);
            self->m_bridge->start();
            // Hook already injected on the worker thread above — do NOT inject again.
            emit self->connected();
        }, Qt::QueuedConnection);
    });
    m_connectWorker = worker;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

bool SshTerminalWidget::promptForMfa(const QString &prompt, bool echo, QString &response)
{
    bool ok = false;
    response = QInputDialog::getText(this, tr("MFA 验证"),
                                     prompt,
                                     echo ? QLineEdit::Normal : QLineEdit::Password,
                                     QString(), &ok);
    return ok;
}

void SshTerminalWidget::onMfaPrompt(const QString &prompt)
{
    // The shell stream itself is asking for a code (e.g. sudo / login OTP).
    // Surface it and let the user type the code into the terminal directly.
    emit mfaRequested(prompt);
}

void SshTerminalWidget::onDisconnected()
{
    emit disconnected();
}

} // namespace cubeshell
