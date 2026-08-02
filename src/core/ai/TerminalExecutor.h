#pragma once

// TerminalExecutor.h — Execute AI commands in the user's terminal with sentinel-based completion detection.
//
// 对应Python: core/ai/ssh_agent.py::_TerminalExecutor
//
// All AI commands are forced through the terminal (matching Python behavior) to naturally
// support interactive prompts (sudo passwords, y/n confirmations, pagers).
// Uses PROMPT_COMMAND hook to inject a sentinel (__CUBE_AI_END__:<id>:<exit_code>)
// after each command completes.
//
// Thread model: this object lives on the main thread. Worker threads call runBlocking()
// which dispatches to the main thread via QMetaObject::invokeMethod and blocks on
// QWaitCondition until the sentinel is detected or timeout fires.

#include <QMutex>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QWaitCondition>

#include <atomic>

class QTermWidget;

namespace cubeshell {

class TerminalExecutor : public QObject {
    Q_OBJECT
public:
    // 对应Python: _TerminalExecutor.run_blocking 的 timeout=180
    static constexpr int kDefaultTimeoutMs = 180 * 1000;
    // 单条命令的输出缓冲上限（防止 yes/tail -f 之类刷屏把内存吃光）。
    static constexpr int kMaxBufferChars = 512 * 1024;

    explicit TerminalExecutor(QObject *parent = nullptr);
    ~TerminalExecutor() override;

    // Bind/unbind the terminal. Must be called from the main thread.
    void setTerminal(QTermWidget *terminal);
    QTermWidget *terminal() const;

    // Execute a command in the terminal, blocking until the sentinel is detected
    // or timeout fires. Safe to call from ANY thread (cross-thread dispatch).
    // Returns (exitCode, capturedOutput). exitCode=-1 on timeout/cancel/error.
    // 对应Python: _TerminalExecutor.run_blocking
    QPair<int, QString> runBlocking(const QString &cmd, int timeoutMs = kDefaultTimeoutMs);

    // Cancel the currently running command (wakes the blocked thread).
    void cancel();

public slots:
    // Feed raw terminal data (BEFORE SshBridge filtering). Connected to SshBridge::rawDataForAi.
    // 对应Python: _TerminalExecutor._on_received
    void onRawData(const QString &text);

private:
    Q_INVOKABLE void dispatch(const QString &cmd);
    void setupEnvironment();

    // 从 m_outputBuffer 里剥掉命令回显与哨兵行，得到可回填给 LLM 的纯输出。
    static QString extractOutput(const QString &cleanBuffer, int sentinelStart);
    // 去掉 CSI / OSC 等控制序列（哨兵匹配与最终输出都基于剥离后的文本）。
    static QString stripAnsi(const QString &text);

    // runBlocking 被本对象所属线程（主线程）调用时的退化路径：QWaitCondition
    // 会把事件循环一起挂死（哨兵永远送不到），故改用局部事件循环轮询。
    QPair<int, QString> waitOnOwnThread(int timeoutMs);
    // 取出并清空当前结果（调用方须持有 m_mutex）。
    QPair<int, QString> takeResultLocked();

    QPointer<QTermWidget> m_terminal;

    // Cross-thread synchronization
    QMutex m_mutex;
    QWaitCondition m_condition;

    // Current execution state (protected by m_mutex)
    int m_currentId = 0;
    int m_resultExitCode = -1;
    QString m_resultOutput;
    bool m_resultReady = false;
    std::atomic<bool> m_cancelled{false};

    // Output buffer for current command
    QString m_outputBuffer;

    // Environment initialization tracking (per terminal instance)
    quintptr m_initializedTerminalId = 0;

    // Command ID counter
    int m_nextId = 1;

    // Sentinel regex: __CUBE_AI_END__:<id>:<exit_code>
    static const QRegularExpression s_sentinelRe;
    // ANSI escape sequence regex for stripping
    static const QRegularExpression s_ansiRe;
};

} // namespace cubeshell
