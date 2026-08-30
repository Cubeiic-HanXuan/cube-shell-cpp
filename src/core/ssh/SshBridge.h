#pragma once

// SshBridge.h — C++ port of core/paramiko_bridge.py
//
// Bridges an interactive SSH shell channel (SshClient) to a qtermwidget
// Session. A background thread reads from the channel and forwards bytes to
// Session::onReceiveBlock (via a queued signal, so screen updates happen on
// the UI thread); keystrokes from the emulation are forwarded to the channel.

#include <QMutex>
#include <QObject>
#include <QThread>

#include <atomic>
#include <memory>

class QThread;

namespace Konsole { class Session; }

namespace cubeshell {

class SshClient;
class ShellMfaWatcher;
class SessionRecorder;

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

    // 生成 shell-integration hook 命令（OSC 7 cwd 上报，bash/zsh）。
    // 执行后会把自身从远端 shell 历史删除，保证按上下键翻不出 hook 行。
    // 供 ssh_terminal_widget 在 openShell 之后立刻调用。
    static QByteArray shellHookCommand();

    // 每行回显的固定结尾（不含 \n），readLoop 用它判断 hook 回显已到齐。
    static const char *hookEchoTail() { return "esac;fi"; }

    // 挂接会话日志录制器（shared_ptr，线程安全）。录制在读线程发生：
    // readLoop 在发任何过滤之前把 readChannel 的原始字节写进 recorder。
    // 用 shared_ptr 是因为 UI 侧可能在读线程仍持副本写盘时销毁自己那份——
    // 读线程的局部副本会把 recorder 续命到 writeRaw 返回，杜绝悬垂。
    void setRecorder(std::shared_ptr<SessionRecorder> rec);
    std::shared_ptr<SessionRecorder> recorder() const;

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
    // 字节流里的内部标记集合（shell hook、AI 执行器哨兵），均为 ASCII。
    static const QList<QByteArray> &markerList();
    // hook 回显是否已完整到达（折叠 readline 软换行后能匹配完整 hook 文本）。
    static bool containsHookEcho(const QByteArray &buf);
    // 折叠匹配删掉启动期缓冲里的每一处完整 hook 回显（tty 驱动层 + readline
    // 折行重绘可能各一次），保住同行的 banner 和提示符。
    static QByteArray stripHookEcho(const QByteArray &buf);
    // 丢弃含任一标记的整行（按 \r 切分）。标记均为 ASCII，字节级比较安全。
    static QByteArray dropMarkerLines(const QByteArray &in,
                                      const QList<QByteArray> &markers);

    Konsole::Session *m_session;
    SshClient *m_client;

    // 会话日志录制器（可为空）。读线程经 m_recorderMutex 拷贝出 shared_ptr 再写，
    // UI 侧 setRecorder 也走同一把锁——shared_ptr 拷贝期间不会被销毁。
    std::shared_ptr<SessionRecorder> m_recorder;
    mutable QMutex m_recorderMutex;

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

    // 连接启动期的输出缓冲。
    // hook 回显会被 SSH 包边界切成多块，分块过滤时残片（不含完整标记的部分）
    // 会泄漏上屏（`}`、`";fi` 之类），分块也可能把标记本身切断而漏判。
    // 因此会话开头先攒着不显示，直到：a) 收到 hook 回显的固定结尾（esac;fi）
    // 后的换行（整行到齐，交 dropMarkerLines 整行丢弃）；b) 缓冲超过上限
    // （远端没回显，放弃等待）；c) 通道静默多轮（兜底，防止终端冻结）。
    // 仅读线程访问，无需加锁。
    QByteArray m_bootstrapBuf;
    int m_idlePolls = 0;
    bool m_bootstrapDone = false;
};

} // namespace cubeshell
