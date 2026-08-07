#pragma once

// SshBridge.h — C++ port of core/paramiko_bridge.py
//
// Bridges an interactive SSH shell channel (SshClient) to a qtermwidget
// Session. A background thread reads from the channel and forwards bytes to
// Session::onReceiveBlock (via a queued signal, so screen updates happen on
// the UI thread); keystrokes from the emulation are forwarded to the channel.

#include <QObject>
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

    // 从字节流中剥掉 OSC 7 序列，并把其中的 cwd 路径追加到 paths。
    // 纯字节扫描：不做 UTF-8 解码，避免多字节字符被网络分包切断后损坏。
    static QByteArray stripOsc7(const QByteArray &in, QList<QByteArray> &paths);
    // 剥除 _CUBE_ID=<数字>;<空白> 前缀，保留其后的真实命令（字节级）。
    static QByteArray stripCubeIdPrefix(const QByteArray &in);
    // 丢弃含任一标记的整行（按 \r 切分）。标记均为 ASCII，字节级比较安全。
    static QByteArray dropMarkerLines(const QByteArray &in,
                                      const QList<QByteArray> &markers);

    Konsole::Session *m_session;
    SshClient *m_client;

    QThread *m_readerThread = nullptr;
    std::atomic<bool> m_running{false};

    ShellMfaWatcher *m_mfaWatcher = nullptr;

    // 返回 in 末尾「不完整多字节 UTF-8 序列」的字节数（0 表示尾部完整）。
    // readChannel 按固定大小切块，切点可能落在多字节字符中间。
    static int incompleteUtf8TailLen(const QByteArray &in);

    // 跨读取块的残留字节：尾部不完整的 UTF-8 序列留到下一块拼接后再处理。
    // 不这样做的话，QString::fromUtf8 会把半个字符换成 U+FFFD，字节流被永久
    // 破坏且长度改变，提示符里的 ANSI 序列随之断裂 —— 表现为按上下键翻历史
    // 时提示符被冲掉、readline 重绘错位。仅读线程访问，无需加锁。
    QByteArray m_residual;
};

} // namespace cubeshell
