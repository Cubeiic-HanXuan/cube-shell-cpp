#pragma once

// SshTerminalWidget.h — vertical-slice SSH terminal tab.
//
// A QTermWidget whose Session is bridged to a remote SSH shell channel
// (SshClient + SshBridge). This is the C++ counterpart of the SSHQTermWidget
// path in cube-shell.py, cut down to the vertical slice: connect -> open pty
// shell -> display + keystrokes.
//
// Connection + auth happen on a worker thread (SshClient is thread-hostile and
// blocking during connect); the SshBridge then runs the read loop on its own
// thread and pushes bytes into the terminal via queued signals.
//
// 密码可以不预存：设备条目里密码为空（且不是私钥登录）时，connectToHost()
// 先在终端画面里就地问一次（TerminalPrompt，不弹对话框），拿到了才建连；
// 认证失败也在终端里重问，最多 kMaxAuthAttempts 次。输入的密码只活在本次
// 会话的 SshClient 上，不写钥匙串。

#include <QPointer>
#include <QWidget>

#include <atomic>
#include <memory>
#include <utility>

#include "config/DeviceConfigStore.h"

class QTermWidget;
class QThread;
class QFrame;
class QLabel;

namespace cubeshell {

class SshClient;
class SshBridge;
class TerminalCommandSuggest;
class TerminalPrompt;
class SessionRecorder;

class SshTerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit SshTerminalWidget(const DeviceEntry &device, QWidget *parent = nullptr);
    ~SshTerminalWidget() override;

    // Kick off the connection (async). Safe to call once, right after ctor.
    void connectToHost();

    // The connected SSH client (null until connected()). Owned by this widget;
    // do not delete. Used to attach an SftpClient for the file browser.
    std::shared_ptr<SshClient> sshClient() const { return m_client; }
    // The terminal (for embedding / focusing).
    QTermWidget *terminal() const { return m_term; }
    // AI 交互式执行需要连接 bridge 的 rawDataForAi 信号（null 直到 connected()）。
    SshBridge *bridge() const { return m_bridge; }

    // 终端里正在进行就地输入（问密码 / MFA，TerminalPrompt 收键中）时返回 true。
    // 此时键盘喂给提示而非远端会话——多会话广播据此跳过本终端，既不把密码扇出
    // 到别的会话，也不让别的会话的输入落进本终端的密码框。
    bool isPromptActive() const;

    // 会话日志录制（审计追溯）。on=开始（按设置自动命名/时间戳/轮转），off=停止。
    // 返回 false 并填 errOut 表示打开日志文件失败。
    bool setSessionLogging(bool on, QString *errOut = nullptr);
    bool isSessionLogging() const;
    QString sessionLogPath() const;

public slots:
    // 显示断线重连覆盖层（心跳检测、网络异常等路径调用）。
    void showReconnectOverlay(const QString &reason = QString());

signals:
    // Emitted on the UI thread.
    void connected();
    void connectionFailed(const QString &message);
    void disconnected();            // channel closed (remote exit / drop)
    void mfaRequested(const QString &prompt);
    // 正在终端里等用户输密码（还没开始建连），供状态栏改文案——否则会一直
    // 挂着 MainWindow::openSshSession 设的"正在连接…"。
    void awaitingPassword();
    // OSC7 报告的远程 cwd（从 SshBridge 转发，已切回 UI 线程）。
    // 对应Python: cube-shell.py::_on_cwd_changed 的信号源
    void cwdChanged(const QString &path);

private slots:
    void onMfaPrompt(const QString &prompt);
    void onDisconnected();

private:
    bool promptForMfa(const QString &prompt, bool echo, QString &response);

    // 终端里就地问密码 → 拿到后 startConnect()。notice 非空时先在上面打一行
    // （"认证失败，请重试。"）。用户取消则放弃连接（不弹框、标签页留着）。
    void promptForPassword(const QString &notice);
    // 真正建连：worker 线程握手 + 认证 + 开 shell。password 由调用方给，
    // 可能来自设备条目，也可能来自刚才的终端提示。
    void startConnect(const QString &password);

    // 断开后显示的重连覆盖层。
    void hideReconnectOverlay();
    void reconnect();
    void resizeEvent(QResizeEvent *event) override;

    // 密码认证失败后允许在终端里重问的总次数（同 ssh 默认的 3 次）。
    static constexpr int kMaxAuthAttempts = 3;

    DeviceEntry m_device;
    QTermWidget *m_term = nullptr;
    // 终端命令智能提示控制器（子 QObject，随本 widget 析构）。
    // 对应Python: cube-shell.py::SSHQTermWidget 提示相关逻辑
    TerminalCommandSuggest *m_suggest = nullptr;
    // 终端内就地读一行（问密码用，子 QObject）。
    TerminalPrompt *m_prompt = nullptr;
    // 已发起的密码认证次数（含首次），到 kMaxAuthAttempts 就不再重问。
    int m_authAttempt = 0;

    // The client is created and connected on a worker thread, then handed to
    // the bridge. shared_ptr so the worker lambda can keep it alive until the
    // UI thread takes its copy.
    std::shared_ptr<SshClient> m_client;
    SshBridge *m_bridge = nullptr;         // child QObject

    bool m_started = false;

    // 会话日志录制器（shared_ptr 以便安全挂到读线程跑的 SshBridge 上）。
    // 为 nullptr 或 !isActive() 表示未在录制。
    std::shared_ptr<SessionRecorder> m_recorder;

    // 断线重连覆盖层
    QFrame *m_reconnectOverlay = nullptr;
    QLabel *m_reconnectReasonLabel = nullptr;

    // 连接中的 client/worker 登记：连接 worker 跑在后台，组件可能（关标签页）
    // 先于连接完成析构。worker 全程用 QPointer 回跳 UI；析构时凭
    // m_pendingClient 打断在途握手、有限等待 worker 退出。
    std::shared_ptr<SshClient> m_pendingClient;
    QPointer<QThread> m_connectWorker;
    // 停机标志：析构最先置位，MFA 回调（BlockingQueuedConnection）据此
    // 直接返回空应答而不是往正在析构的 UI 线程上死等。
    std::atomic<bool> m_teardown{false};
};

} // namespace cubeshell
