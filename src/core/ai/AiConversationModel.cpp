// AiConversationModel.cpp — 时间线记录更新、命令活动汇总与本地事实摘要。
// 条目内容变化递增 revision，轮次变化发送 changed；视图自行决定何时渲染。

#include "AiConversationModel.h"

namespace cubeshell {

QString aiActivityLabel(AiActivityKind kind)
{
    switch (kind) {
    case AiActivityKind::Tool: return QStringLiteral("工具");
    case AiActivityKind::SubAgent: return QStringLiteral("Agent");
    case AiActivityKind::AgentMessage: return QStringLiteral("通信");
    case AiActivityKind::Mcp: return QStringLiteral("MCP");
    case AiActivityKind::Skill: return QStringLiteral("Skill");
    }
    return {};
}

QString aiPhaseLabel(AiPhase phase)
{
    switch (phase) {
    case AiPhase::Waiting: return QStringLiteral("等待模型响应");
    case AiPhase::Responding: return QStringLiteral("正在生成回复");
    case AiPhase::AwaitingApproval: return QStringLiteral("等待命令确认");
    case AiPhase::Executing: return QStringLiteral("正在执行命令");
    case AiPhase::Verifying: return QStringLiteral("正在验证执行结果");
    case AiPhase::Diagnosing: return QStringLiteral("正在分析失败原因");
    case AiPhase::Finalizing: return QStringLiteral("正在生成最终报告");
    case AiPhase::Finished: return QStringLiteral("本轮已结束");
    }
    return {};
}

QString aiStepLabel(AiStepState state)
{
    switch (state) {
    case AiStepState::Pending: return QStringLiteral("待执行");
    case AiStepState::Running: return QStringLiteral("执行中");
    case AiStepState::Succeeded: return QStringLiteral("完成");
    case AiStepState::AllowedFailure: return QStringLiteral("非零退出 · 允许继续");
    case AiStepState::Failed: return QStringLiteral("失败");
    case AiStepState::Skipped: return QStringLiteral("已跳过");
    case AiStepState::Cancelled: return QStringLiteral("已取消");
    case AiStepState::Unknown: return QStringLiteral("结果未知");
    }
    return {};
}

QString aiDurationLabel(qint64 ms)
{
    if (ms < 1000) return QStringLiteral("%1ms").arg(ms);
    if (ms < 60000) return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1m%2s").arg(ms / 60000).arg((ms / 1000) % 60);
}

AiConversationModel::AiConversationModel(QObject *parent) : QObject(parent) {}

const AiConversationTurn *AiConversationModel::currentTurn() const
{
    return m_turns.isEmpty() ? nullptr : &m_turns.last();
}

bool AiConversationModel::isBusy() const
{
    return currentTurn() && currentTurn()->phase != AiPhase::Finished;
}

// 轮次与条目共用单调递增 ID，执行结果即使迟到也不会误命中新条目。
// 时钟从用户提交开始计时，包含模型等待、审批、执行及总结的完整耗时。
quint64 AiConversationModel::beginTurn(const QString &input, const QString &model)
{
    if (isBusy()) return 0;
    AiConversationTurn turn;
    turn.id = m_nextId++;
    turn.userText = input;
    turn.model = model;
    turn.clock.start();
    m_turns.append(turn);
    emit changed(turn.id);
    return turn.id;
}

quint64 AiConversationModel::beginMessage()
{
    if (!isBusy()) return 0;
    AiConversationEntry item;
    item.id = m_nextId++;
    m_turns.last().entries.append(item);
    emit changed(m_turns.last().id);
    return item.id;
}

// 回调通常更新最新轮次，从后向前查找；返回的指针仅供当前同步操作使用，
// 不应跨越向 QList 追加条目的操作保存，以免容器重分配后失效。
AiConversationEntry *AiConversationModel::entry(quint64 id, AiConversationTurn **turn)
{
    for (auto t = m_turns.rbegin(); t != m_turns.rend(); ++t) {
        for (auto e = t->entries.rbegin(); e != t->entries.rend(); ++e) {
            if (e->id == id) {
                if (turn) *turn = &*t;
                return &*e;
            }
        }
    }
    return nullptr;
}

void AiConversationModel::appendDelta(quint64 id, const QString &text, const QString &reasoning)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(id, &turn);
    if (!e || turn->phase == AiPhase::Finished) return;
    e->text += text;
    e->reasoning += reasoning;
    // 只有普通等待阶段在收到正文时切为回复中；验证/诊断/总结阶段保留其业务含义。
    if (turn->phase == AiPhase::Waiting && !text.isEmpty()) turn->phase = AiPhase::Responding;
    ++e->revision;
    emit changed(turn->id);
}

void AiConversationModel::finalizeMessage(quint64 id, const QString &text, bool finalAnswer)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(id, &turn);
    if (!e) return;
    e->text = text;
    e->finalAnswer = finalAnswer;
    ++e->revision;
    emit changed(turn->id);
}

void AiConversationModel::setPhase(AiPhase phase)
{
    if (!isBusy() || m_turns.last().phase == phase) return;
    m_turns.last().phase = phase;
    emit changed(m_turns.last().id);
}

// 先创建命令条目再交给执行器，待审批/跳过的命令也有稳定位置。
// 通过 toolCallId 挂到已记录的工具活动下；旧式文本命令没有调用 ID，保持为顶层条目。
quint64 AiConversationModel::addCommand(AiCommand command)
{
    if (!isBusy()) return 0;
    AiConversationEntry item;
    item.id = m_nextId++;
    item.kind = AiConversationEntry::Command;
    command.stepId = item.id;
    item.command = command;
    for (const auto &parent : m_turns.last().entries)
        if (parent.kind == AiConversationEntry::Activity && !command.toolCallId.isEmpty()
            && parent.activity.callId == command.toolCallId) { item.parentEntryId = parent.id; break; }
    m_turns.last().entries.append(item);
    emit changed(m_turns.last().id);
    return item.id;
}

void AiConversationModel::setCommandState(quint64 id, AiStepState state, const QString &reason)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(id, &turn);
    if (!e) return;
    e->state = state;
    e->text = reason;
    ++e->revision;
    updateCommandActivity(*turn, e->parentEntryId);
    emit changed(turn->id);
}

void AiConversationModel::setCommandResult(const AiCommandResult &result)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(result.stepId, &turn);
    if (!e) return;
    // 最终捕获结果替换实时预览，不能再追加，否则输出会重复。
    // 负退出码优先判为未知；allowFailure 只影响已知的非零退出。
    e->result = result;
    if (e->livePreviewLimited) { e->text.clear(); e->livePreviewLimited = false; }
    e->state = !result.resultKnown || result.exitCode < 0 ? AiStepState::Unknown
        : result.exitCode == 0 ? AiStepState::Succeeded
        : result.allowFailure ? AiStepState::AllowedFailure : AiStepState::Failed;
    ++e->revision;
    updateCommandActivity(*turn, e->parentEntryId);
    emit changed(turn->id);
}

void AiConversationModel::appendCommandOutput(quint64 id, const QString &text)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(id, &turn);
    if (!e || e->state != AiStepState::Running || e->livePreviewLimited) return;
    // 只限制界面的实时预览，避免长输出持续复制并触发布局；单位是 QString 字符数。
    // 命令完成后 setCommandResult 会用执行器捕获的结果整体替换此预览。
    constexpr int previewLimit = 512 * 1024;
    const int remaining = qMax(0, previewLimit - int(e->result.stdoutText.size()));
    e->result.stdoutText += text.left(remaining);
    if (text.size() > remaining) {
        e->livePreviewLimited = true;
        e->text = tr("实时预览较长，已暂停刷新；命令结束后将显示捕获结果。");
    }
    ++e->revision;
    emit changed(turn->id);
}

void AiConversationModel::addNotice(const QString &text)
{
    if (!isBusy() || text.isEmpty()) return;
    AiConversationEntry item;
    item.id = m_nextId++;
    item.kind = AiConversationEntry::Notice;
    item.text = text;
    m_turns.last().entries.append(item);
    emit changed(m_turns.last().id);
}

// 外部生产者可能重复上报同一活动，非空 callId 在本轮内保持幂等。
// 父活动必须先出现；这里只建立 ID 关联，不生成虚构的父节点或推断执行事件。
quint64 AiConversationModel::beginActivity(const AiActivity &activity, AiStepState state)
{
    if (!isBusy()) return 0;
    AiConversationEntry item;
    for (const auto &parent : m_turns.last().entries) {
        if (parent.kind != AiConversationEntry::Activity) continue;
        if (!activity.callId.isEmpty() && parent.activity.callId == activity.callId) return parent.id;
        if (!activity.parentCallId.isEmpty() && parent.activity.callId == activity.parentCallId) item.parentEntryId = parent.id;
    }
    item.id = m_nextId++;
    item.kind = AiConversationEntry::Activity;
    item.activity = activity;
    item.state = state;
    m_turns.last().entries.append(item);
    emit changed(m_turns.last().id);
    return item.id;
}

void AiConversationModel::updateActivity(quint64 id, AiStepState state, const QString &output, qint64 durationMs)
{
    AiConversationTurn *turn = nullptr;
    auto *e = entry(id, &turn);
    // 结束/取消后的迟到事件不能重新激活本轮，也不能覆盖已经落定的活动状态。
    if (!e || e->kind != AiConversationEntry::Activity || turn->phase == AiPhase::Finished) return;
    e->state = state;
    e->activity.output = output;
    e->activity.durationMs = durationMs;
    ++e->revision;
    emit changed(turn->id);
}

void AiConversationModel::updateCommandActivity(AiConversationTurn &turn, quint64 parentId)
{
    if (!parentId || turn.phase == AiPhase::Finished) return;
    auto *parent = entry(parentId);
    if (!parent || parent->kind != AiConversationEntry::Activity) return;
    bool running = false, pending = false, failed = false, unknown = false, allowed = false, cancelled = false, skipped = false;
    qint64 duration = 0;
    for (const auto &child : turn.entries) {
        if (child.parentEntryId != parentId || child.kind != AiConversationEntry::Command) continue;
        running |= child.state == AiStepState::Running;
        pending |= child.state == AiStepState::Pending;
        failed |= child.state == AiStepState::Failed;
        unknown |= child.state == AiStepState::Unknown;
        allowed |= child.state == AiStepState::AllowedFailure;
        cancelled |= child.state == AiStepState::Cancelled;
        skipped |= child.state == AiStepState::Skipped;
        duration += child.result.durationMs;
    }
    // 父活动由子命令的真实状态汇总：运行中优先，其次是未知/失败。
    // 有允许失败的命令时保留该提示，不能仅因批次已结束就显示为全部成功。
    parent->state = running ? AiStepState::Running : unknown ? AiStepState::Unknown
        : failed ? AiStepState::Failed : pending ? AiStepState::Pending
        : cancelled ? AiStepState::Cancelled : skipped ? AiStepState::Skipped
        : allowed ? AiStepState::AllowedFailure : AiStepState::Succeeded;
    parent->activity.durationMs = duration;
    ++parent->revision;
}

// 活动未收到明确结束事件时不能默认为成功，结束轮次后收拢为取消/未知。
// 命令条目的真实结果另由控制器填写，晚到结果仍可补记，但不会改写本轮结论。
void AiConversationModel::finishTurn(AiTurnOutcome outcome, const QString &text, bool localReport)
{
    if (!isBusy()) return;
    auto &turn = m_turns.last();
    for (auto &item : turn.entries) {
        if (item.kind != AiConversationEntry::Activity) continue;
        if (item.state == AiStepState::Running || item.state == AiStepState::Pending) {
            item.state = outcome == AiTurnOutcome::Cancelled ? AiStepState::Cancelled : AiStepState::Unknown;
            ++item.revision;
        }
    }
    turn.phase = AiPhase::Finished;
    turn.outcome = outcome;
    turn.finalText = text;
    turn.localReport = localReport;
    turn.durationMs = turn.clock.elapsed();
    emit changed(turn.id);
}

QString AiConversationModel::factualSummary(const QString &reason) const
{
    QString text = QStringLiteral("%1\n\n").arg(reason);
    const auto *turn = currentTurn();
    if (!turn) return text;
    int commands = 0;
    for (const auto &e : turn->entries) {
        if (e.kind != AiConversationEntry::Command) continue;
        ++commands;
        QString label = aiStepLabel(e.state);
        if (e.state == AiStepState::Failed || e.state == AiStepState::AllowedFailure)
            label += QStringLiteral("（exit %1）").arg(e.result.exitCode);
        // 用缩进代码块保护命令中的 Markdown 符号，多行命令的每一行都要缩进。
        text += QStringLiteral("- %1%2\n\n    %3\n\n")
            .arg(label, e.command.description.isEmpty() ? QString() : QStringLiteral(" · ") + e.command.description,
                 e.command.cmd.split(QLatin1Char('\n')).join(QStringLiteral("\n    ")));
    }
    if (!commands) text += QStringLiteral("本轮没有已执行的命令。\n\n");
    text += QStringLiteral("以上是本地记录的执行情况，不代表用户目标已通过验证。可根据这些结果继续询问，避免重复执行已有操作。");
    return text;
}

void AiConversationModel::setProcessCollapsed(quint64 id, bool collapsed)
{
    for (auto &turn : m_turns) if (turn.id == id) {
        turn.processCollapsed = collapsed;
        emit changed(id);
        return;
    }
}

void AiConversationModel::setEntryExpanded(quint64 id, quint64 entryId, bool expanded)
{
    for (auto &turn : m_turns) if (turn.id == id) {
        if (expanded) turn.expandedEntries.insert(entryId);
        else turn.expandedEntries.remove(entryId);
        emit changed(id);
        return;
    }
}

void AiConversationModel::clear()
{
    if (isBusy()) return;
    m_turns.clear();
    emit reset();
}

} // namespace cubeshell
