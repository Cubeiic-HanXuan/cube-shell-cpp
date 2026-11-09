#pragma once

// AiInteractionTypes.h — AI 交互层共享的数据契约。
// 区分网络请求、单条命令与整轮任务的完成状态：网络响应正常结束不等于命令
// 执行成功，命令成功也不等于用户目标已验证。各层不能混用这些状态枚举。

#include <QJsonArray>
#include <QMetaType>
#include <QString>

namespace cubeshell {

// --- 模型请求结果 ---
// 请求结果由 AiChatWorker 统一结算；流式增量仅供预览，以此结果为准。
// Complete 可以只有工具调用；Empty 包括仅返回思考内容的响应。
// Truncated 表示达到输出上限；Failed 表示传输/协议等失败；Cancelled 表示主动停止等待。
enum class AiRequestOutcome { Complete, Empty, Truncated, Failed, Cancelled };
struct AiRequestResult {
    quint64 requestId = 0; // 请求序号，用于排除上一请求迟到的回调
    QString text; // 累计正文；非 Complete 时也可能含有部分内容，不代表完整回答
    QJsonArray toolCalls; // 按索引聚合的 id/name/arguments 对象，参数仍需上层校验
    QString finishReason; // 保留服务端原始结束原因，用于区分 stop、length、tool_calls 等
    QString error; // 面向用户的错误/取消说明，由请求驱动在结算时脱敏
    AiRequestOutcome outcome = AiRequestOutcome::Complete;
};

// --- 整轮任务与单步状态 ---
// phase 描述当前正在做什么，outcome 描述结束结论；Finished 不隐含 Complete。
// Verifying 检查目标，Diagnosing 分析失败，Finalizing 仅根据既有事实生成报告。
enum class AiPhase { Waiting, Responding, AwaitingApproval, Executing,
                     Verifying, Diagnosing, Finalizing, Finished };
enum class AiTurnOutcome { Complete, Incomplete, Failed, Cancelled };
// AllowedFailure 仅表示非零退出后允许继续；Unknown 表示没有可靠的退出结果。
// Skipped 表示未执行（未获批准/前序失败等）；Cancelled 与远端进程已终止并不等价。
enum class AiStepState { Pending, Running, Succeeded, AllowedFailure,
                         Failed, Skipped, Cancelled, Unknown };

// --- 与服务商协议无关的展示事件 ---
// 生产者必须提供真实事件，不能把模型正文中的“调用工具”等叙述解释成已发生的执行。
// 枚举为时间线提供统一分类，不表示每种活动都已具备对应的执行后端。
enum class AiActivityKind { Tool, SubAgent, AgentMessage, Mcp, Skill };
struct AiActivity {
    AiActivityKind kind = AiActivityKind::Tool;
    QString callId; // 活动的外部关联 ID，用于去重及关联子命令
    QString parentCallId; // 父活动的 callId；空值表示无显式父活动
    QString name; // 工具、技能或活动名称
    QString summary; // 折叠态的一行概述，不替代原始输入/输出
    QString input;
    QString output;
    QString agent; // 活动所属或消息发送方的代理标识
    QString recipient; // 通信类活动的接收方
    QString server; // MCP 等活动的服务端标识
    QString inputLanguage = QStringLiteral("json"); // 仅控制代码区高亮，不决定执行方式
    QString outputLanguage = QStringLiteral("text");
    qint64 durationMs = 0;
};
QString aiActivityLabel(AiActivityKind kind);

// --- 命令执行事实 ---
// 执行器回传的原始捕获结果，用于时间线；发送给模型时另行清理、截断并脱敏。
struct AiCommandResult {
    QString cmd;
    QString description;
    int exitCode = -1; // 负值代表未观测到可靠退出码，不应直接解释为命令执行失败
    QString stdoutText; // 终端后端为合并输出；独立 exec 后端可区分 stdout/stderr
    QString stderrText;
    qint64 durationMs = 0;
    bool allowFailure = false; // 批次是否可在非零退出后继续，不会把失败改写为成功
    quint64 stepId = 0; // 对应会话模型中的命令条目，不以命令文本匹配结果
    QString toolCallId; // 对应服务商工具调用，供多工具结果分别回填
    // 消费端同时检查此标记与 exitCode；取消、超时、传输失败均不能据此断言远端状态。
    bool resultKnown = true;
};

// 状态与耗时的统一展示文案，避免面板、命令卡片和本地报告各自解释状态。
QString aiPhaseLabel(AiPhase phase);
QString aiStepLabel(AiStepState state);
QString aiDurationLabel(qint64 milliseconds);

} // namespace cubeshell

// 供 Qt 信号/槽传递结果；跨线程使用前由控制器注册对应元类型。
Q_DECLARE_METATYPE(cubeshell::AiRequestResult)
Q_DECLARE_METATYPE(cubeshell::AiCommandResult)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommandResult>)
