#pragma once

// AiConversationModel.h — 会话级时间线数据与展示状态，不依赖可见面板。
// 网络协议历史由 ChatHistory 保存；本模型保留消息、执行事实及折叠状态。

#include "AiInteractionTypes.h"
#include "CommandSafetyChecker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSet>

namespace cubeshell {

// 一轮对话内的时间线条目；按 kind 使用正文、命令或活动数据。
// id 稳定标识条目，revision 标识内容变化，视图据此复用控件并跳过重复渲染。
struct AiConversationEntry {
    enum Kind { Message, Command, Notice, Activity } kind = Message;
    quint64 id = 0;
    quint64 parentEntryId = 0; // 0 表示顶层；命令可挂在对应工具活动下
    AiActivity activity;
    int revision = 0; // 修改条目内容时递增；折叠状态单独通知，不依赖此版本
    QString text; // 消息/提示的正文；命令条目中保存跳过原因等附加说明
    QString reasoning; // 思考内容独立展示，不作为最终回答
    AiCommand command;
    AiCommandResult result;
    AiStepState state = AiStepState::Pending;
    bool finalAnswer = false; // 此消息已作为最终报告展示，过程区无需重复正文
    bool livePreviewLimited = false; // 实时输出已达预览上限，等待最终结果替换
};

// 一次用户提交对应一轮；可包含多次模型请求及多批命令执行。
struct AiConversationTurn {
    quint64 id = 0;
    QString userText;
    QString model; // 创建轮次时的模型名称，后续切换配置不改写历史标签
    QList<AiConversationEntry> entries; // 按事件发生顺序追加，父子关系另由 ID 描述
    AiPhase phase = AiPhase::Waiting;
    AiTurnOutcome outcome = AiTurnOutcome::Incomplete;
    QString finalText; // 轮次结束后独立呈现的报告，可能来自模型或本地兜底
    bool localReport = false; // 由本地事实生成的兜底报告，不冒充模型最终结论
    QElapsedTimer clock;
    qint64 durationMs = 0; // 结束时冻结耗时，运行中视图读取 clock.elapsed()
    // 展示状态保存在会话中，切换 SSH 标签后恢复；不发送给模型。
    bool processCollapsed = false;
    QSet<quint64> expandedEntries;
};

// 会话拥有的内存记录，独立于面板生命周期；面板不可见时仍记录执行事实。
// 通过 changed 通知视图刷新，网络/执行调度由控制器负责，本类不执行命令。
// 不提供内部锁，控制器应在对象所属线程更新；工作线程结果通过队列连接转交。
class AiConversationModel : public QObject {
    Q_OBJECT
public:
    explicit AiConversationModel(QObject *parent = nullptr);
    const QList<AiConversationTurn> &turns() const { return m_turns; }
    // 返回最近一轮（也可能已结束）；空记录返回 nullptr，不转移所有权。
    const AiConversationTurn *currentTurn() const;
    bool isBusy() const;

    // --- 轮次与模型消息 ---
    // 同时只允许一轮运行；无法创建时返回 0。每次模型请求单独创建消息条目。
    quint64 beginTurn(const QString &input, const QString &model);
    quint64 beginMessage();
    // 增量只更新未结束轮次，最终以完整正文覆盖，兼容非流式与失败后的部分正文。
    void appendDelta(quint64 messageId, const QString &text, const QString &reasoning);
    void finalizeMessage(quint64 messageId, const QString &text, bool finalAnswer = false);
    void setPhase(AiPhase phase);

    // --- 执行事实与活动树 ---
    // addCommand 分配 stepId 并返回；调用方须将该 ID 带入执行器，不能按命令文本匹配。
    quint64 addCommand(AiCommand command);
    void setCommandState(quint64 stepId, AiStepState state, const QString &reason = {});
    // 结果按 stepId 写回原条目；真实结果可以迟到，但不重新开启已结束轮次。
    void setCommandResult(const AiCommandResult &result);
    void appendCommandOutput(quint64 stepId, const QString &text);
    void addNotice(const QString &text);
    // 同一轮内非空 callId 去重；仅关联已存在的父活动，生产者应先上报父节点。
    quint64 beginActivity(const AiActivity &activity, AiStepState state = AiStepState::Pending);
    void updateActivity(quint64 id, AiStepState state, const QString &output, qint64 durationMs = 0);

    // --- 轮次收尾 ---
    // 将仍待定的活动标为取消或未知，冻结耗时；命令本身的收尾状态由控制器填写。
    void finishTurn(AiTurnOutcome outcome, const QString &text, bool localReport = false);
    // 只列举记录中的命令状态，不依据退出码推断用户目标已完成。
    QString factualSummary(const QString &reason) const;

    // --- 仅供视图使用的状态 ---
    void setProcessCollapsed(quint64 turnId, bool collapsed);
    void setEntryExpanded(quint64 turnId, quint64 entryId, bool expanded);
    void clear(); // 忙碌时不清空，避免执行回调失去关联条目

signals:
    void changed(quint64 turnId); // 只通知被修改的轮次，由视图合并刷新
    void reset(); // 清空后视图需丢弃控件、分页位置及滚动缓存

private:
    void updateCommandActivity(AiConversationTurn &turn, quint64 parentId);
    AiConversationEntry *entry(quint64 id, AiConversationTurn **turn = nullptr);
    QList<AiConversationTurn> m_turns;
    quint64 m_nextId = 1; // 轮次与条目共用序号；清空记录也不回退，避免复用旧 ID
};

} // namespace cubeshell
