#include "ssh_terminal_widget.h"

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
    m_term->setScrollBarPosition(ScrollBarRight);

    // 终端命令智能提示（候选弹窗 + 历史记录），历史按配置名分组。
    // 本 widget 仅用于 SSH 会话（RDP 走 RdpPanel，不经过这里），无需协议判断。
    // 对应Python: cube-shell.py::SSHQTermWidget 提示相关初始化 (L7322-7359)
    m_suggest = new TerminalCommandSuggest(m_term, m_device.name, this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_term);
}

SshTerminalWidget::~SshTerminalWidget()
{
    // Stop the bridge's read thread and close the channel before teardown.
    if (m_bridge)
        m_bridge->stop();
}

void SshTerminalWidget::connectToHost()
{
    if (m_started)
        return;
    m_started = true;

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
        client->setPassword(device.password);

    // Keyboard-interactive MFA: libssh2 invokes this callback from the worker
    // thread, but it must show a dialog on the UI thread. We bounce through the
    // widget with a BlockingQueuedConnection (the worker is blocked in connect
    // anyway, so this is safe).
    SshPromptCallback promptCb = [this](const QString &prompt, bool echo) -> QString {
        if (QThread::currentThread() == this->thread()) {
            QString resp;
            promptForMfa(prompt, echo, resp);
            return resp;
        }
        QString resp;
        // Fire and forget the actual dialog call; run it on the UI thread and wait.
        QMetaObject::invokeMethod(this, [this, prompt, echo, &resp]() {
            promptForMfa(prompt, echo, resp);
        }, Qt::BlockingQueuedConnection);
        return resp;
    };

    QThread *worker = QThread::create([this, client, promptCb, device]() {
        SshError err;
        if (!client->connectToHost(promptCb, err)) {
            QMetaObject::invokeMethod(this, [this, msg = err.message]() {
                emit connectionFailed(msg);
            }, Qt::QueuedConnection);
            return;
        }

        // Open the pty shell at the terminal's current size.
        const QSize sz = m_term->session() ? m_term->session()->size() : QSize(80, 24);
        if (!client->openShell("xterm-256color", sz.width(), sz.height(), err)) {
            QMetaObject::invokeMethod(this, [this, msg = err.message]() {
                emit connectionFailed(msg);
            }, Qt::QueuedConnection);
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

        QMetaObject::invokeMethod(this, [this, client]() {
            m_client = client;   // take ownership (unique_ptr from shared copy)
            m_bridge = new SshBridge(m_term->session(), m_client.get(), this);
            connect(m_bridge, &SshBridge::channelClosed, this, &SshTerminalWidget::onDisconnected);
            connect(m_bridge, &SshBridge::shellMfaPromptDetected, this, &SshTerminalWidget::onMfaPrompt);
            // cwdChanged 从读线程发射 → 显式 QueuedConnection 切回 UI 线程。
            connect(m_bridge, &SshBridge::cwdChanged,
                    this, &SshTerminalWidget::cwdChanged, Qt::QueuedConnection);
            m_bridge->start();
            // Hook already injected on the worker thread above — do NOT inject again.
            emit connected();
        }, Qt::QueuedConnection);
    });
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
