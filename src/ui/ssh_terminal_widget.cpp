#include "ssh_terminal_widget.h"

#include <QDebug>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include "qtermwidget.h"
#include "Session.h"

#include "ssh/SshBridge.h"
#include "ssh/KnownHostsStore.h"
#include "ssh/SshClient.h"

#include "config/GlobalState.h"
#include "dialogs/HostKeyDialog.h"

#include "terminal_command_suggest.h"
#include "terminal_prompt.h"
#include "terminal_theme_util.h"
#include "terminal/session_recorder.h"

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

    // 断线重连覆盖层：默认隐藏，onDisconnected 时显示。
    m_reconnectOverlay = new QFrame(this);
    m_reconnectOverlay->setObjectName(QStringLiteral("reconnectOverlay"));
    m_reconnectOverlay->setStyleSheet(
        QStringLiteral("QFrame#reconnectOverlay { background: rgba(0, 0, 0, 180); }"));
    m_reconnectOverlay->setVisible(false);

    auto *overlayLayout = new QVBoxLayout(m_reconnectOverlay);
    overlayLayout->setAlignment(Qt::AlignCenter);

    m_reconnectReasonLabel = new QLabel(m_reconnectOverlay);
    m_reconnectReasonLabel->setAlignment(Qt::AlignCenter);
    m_reconnectReasonLabel->setStyleSheet(QStringLiteral("color: white; font-size: 14px;"));
    overlayLayout->addWidget(m_reconnectReasonLabel);

    auto *buttonLayout = new QHBoxLayout();
    auto *reconnectBtn = new QPushButton(tr("重新连接"), m_reconnectOverlay);
    auto *closeBtn = new QPushButton(tr("关闭标签页"), m_reconnectOverlay);
    buttonLayout->addWidget(reconnectBtn);
    buttonLayout->addWidget(closeBtn);
    overlayLayout->addLayout(buttonLayout);

    connect(reconnectBtn, &QPushButton::clicked, this, &SshTerminalWidget::reconnect);
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        emit connectionFailed(tr("用户关闭了已断开的标签页"));
    });
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

bool SshTerminalWidget::isPromptActive() const
{
    return m_prompt && m_prompt->active();
}

bool SshTerminalWidget::setSessionLogging(bool on, QString *errOut)
{
    if (on) {
        if (m_recorder && m_recorder->isActive())
            return true;   // 已在录制

        GlobalState &gs = GlobalState::instance();
        SessionRecorder::Options opt;
        opt.addTimestamps = gs.sessionLogTimestamps();
        opt.maxBytes = qint64(gs.sessionLogMaxMB()) * 1024 * 1024;   // 0=不轮转
        opt.backupCount = gs.sessionLogBackupCount();

        // 目标路径：自动命名（设备名+时间，落到默认目录）或另存为。
        QString path;
        if (gs.sessionLogAutoName()) {
            QString dir = gs.sessionLogDir();
            if (dir.isEmpty())
                dir = GlobalState::dataDir() + QStringLiteral("/session-logs");
            const QString tag = m_device.name.isEmpty()
                                    ? m_device.hostPort().host
                                    : m_device.name;
            path = SessionRecorder::autoFileName(tag, dir);
        } else {
            const QString suggested = QStringLiteral("ssh-%1.log")
                .arg(SessionRecorder::sanitizeTag(m_device.hostPort().host));
            path = QFileDialog::getSaveFileName(
                this, tr("录制会话日志"), suggested,
                tr("日志文件 (*.log);;所有文件 (*)"));
            if (path.isEmpty())
                return false;   // 用户取消，不算错误
        }

        auto rec = std::make_shared<SessionRecorder>();
        if (!rec->start(path, opt, errOut))
            return false;
        m_recorder = rec;
        if (m_bridge)
            m_bridge->setRecorder(m_recorder);
        return true;
    }

    // 停止：先从 bridge 摘掉（读线程不再拿到新引用），再停本地这份。
    if (m_bridge)
        m_bridge->setRecorder(nullptr);
    if (m_recorder)
        m_recorder->stop();
    m_recorder.reset();
    return true;
}

bool SshTerminalWidget::isSessionLogging() const
{
    return m_recorder && m_recorder->isActive();
}

QString SshTerminalWidget::sessionLogPath() const
{
    return isSessionLogging() ? m_recorder->filePath() : QString();
}

void SshTerminalWidget::connectToHost()
{
    if (m_started)
        return;
    m_started = true;

    // 密码没预存（且不是私钥登录）：先在终端画面里问一次，拿到了才建连。
    // 不能带着空密码去连——SshClient::authenticate() 会跳过 password 认证
    // 落到 keyboard-interactive，那条路的回调 promptForMfa() 是个对话框。
    // ssh-agent 设备既没有密码也不该问密码：凭据在本地 agent 里。
    if (!m_device.usesKey() && !m_device.usesAgent() && m_device.password.isEmpty()) {
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
    client->setCredentialKind(device.credentialKind);
    client->setAgentForwarding(device.agentForwarding);
    if (device.usesKey())
        client->setPrivateKey(device.keyType, device.keyFile);
    else
        client->setPassword(password);
    // 代理。device 是 MainWindow::openSshSession 用 resolved() 取出来的，
    // 代理口令已经填好（见 DeviceConfigStore::resolved）。
    // 类型是「全局代理」时不必在这里读设置——connectToHost 自己会去取。
    client->setProxyConfig(device.proxy);

    // 主机密钥校验。
    client->setKnownHostsStore(KnownHostsStore::defaultInstance());
    client->setHostKeyVerification(static_cast<HostKeyVerification>(
        GlobalState::instance().hostKeyVerification()));
    QPointer<SshTerminalWidget> hostKeySelf(this);
    client->setHostKeyPromptCallback(
        [hostKeySelf](const QString &host, quint16 port,
                      const QString &fingerprint, const QString &keyType,
                      bool keyChanged) -> HostKeyPromptResult {
            if (!hostKeySelf || hostKeySelf->m_teardown)
                return HostKeyPromptResult::Reject;

            HostKeyPromptResult result = HostKeyPromptResult::Reject;
            QMetaObject::invokeMethod(
                hostKeySelf,
                [hostKeySelf, host, port, fingerprint, keyType, keyChanged, &result]() {
                    if (!hostKeySelf || hostKeySelf->m_teardown)
                        return;
                    HostKeyDialog dlg(host, port, fingerprint, keyType, keyChanged,
                                      hostKeySelf->window());
                    if (dlg.exec() == QDialog::Accepted)
                        result = dlg.hostKeyResult();
                },
                Qt::BlockingQueuedConnection);
            return result;
        });

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
                    // 三次机会。私钥 / ssh-agent 登录没得重试（agent 失败该去
                    // 检查 agent 而不是重输密码），非认证类失败（网络不通、
                    // 主机不存在）重问也没意义，都直接报错。
                    if (authFailed && !self->m_device.usesKey()
                        && !self->m_device.usesAgent()
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
        // 目录追踪开关：受限的/不兼容的远端 shell（如 rbash、缺 awk/hostname
        // 的网络设备）执行该 hook 会刷一屏 "command not found"，关闭则跳过注入。
        if (self->m_device.directoryTracking)
            client->writeChannel(SshBridge::shellHookCommand());

        QMetaObject::invokeMethod(self, [self, client]() {
            if (!self)
                return;
            self->m_pendingClient.reset();
            self->m_client = client;   // take ownership (unique_ptr from shared copy)
            self->m_bridge = new SshBridge(self->m_term->session(), self->m_client.get(), self);
            // 重连后 bridge 是新建的：若此前已开录制，把同一个 recorder 挂回去。
            if (self->m_recorder)
                self->m_bridge->setRecorder(self->m_recorder);
            connect(self->m_bridge, &SshBridge::channelClosed, self, &SshTerminalWidget::onDisconnected);
            connect(self->m_bridge, &SshBridge::shellMfaPromptDetected, self, &SshTerminalWidget::onMfaPrompt);
            // cwdChanged 从读线程发射 → 显式 QueuedConnection 切回 UI 线程。
            connect(self->m_bridge, &SshBridge::cwdChanged,
                    self, &SshTerminalWidget::cwdChanged, Qt::QueuedConnection);
            self->m_bridge->start();
            // Hook already injected on the worker thread above — do NOT inject again.
            self->hideReconnectOverlay();
            emit self->connected();
        }, Qt::QueuedConnection);
    });
    m_connectWorker = worker;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void SshTerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_reconnectOverlay && m_reconnectOverlay->isVisible())
        m_reconnectOverlay->setGeometry(rect());
}

void SshTerminalWidget::showReconnectOverlay(const QString &reason)
{
    if (!m_reconnectOverlay)
        return;
    m_reconnectReasonLabel->setText(reason.isEmpty()
                                        ? tr("连接已断开")
                                        : tr("连接已断开：%1").arg(reason));
    m_reconnectOverlay->setGeometry(rect());
    m_reconnectOverlay->setVisible(true);
    m_reconnectOverlay->raise();
}

void SshTerminalWidget::hideReconnectOverlay()
{
    if (m_reconnectOverlay)
        m_reconnectOverlay->setVisible(false);
}

void SshTerminalWidget::reconnect()
{
    hideReconnectOverlay();
    m_started = false;
    m_authAttempt = 0;
    m_teardown = false;
    m_client.reset();
    if (m_bridge) {
        m_bridge->stop();
        m_bridge->deleteLater();
        m_bridge = nullptr;
    }
    connectToHost();
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
    showReconnectOverlay(QString());
    emit disconnected();
}

} // namespace cubeshell
