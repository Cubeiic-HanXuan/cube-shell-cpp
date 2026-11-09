#pragma once

// TerminalExecutor.h — 在用户终端中执行 AI 命令，以带编号的哨兵检测完成状态。
//
// 对应Python: core/ai/ssh_agent.py::_TerminalExecutor
//
// 保留用户终端的交互能力，密码/确认提示仍在终端处理；调用方可在无终端时选择其他后端。
// Bash 的 PROMPT_COMMAND / Zsh 的 precmd 钩子在命令结束后输出
// __CUBE_AI_END__:<id>:<exit_code>，仅匹配当前编号的哨兵才算取得可靠退出结果。
//
// 线程模型：对象留在主线程，工作线程调用 runBlocking 后通过 invokeMethod 排队发送命令，
// 自身用 QWaitCondition 等待；原始终端数据也回到主线程处理，结果与缓冲由 m_mutex 保护。
// 取消/超时只结束本地等待，不保证远端进程已经终止。

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
    // 单条命令的输出缓冲上限，以 QString 字符计；超限保留末尾而不是持续增长。
    static constexpr int kMaxBufferChars = 512 * 1024;

    explicit TerminalExecutor(QObject *parent = nullptr);
    ~TerminalExecutor() override;

    // --- 终端绑定与命令提交 ---
    // 主线程调用；更换终端先取消旧等待，并使环境钩子的初始化记录失效。
    void setTerminal(QTermWidget *terminal);
    QTermWidget *terminal() const;

    // 等待当前命令的哨兵或超时；同一实例只允许一条命令在途。
    // 返回 (exitCode, capturedOutput)，超时/取消/错误时退出码为 -1，远端状态未知。
    // 可跨线程调用；所属线程调用时使用事件循环轮询，不能阻塞哨兵的接收。
    // stopFlag 用于提交前检查，运行中的主动停止由 cancel 唤醒等待者。
    // 对应Python: _TerminalExecutor.run_blocking
    QPair<int, QString> runBlocking(const QString &cmd, int timeoutMs = kDefaultTimeoutMs,
                                   const std::atomic<bool> *stopFlag = nullptr);
    void resetConnection(); // 断线时取消等待，重连后重新注入 shell 环境

signals:
    void outputReceived(const QString &text); // 清理控制码后的实时预览片段，非最终捕获结果

public:

    // 结束当前等待并唤醒阻塞线程；保留已有输出及“远端可能仍运行”的说明，不发送强杀命令。
    void cancel();

public slots:
    // 接收 SshBridge::rawDataForAi 提供的过滤前数据，否则结束哨兵已被桥接层去除。
    // 对应Python: _TerminalExecutor._on_received
    void onRawData(const QString &text);

private:
    void dispatch(const QString &cmd, int id); // 主线程发送，先核对编号以拒绝过期排队任务
    void setupEnvironment(); // 每个终端/连接周期注入一次，保留已有提示符钩子

    // 从 m_outputBuffer 里剥掉命令回显与哨兵，得到可回填给模型的纯输出。
    static QString extractOutput(const QString &cleanBuffer, int sentinelStart);
    // 去掉 CSI / OSC 等控制序列（哨兵匹配与最终输出都基于剥离后的文本）。
    static QString stripAnsi(const QString &text);
    // 以下 Locked 辅助函数由持有 m_mutex 的调用方使用，跨包 ANSI 状态也受同一锁保护。
    QString cleanChunkLocked(const QString &text);
    void appendOutputLocked(const QString &text);
    QString capturedOutputLocked() const;
    void resetOutputLocked();

    // runBlocking 被本对象所属线程（主线程）调用时的退化路径：QWaitCondition
    // 会把事件循环一起挂死（哨兵永远送不到），故改用局部事件循环轮询。
    QPair<int, QString> waitOnOwnThread(int timeoutMs);
    // 取出并清空当前结果（调用方须持有 m_mutex），此时才释放在途命令限制。
    QPair<int, QString> takeResultLocked();

    QPointer<QTermWidget> m_terminal; // 不拥有终端，终端销毁后自动置空

    // --- 跨线程同步 ---
    QMutex m_mutex;
    QWaitCondition m_condition;

    // --- 当前执行状态（由 m_mutex 保护） ---
    int m_currentId = 0; // 0 表示不再接收输出；结果待取时 m_inFlight 仍可能为 true
    int m_resultExitCode = -1;
    QString m_resultOutput;
    bool m_resultReady = false;
    bool m_inFlight = false; // 从提交到结果被取走期间拒绝重复执行
    std::atomic<bool> m_cancelled{false};

    // 清理后的输出缓冲，过期前缀按批压缩，避免每个网络包都搬移保留内容。
    QString m_outputBuffer;
    qsizetype m_outputStart = 0; // 有效输出的逻辑起点
    QString m_pendingAnsi; // 有界的未完成转义序列，留待下一包拼接
    bool m_outputTruncated = false;

    // 按终端实例记录环境是否已初始化；断线/更换终端时清零。
    quintptr m_initializedTerminalId = 0;

    // 命令编号不随取消回退，迟到的旧哨兵不能完成下一条命令。
    int m_nextId = 1;

    // 哨兵格式：__CUBE_AI_END__:<id>:<exit_code>
    static const QRegularExpression s_sentinelRe;
    // 支持清理的 ANSI 转义序列，不包含业务输出。
    static const QRegularExpression s_ansiRe;
};

} // namespace cubeshell
