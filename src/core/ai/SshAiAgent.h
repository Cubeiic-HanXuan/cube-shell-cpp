#pragma once

// SshAiAgent.h — AI-driven SSH operations agent (tool-calling loop).
//
// 对应Python: core/ai/ssh_agent.py::SSHAIAgent + _CommandExecThread
//
// 工具调用循环：
//   用户输入 → LLM(流式, tools=execute_ssh_commands) → 解析 tool_calls /
//   文本 fallback 提取命令 → CommandSafetyChecker 安全检查 →
//   commandReady(待确认) → [UI 确认后] executeCommands →
//   AiCommandExecThread 逐条经 CommandExecutor 执行 → 结果回填对话
//   （ChatHistory::addCommandResult, user 角色）→ 失败自动诊断 /
//   全部成功触发目标验证（最多 kMaxVerifyRounds 轮）→ 继续对话。
//
// 未移植部分（Python 侧依赖的可选子系统，不在本阶段范围）：
//   audit.py（审计库）、skill_loader.py。系统提示词相应省略 Skill 段落。
//   服务器画像由 ServerProfileBuilder 异步构建后经 setServerProfile 注入。
//
// 线程模型：AiChatWorker 为纯异步（主线程事件驱动）；命令执行在
// AiCommandExecThread(QThread) 中同步调用 TerminalExecutor::runBlocking
//（未绑定终端时降级为 CommandExecutor::exec/sudoExec），
// 其信号跨线程，连接处全部显式 Qt::QueuedConnection。

#include "AiChatWorker.h"
#include "AiPreferences.h"
#include "ChatHistory.h"
#include "CommandSafetyChecker.h"

#include <QJsonArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

class QTermWidget;

namespace cubeshell {

class CommandExecutor;
class TerminalExecutor;

// 单条命令的执行结果。
// 对应Python: ssh_agent.py::_CommandExecThread.run 里组装的 result dict
struct AiCommandResult {
    QString cmd;
    QString description;
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    qint64 durationMs = 0;
    bool allowFailure = false;
};

// 命令执行工作线程：逐条同步执行（优先经 TerminalExecutor 走用户终端，
// 未绑定终端时降级到 CommandExecutor 的 exec channel）。
// 对应Python: ssh_agent.py::_CommandExecThread (QThread)
class AiCommandExecThread : public QThread {
    Q_OBJECT
public:
    AiCommandExecThread(CommandExecutor *executor,
                        TerminalExecutor *terminalExecutor,
                        const QList<AiCommand> &commands,
                        QObject *parent = nullptr);

    // 对应Python: _CommandExecThread.request_stop
    void requestStop();

signals:
    // 均从工作线程发出 — 连接必须显式 Qt::QueuedConnection。
    void commandStarted(const QString &cmd);                       // command_started
    void commandFinished(const cubeshell::AiCommandResult &result); // command_finished
    void allFinished(const QList<cubeshell::AiCommandResult> &results); // all_finished
    void execError(const QString &message);                        // error
    void progress(int current, int total);                         // progress

protected:
    void run() override;

private:
    CommandExecutor *m_executor;
    TerminalExecutor *m_terminalExecutor;
    QList<AiCommand> m_commands;
    std::atomic<bool> m_stopFlag{false};
};

// 对应Python: ssh_agent.py::SSHAIAgent
class SshAiAgent : public QObject {
    Q_OBJECT
public:
    // 对应Python: SSHAIAgent._MAX_VERIFY_ROUNDS
    static constexpr int kMaxVerifyRounds = 3;

    // executor 生命周期须长于 agent（不取所有权）。
    // 对应Python: SSHAIAgent.__init__(ssh_client, prefs)
    explicit SshAiAgent(CommandExecutor *executor,
                        const AiPreferences &prefs = AiPreferences::load(),
                        QObject *parent = nullptr);
    ~SshAiAgent() override;

    void setPreferences(const AiPreferences &prefs);
    AiPreferences preferences() const { return m_prefs; }

    // 绑定执行命令用的终端（AI 命令强制经终端执行以支持交互式提示）。
    // 对应Python: SSHAIAgent 内部 _TerminalExecutor 的终端绑定
    void setTerminal(QTermWidget *terminal);
    TerminalExecutor *terminalExecutor() const { return m_terminalExecutor; }

    // 处理用户自然语言输入（异步）。
    // 对应Python: SSHAIAgent.process_user_input
    void processUserInput(const QString &userInput);

    // 执行已确认的命令列表（工作线程）。
    // 对应Python: SSHAIAgent.execute_commands
    void executeCommands(const QList<AiCommand> &commands);

    // 对失败命令触发 AI 诊断。
    // 对应Python: SSHAIAgent.diagnose_error
    void diagnoseError(const QString &command, const QString &stdoutText,
                       const QString &stderrText, int exitCode);

    // 停止当前 AI 请求 / 命令执行。
    // 对应Python: SSHAIAgent.stop
    void stop();

    // 退出前安全关闭（等待执行线程结束）。
    // 对应Python: SSHAIAgent.shutdown
    void shutdown(int waitMs = 2000);

    // 对应Python: SSHAIAgent.clear_conversation
    void clearConversation();

    // 设置服务器画像（由 ServerProfileBuilder 异步构建后注入）。
    // 对应Python: ServerProfile.inject_to_system_prompt
    void setServerProfile(const QString &profile);

    ChatHistory *conversation() { return &m_conversation; }

    // --- 纯函数（供单测） ---

    // Function Calling 工具定义。
    // 对应Python: ssh_agent.py::TOOLS
    static QJsonArray toolsDefinition();

    // 从 AI 文本回复中正则提取命令（fallback；含 CJK/列表项/注释过滤）。
    // 对应Python: SSHAIAgent._extract_commands_from_text
    static QList<AiCommand> extractCommandsFromText(const QString &text);

signals:
    // 对应Python: SSHAIAgent 的同名信号集
    void commandReady(const QList<cubeshell::AiCommand> &commands); // command_ready
    void executionStarted(const QString &cmd);                      // execution_started
    void executionFinished(const cubeshell::AiCommandResult &result); // execution_finished
    void aiMessage(const QString &reasoning, const QString &content); // ai_message
    void errorOccurred(const QString &message);                     // error_occurred
    void thinkingStarted();                                         // thinking_started
    void thinkingFinished();                                        // thinking_finished
    void executionProgress(int current, int total);                 // execution_progress
    void diagnosingStarted();                                       // diagnosing_started
    void taskSummary(const QString &summary);                       // task_summary
    void commandOutput(const QString &text);                        // command_output

private:
    void onAiDelta(const QString &content, const QString &reasoning);
    void onAiCompleted(const QString &fullText, const QJsonArray &toolCalls,
                       const QString &finishReason);
    void onAiFailed(const QString &message);
    void onExecFinished(const AiCommandResult &result);
    void onAllExecFinished(const QList<AiCommandResult> &results);

    // 对应Python: SSHAIAgent._parse_tool_calls（handled=false 表示未识别）
    QList<AiCommand> parseToolCalls(const QJsonArray &toolCalls, bool *handled);
    // 对应Python: SSHAIAgent._check_commands_safety
    QList<AiCommand> checkCommandsSafety(const QList<AiCommand> &commands) const;
    // 对应Python: SSHAIAgent._verify_goal_completion
    void verifyGoalCompletion();
    // 对应Python: SSHAIAgent._build_system_prompt（无画像/Skill 段落）
    QString buildSystemPrompt() const;

    CommandExecutor *m_executor;
    AiPreferences m_prefs;
    AiChatWorker *m_aiWorker;
    ChatHistory m_conversation;
    CommandSafetyChecker m_safetyChecker;
    TerminalExecutor *m_terminalExecutor;
    AiCommandExecThread *m_execThread = nullptr;

    // 任务目标跟踪（诊断/验证闭环）
    // 对应Python: _original_goal / _is_diagnosing / _goal_verify_count
    QString m_originalGoal;
    bool m_isDiagnosing = false;
    int m_goalVerifyCount = 0;
    int m_lastDispatchedCount = 0;   // 本轮下发命令数（用于统计 skipped）

    QString m_serverProfile;    // ServerProfileBuilder 提供的服务器上下文
};

} // namespace cubeshell

Q_DECLARE_METATYPE(cubeshell::AiCommandResult)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommandResult>)
