#pragma once

// SshAiAgent.h — 会话级 AI 运维控制器，串联模型请求、命令审批、执行与结果回传。
//
// 一次用户输入是一轮任务，内部可以包含多次请求与多批命令；诊断和验证分别限次。
// ChatHistory 保存发给模型的协议历史，AiConversationModel 保存面向用户的执行时间线。
// 控制器留在对象所属线程；阻塞命令放入 AiCommandExecThread，结果通过队列信号转交。

#include "AiChatWorker.h"
#include "AiConversationModel.h"
#include "ChatHistory.h"

#include <QThread>
#include <atomic>
#include <functional>

class QTermWidget;

namespace cubeshell {
class CommandExecutor;
class TerminalExecutor;

// 可选执行适配器，优先于真实终端/SSH 后端，便于确定性、无网络测试。
using AiCommandRunner = std::function<AiCommandResult(const AiCommand &)>;

// 串行执行已批准批次；每条命令在执行前仍校验禁止规则。
// 非允许的失败或未知退出结果会终止批次；未执行条目由控制器补记为跳过。
class AiCommandExecThread : public QThread {
    Q_OBJECT
public:
    // 两个执行器均不转移所有权，必须在线程结束前保持有效；commands 按值保存。
    AiCommandExecThread(CommandExecutor *executor, TerminalExecutor *terminalExecutor,
                        const QList<AiCommand> &commands, QObject *parent = nullptr,
                        AiCommandRunner runner = {});
    void requestStop(); // 设置原子停止标记并唤醒执行器，不强行终止 QThread
signals:
    // 这些信号从 run 所在线程发出，更新界面的接收方须使用队列连接。
    void commandStarted(const cubeshell::AiCommand &command);
    void commandFinished(const cubeshell::AiCommandResult &result);
    void allFinished(const QList<cubeshell::AiCommandResult> &results);
    void execError(const QString &message);
    void progress(int current, int total);
protected:
    void run() override;
private:
    CommandExecutor *m_executor;
    TerminalExecutor *m_terminalExecutor;
    QList<AiCommand> m_commands;
    AiCommandRunner m_runner;
    std::atomic<bool> m_stopFlag{false};
};

class SshAiAgent : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxVerifyRounds = 3;
    static constexpr int kMaxDiagnoseRounds = 3;
    explicit SshAiAgent(CommandExecutor *executor,
                        const AiPreferences &prefs = AiPreferences::load(),
                        QObject *parent = nullptr, AiCommandRunner runner = {});
    ~SshAiAgent() override;

    // --- 配置与后端绑定 ---
    void setPreferences(const AiPreferences &prefs);
    AiPreferences preferences() const { return m_prefs; }
    void setTerminal(QTermWidget *terminal);
    TerminalExecutor *terminalExecutor() const { return m_terminalExecutor; }
    void setExecutor(CommandExecutor *executor); // 执行线程存活期间拒绝更换后端
    void connectionLost(); // 停止后续调度，将无法确认的运行结果保留为未知

    // --- 用户输入与审批 ---
    void processUserInput(const QString &input); // 空输入或忙碌时忽略，不替换当前轮次
    // 仅接受待审批列表的子集；stepId 与命令文本必须同时匹配，不能凭审批注入新命令。
    void executeCommands(const QList<AiCommand> &commands);
    void rejectPendingCommands(const QString &reason = QString());
    QList<AiCommand> pendingCommands() const { return m_pendingCommands; }
    void diagnoseError(const QString &command, const QString &stdoutText,
                       const QString &stderrText, int exitCode);

    // --- 停止与会话生命周期 ---
    void stop(); // 停止模型/执行等待；不保证远端进程已经终止
    // 先停止再等待线程；waitMs 到期仍继续等待，不能让后台线程访问已析构的执行器。
    void shutdown(int waitMs = 2000);
    void clearConversation(); // 仅空闲时同时清空协议历史和展示模型
    void setServerProfile(const QString &profile);
    ChatHistory *conversation() { return &m_conversation; }
    AiConversationModel *displayModel() const { return m_display; }
    // 轮次已结束但旧执行线程尚未退出时，仍不能接受下一轮输入。
    bool isBusy() const { return m_display->isBusy() || m_execThread; }

    // --- 纯协议辅助 ---
    static QJsonArray toolsDefinition();
    // 旧式文本工具兼容：只提取 shell 围栏中的候选行，不在此函数中执行。
    static QList<AiCommand> extractCommandsFromText(const QString &text);

signals:
    void commandReady(const QList<cubeshell::AiCommand> &commands); // 交由外部审批界面选择
    void executionStarted(const QString &cmd);
    void executionFinished(const cubeshell::AiCommandResult &result);
    void aiMessage(const QString &reasoning, const QString &content);
    void errorOccurred(const QString &message);
    void thinkingStarted();
    void thinkingFinished();
    void executionProgress(int current, int total);
    void diagnosingStarted();
    void taskSummary(const QString &summary);
    void commandOutput(const QString &text);
    void turnFinished(); // 业务轮次落定，只通知一次
    void executionIdle(); // 取消后执行线程退出，释放忙碌限制，不重复结束轮次

private:
    // --- 请求与任务状态机 ---
    void requestFollowup(const QString &prompt, AiPhase phase);
    void startRequest(AiPhase phase);
    void onAiCompleted(const AiRequestResult &result);
    void onAllExecFinished();
    void finishTurn(AiTurnOutcome outcome, const QString &text, bool local = false);
    void finalizeTask(const QString &reason); // 仅追加一次禁用工具的总结请求
    void finishLocally(AiTurnOutcome outcome, const QString &reason);
    void cancelTask(AiTurnOutcome outcome, const QString &reason);
    // 所有工具调用均需补齐结果（包括跳过/取消），然后才能发下一次模型请求。
    void closeToolBatch();
    QList<AiCommand> parseToolCalls(const QJsonArray &calls, QString *error);
    QList<AiCommand> checkCommandsSafety(const QList<AiCommand> &commands) const;
    void verifyGoalCompletion();
    QString buildSystemPrompt() const;

    CommandExecutor *m_executor; // 外部拥有的 SSH 后端，销毁前必须 shutdown
    AiPreferences m_prefs;
    AiChatWorker *m_aiWorker;
    ChatHistory m_conversation;
    CommandSafetyChecker m_safetyChecker;
    TerminalExecutor *m_terminalExecutor;
    AiConversationModel *m_display;
    AiCommandExecThread *m_execThread = nullptr; // 线程 finished 后清空，不能仅以轮次状态判断空闲
    AiCommandRunner m_runner;
    QString m_originalGoal; // 验证/诊断始终围绕本轮最初目标，而非最近一条工具输出
    int m_goalVerifyCount = 0;
    int m_diagnoseCount = 0;
    bool m_finalizing = false; // 总结阶段禁止进入新的命令审批/执行分支
    bool m_cancelled = false; // 先置位再中止传输，防止同步完成回调继续调度
    quint64 m_generation = 0; // 轮次/取消代号，隔离旧执行线程的排队回调
    quint64 m_requestId = 0; // 只接收本次模型请求的结算结果
    quint64 m_messageId = 0; // 本次模型响应在展示模型中的条目 ID
    quint64 m_runningStep = 0; // 实时终端输出当前应写入的命令条目，0 表示无运行项
    QList<AiCommand> m_pendingCommands; // 等待审批的候选命令
    QList<AiCommand> m_batchCommands; // 保留整批原始条目，用于补齐未执行项与回填结果
    QJsonArray m_pendingToolCalls; // 保留服务商协议中的调用 ID 与 function 结构
    QString m_serverProfile;
};
} // namespace cubeshell
