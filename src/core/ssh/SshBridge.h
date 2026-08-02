#pragma once

// SshBridge.h — C++ port of core/paramiko_bridge.py
//
// Bridges an interactive SSH shell channel (SshClient) to a qtermwidget
// Session. A background thread reads from the channel and forwards bytes to
// Session::onReceiveBlock (via a queued signal, so screen updates happen on
// the UI thread); keystrokes from the emulation are forwarded to the channel.

#include <QObject>
#include <QRegularExpression>
#include <QThread>

#include <atomic>

class QThread;

namespace Konsole { class Session; }

namespace cubeshell {

class SshClient;
class ShellMfaWatcher;

// 对应C++: class ParamikoBridge(QObject)
class SshBridge : public QObject {
    Q_OBJECT
public:
    // session: qtermwidget Session the terminal is attached to.
    // client:  an SshClient whose shell channel is already open.
    SshBridge(Konsole::Session *session, SshClient *client, QObject *parent = nullptr);
    ~SshBridge() override;

    void start();
    void stop();

    // Send interactive text (e.g. an MFA code) followed by carriage return.
    void sendInput(const QString &text);
    // Sync the terminal size to the remote pty.
    void resize(int columns, int rows);
    // Inject the OSC7 cwd-reporting shell hook (bash/zsh).
    void injectShellIntegration();

signals:
    void channelClosed();
    void dataReceived(const QByteArray &data);           // queued to Session::onReceiveBlock
    // 过滤前的原始数据（含 __CUBE_AI_END__ 哨兵），供 TerminalExecutor 检测命令结束。
    // 从读线程发出，接收方在主线程时 Qt 自动排队。
    void rawDataForAi(const QString &text);
    void cwdChanged(const QString &path);                // OSC 7 cwd report
    void shellMfaPromptDetected(const QString &prompt);  // MFA/OTP prompt in shell stream

private:
    void readLoop();           // runs on the worker thread
    void onEmulationSendData(const char *data, int length);

    Konsole::Session *m_session;
    SshClient *m_client;

    QThread *m_readerThread = nullptr;
    std::atomic<bool> m_running{false};

    ShellMfaWatcher *m_mfaWatcher = nullptr;

    static const QRegularExpression s_osc7Pattern;
    static const QRegularExpression s_cubeIdPattern;
};

} // namespace cubeshell
