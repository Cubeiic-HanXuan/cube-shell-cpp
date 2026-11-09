// SshAiAgent.cpp — 请求 → 审批 → 执行 → 回传 → 验证/诊断的任务调度。
// 每批工具调用必须先补齐结果再请求模型；达到上限后只总结，结果未知时停止自动执行。

#include "SshAiAgent.h"
#include "SensitiveDataMasker.h"
#include "TerminalExecutor.h"
#include "ServerProfileBuilder.h"
#include "ssh/CommandExecutor.h"
#include <algorithm>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace cubeshell {

AiCommandExecThread::AiCommandExecThread(CommandExecutor *executor, TerminalExecutor *terminal,
                                         const QList<AiCommand> &commands, QObject *parent,
                                         AiCommandRunner runner)
    : QThread(parent), m_executor(executor), m_terminalExecutor(terminal),
      m_commands(commands), m_runner(std::move(runner)) {}

void AiCommandExecThread::requestStop()
{
    m_stopFlag.store(true);
    if (m_terminalExecutor) m_terminalExecutor->cancel();
    if (m_executor) m_executor->cancel();
}

void AiCommandExecThread::run()
{
    QList<AiCommandResult> results;
    // 只有开头不带选项的 sudo 才能交给旧 sudoExec 辅助函数拆除再包装。
    // grep/echo 参数中的 sudo 是数据；-u/-n/-i 等选项不能剥掉，否则会改变已批准的身份或语义。
    static const QRegularExpression leadingSudoRe(QStringLiteral("^[ \\t]*sudo[ \\t]+(?=[^ \\t-])"));
    for (int i = 0; i < m_commands.size(); ++i) {
        if (m_stopFlag.load()) break;
        const auto &command = m_commands.at(i);
        // 审批不能覆盖禁止规则，真正调用执行后端前再校验一次。
        if (!CommandSafetyChecker().check(command.cmd).isAllowed) break;
        emit progress(i + 1, m_commands.size());
        emit commandStarted(command);
        QElapsedTimer timer;
        timer.start();
        AiCommandResult result;
        const int timeout = CommandExecutor::isLongRunningCommand(command.cmd)
            ? CommandExecutor::kLongRunningTimeoutMs : CommandExecutor::kNormalStreamTimeoutMs;
        // 执行适配器优先，其次使用已绑定的用户终端，最后回退独立 exec 通道。
        // 终端输出已合并 stdout/stderr，不能伪造两路分离结果。
        if (m_runner) {
            result = m_runner(command);
        } else if (m_terminalExecutor) {
            const auto value = m_terminalExecutor->runBlocking(command.cmd, timeout, &m_stopFlag);
            result.exitCode = value.first;
            result.stdoutText = value.second;
        } else if (m_executor) {
            ExecResult value;
            const auto leadingSudo = leadingSudoRe.match(command.cmd);
            if (leadingSudo.hasMatch()) {
                value = m_executor->sudoExec(command.cmd.mid(leadingSudo.capturedLength()), false, timeout);
            } else value = m_executor->exec(command.cmd, false, timeout);
            result.exitCode = value.exitCode;
            result.stdoutText = value.stdoutText;
            result.stderrText = value.stderrText;
            if (!value.errorMessage.isEmpty() || value.timedOut) {
                result.exitCode = -1;
                result.stderrText += value.timedOut ? QStringLiteral("\n命令执行超时") : value.errorMessage;
            }
        } else {
            result.stderrText = QStringLiteral("未绑定 SSH 连接，无法执行命令");
        }
        result.cmd = command.cmd;
        result.description = command.description;
        result.allowFailure = command.allowFailure;
        result.stepId = command.stepId;
        result.toolCallId = command.toolCallId;
        result.resultKnown = result.exitCode >= 0;
        result.durationMs = timer.elapsed();
        results.append(result);
        emit commandFinished(result);
        if (!result.resultKnown || (result.exitCode != 0 && !result.allowFailure)) break;
    }
    emit allFinished(results);
}

SshAiAgent::SshAiAgent(CommandExecutor *executor, const AiPreferences &prefs,
                       QObject *parent, AiCommandRunner runner)
    : QObject(parent), m_executor(executor), m_prefs(prefs), m_aiWorker(new AiChatWorker(this)),
      m_conversation(QString(), ChatHistory::kMaxHistoryRounds),
      m_terminalExecutor(new TerminalExecutor(this)), m_display(new AiConversationModel(this)),
      m_runner(std::move(runner))
{
    qRegisterMetaType<AiCommand>();
    qRegisterMetaType<AiCommandResult>();
    qRegisterMetaType<QList<AiCommand>>();
    qRegisterMetaType<QList<AiCommandResult>>();
    m_conversation.setSystemPrompt(buildSystemPrompt());
    connect(m_aiWorker, &AiChatWorker::deltaReceived, this,
            [this](const QString &text, const QString &reasoning) {
        if (m_cancelled || !m_display->isBusy() || m_requestId != m_aiWorker->requestId()) return;
        m_display->appendDelta(m_messageId, text, reasoning);
        emit aiMessage(reasoning, text);
    });
    connect(m_aiWorker, &AiChatWorker::requestFinished, this, &SshAiAgent::onAiCompleted);
    connect(m_terminalExecutor, &TerminalExecutor::outputReceived, this, [this](const QString &text) {
        if (!m_cancelled && m_runningStep) m_display->appendCommandOutput(m_runningStep, text);
        emit commandOutput(text);
    });
}

SshAiAgent::~SshAiAgent()
{
    shutdown();
    // 探测器持有执行器裸指针；先销毁探测器并等待探测结束，再让 QObject 回收子执行器。
    qDeleteAll(findChildren<ServerProfileBuilder *>(QString(), Qt::FindDirectChildrenOnly));
}
void SshAiAgent::setPreferences(const AiPreferences &prefs) { m_prefs = prefs; }
void SshAiAgent::setTerminal(QTermWidget *terminal) { m_terminalExecutor->setTerminal(terminal); }
void SshAiAgent::setExecutor(CommandExecutor *executor) { if (!m_execThread) m_executor = executor; }
void SshAiAgent::setServerProfile(const QString &profile) { m_serverProfile = profile; }

void SshAiAgent::processUserInput(const QString &input)
{
    if (input.trimmed().isEmpty() || isBusy()) return;
    ++m_generation;
    m_cancelled = false;
    m_finalizing = false;
    m_goalVerifyCount = 0;
    m_diagnoseCount = 0;
    m_originalGoal = input;
    m_pendingCommands.clear();
    m_batchCommands.clear();
    m_pendingToolCalls = {};
    m_display->beginTurn(input, m_prefs.model);
    m_conversation.setSystemPrompt(buildSystemPrompt());
    m_conversation.addUserMessage(input);
    startRequest(AiPhase::Waiting);
}

void SshAiAgent::startRequest(AiPhase phase)
{
    if (m_cancelled || !m_display->isBusy()) return;
    m_display->setPhase(phase);
    m_messageId = m_display->beginMessage();
    m_aiWorker->setPreferences(m_prefs);
    m_aiWorker->setTools(m_finalizing ? QJsonArray() : toolsDefinition());
    m_aiWorker->setSummaryOnly(m_finalizing);
    // start 可能因缺少 API Key 同步失败；必须预先记下下一请求 ID，完成回调才不会被误丢弃。
    m_requestId = m_aiWorker->requestId() + 1;
    emit thinkingStarted();
    m_aiWorker->start(m_conversation.buildMessages());
}

void SshAiAgent::requestFollowup(const QString &prompt, AiPhase phase)
{
    m_conversation.addUserMessage(prompt);
    startRequest(phase);
}

// 只接受当前请求结果。空回复/截断可尝试一次只读总结；其他失败直接用本地事实收尾。
// 总结分支先于工具分支，确保服务端即使意外返回工具调用也不会继续执行。
void SshAiAgent::onAiCompleted(const AiRequestResult &result)
{
    if (result.requestId != m_requestId || m_cancelled || !m_display->isBusy()) return;
    emit thinkingFinished();
    m_display->finalizeMessage(m_messageId, result.text);
    if (result.outcome != AiRequestOutcome::Complete) {
        if (!result.text.isEmpty()) m_conversation.addAssistantMessage(result.text);
        if (!m_finalizing && (result.outcome == AiRequestOutcome::Empty || result.outcome == AiRequestOutcome::Truncated)) {
            finalizeTask(result.error);
        } else {
            finishLocally(result.outcome == AiRequestOutcome::Cancelled ? AiTurnOutcome::Cancelled : AiTurnOutcome::Failed,
                          result.error + QStringLiteral("。未取得完整的模型报告。"));
        }
        return;
    }
    if (m_finalizing) {
        if (!result.toolCalls.isEmpty() || result.text.trimmed().isEmpty()) {
            finishLocally(AiTurnOutcome::Incomplete, tr("模型没有给出纯文本总结，已停止追加操作。"));
        } else {
            m_display->finalizeMessage(m_messageId, result.text, true);
            finishTurn(AiTurnOutcome::Incomplete, result.text);
        }
        return;
    }
    if (!result.toolCalls.isEmpty()) {
        QString error;
        auto commands = parseToolCalls(result.toolCalls, &error);
        if (!error.isEmpty()) {
            finishLocally(AiTurnOutcome::Failed, error + tr("；未执行本批命令。"));
            return;
        }
        // 协议历史保留每个调用的原始身份；包括跳过项在内，所有调用都有对应结果后
        // 才能再次请求模型，避免产生未闭合的 assistant/tool 消息序列。
        m_conversation.addAssistantToolCalls(result.text, m_pendingToolCalls);
        for (const auto &value : m_pendingToolCalls) {
            const auto call = value.toObject();
            const auto function = call.value(QStringLiteral("function")).toObject();
            AiActivity activity;
            activity.callId = call.value(QStringLiteral("id")).toString();
            activity.name = function.value(QStringLiteral("name")).toString();
            const auto args = QJsonDocument::fromJson(function.value(QStringLiteral("arguments")).toString().toUtf8());
            activity.input = QString::fromUtf8(args.toJson(QJsonDocument::Indented));
            activity.summary = tr("SSH 执行 · %1 个命令").arg(args.object().value(QStringLiteral("commands")).toArray().size());
            activity.outputLanguage = QStringLiteral("json");
            m_display->beginActivity(activity);
        }
        m_pendingCommands = checkCommandsSafety(commands);
    } else {
        m_conversation.addAssistantMessage(result.text);
        // 进入执行后的验证/诊断阶段，正文代码块属于报告，不再作为新命令提取。
        // 只有尚未验证/诊断的初始回复兼容旧式文本工具，后续操作必须显式调用工具。
        m_pendingCommands = (m_goalVerifyCount == 0 && m_diagnoseCount == 0)
            ? checkCommandsSafety(extractCommandsFromText(result.text)) : QList<AiCommand>();
        if (m_pendingCommands.isEmpty()) {
            m_display->finalizeMessage(m_messageId, result.text, true);
            finishTurn(AiTurnOutcome::Complete, result.text);
            return;
        }
    }
    for (auto &command : m_pendingCommands) command.stepId = m_display->addCommand(command);
    m_batchCommands = m_pendingCommands;
    m_display->setPhase(AiPhase::AwaitingApproval);
    emit commandReady(m_pendingCommands);
}

// 整批校验通过后才提交 m_pendingToolCalls，任一未知工具、重复 ID 或空命令使整批失败。
// 缺失 ID 时生成本地 ID 并同时写入协议历史与命令，保证结果回填仍能一一关联。
QList<AiCommand> SshAiAgent::parseToolCalls(const QJsonArray &calls, QString *error)
{
    QList<AiCommand> commands;
    QJsonArray wireCalls;
    QSet<QString> ids;
    QStringList explanations;
    for (const auto &value : calls) {
        const auto call = value.toObject();
        const QString name = call.value(QStringLiteral("name")).toString();
        QString id = call.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) id = QStringLiteral("call_") + QUuid::createUuid().toString(QUuid::Id128);
        QJsonParseError parseError{};
        const QString arguments = call.value(QStringLiteral("arguments")).toString();
        const auto doc = QJsonDocument::fromJson(arguments.toUtf8(), &parseError);
        if (ids.contains(id) || name != QLatin1String("execute_ssh_commands") || !doc.isObject()
            || parseError.error != QJsonParseError::NoError) {
            *error = tr("模型返回了无效的工具调用或参数");
            return {};
        }
        ids.insert(id);
        const auto array = doc.object().value(QStringLiteral("commands")).toArray();
        if (array.isEmpty()) { *error = tr("工具调用中的命令列表为空"); return {}; }
        for (const auto &commandValue : array) {
            const auto obj = commandValue.toObject();
            AiCommand command;
            command.cmd = obj.value(QStringLiteral("cmd")).toString();
            command.description = obj.value(QStringLiteral("description")).toString();
            command.allowFailure = obj.value(QStringLiteral("allow_failure")).toBool(false);
            command.interactive = obj.value(QStringLiteral("interactive")).toBool(true);
            command.toolCallId = id;
            if (command.cmd.trimmed().isEmpty()) { *error = tr("工具调用包含空命令"); return {}; }
            commands.append(command);
        }
        const QString explanation = doc.object().value(QStringLiteral("explanation")).toString();
        if (!explanation.isEmpty()) explanations.append(explanation);
        wireCalls.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), arguments}}}});
    }
    m_pendingToolCalls = wireCalls;
    if (!explanations.isEmpty()) m_display->addNotice(explanations.join(QLatin1Char('\n')));
    return commands;
}

// 审批只决定取待执行列表中的哪些项，实际执行数据重新从原候选项取出。
// 同时匹配 ID 与原命令并拒绝重复 ID，防止旧审批或被改写的参数作用于当前批次。
void SshAiAgent::executeCommands(const QList<AiCommand> &approved)
{
    if (m_cancelled || m_finalizing || m_execThread || m_pendingCommands.isEmpty()) return;
    QList<AiCommand> commands;
    QSet<quint64> ids;
    for (const auto &requested : approved) {
        auto it = std::find_if(m_pendingCommands.cbegin(), m_pendingCommands.cend(), [&](const AiCommand &c) {
            return c.stepId == requested.stepId && c.cmd == requested.cmd;
        });
        if (it == m_pendingCommands.cend() || ids.contains(it->stepId)) return; // 过期或重复审批，整次拒绝
        ids.insert(it->stepId);
        if (m_safetyChecker.check(it->cmd).isAllowed) commands.append(*it);
        else m_display->setCommandState(it->stepId, AiStepState::Skipped, tr("安全规则禁止执行"));
    }
    for (const auto &command : m_pendingCommands)
        if (!ids.contains(command.stepId)) m_display->setCommandState(command.stepId, AiStepState::Skipped, tr("未获批准"));
    m_pendingCommands.clear();
    if (commands.isEmpty()) {
        closeToolBatch();
        finishLocally(AiTurnOutcome::Incomplete, tr("没有获准执行的命令。"));
        return;
    }
    m_display->setPhase(AiPhase::Executing);
    auto *thread = new AiCommandExecThread(m_executor,
        m_terminalExecutor->terminal() ? m_terminalExecutor : nullptr, commands, this, m_runner);
    m_execThread = thread;
    const quint64 generation = m_generation;
    connect(thread, &AiCommandExecThread::commandStarted, this, [this, generation](const AiCommand &command) {
        if (generation != m_generation || m_cancelled) return;
        m_runningStep = command.stepId;
        m_display->setCommandState(command.stepId, AiStepState::Running);
        emit executionStarted(command.cmd);
    }, Qt::QueuedConnection);
    connect(thread, &AiCommandExecThread::commandFinished, this, [this, generation](const AiCommandResult &result) {
        // 迟到的真实结果仍写回原 stepId；但取消/过期后不再触发当前任务的业务信号。
        m_display->setCommandResult(result);
        if (generation != m_generation || m_cancelled) return;
        m_runningStep = 0;
        emit executionFinished(result);
    }, Qt::QueuedConnection);
    connect(thread, &AiCommandExecThread::progress, this, &SshAiAgent::executionProgress, Qt::QueuedConnection);
    connect(thread, &QThread::finished, this, [this, thread, generation] {
        if (m_execThread == thread) m_execThread = nullptr;
        thread->deleteLater();
        if (generation == m_generation && !m_cancelled) onAllExecFinished();
        else emit executionIdle(); // 只解除界面忙碌限制，不再发送第二次任务完成通知
    });
    thread->start();
}

// 从展示模型读取已落定事实，按 toolCallId 分组回填；未知退出码写 JSON null，不伪装成失败码。
// 工具输出先脱敏、去 ANSI 并截断，用户界面的单命令行仍保留执行器实际捕获的内容。
void SshAiAgent::closeToolBatch()
{
    const auto *turn = m_display->currentTurn();
    if (!turn) return;
    for (const auto &callValue : m_pendingToolCalls) {
        const QString id = callValue.toObject().value(QStringLiteral("id")).toString();
        QJsonArray results;
        for (const auto &entry : turn->entries) {
            if (entry.kind != AiConversationEntry::Command || entry.command.toolCallId != id) continue;
            results.append(QJsonObject{{QStringLiteral("command"), SensitiveDataMasker::mask(entry.command.cmd)},
                {QStringLiteral("status"), aiStepLabel(entry.state)},
                {QStringLiteral("exit_code"), entry.result.resultKnown && entry.result.exitCode >= 0
                    ? QJsonValue(entry.result.exitCode) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("stdout"), ChatHistory::truncateOutput(ChatHistory::stripAnsi(SensitiveDataMasker::mask(entry.result.stdoutText)))},
                {QStringLiteral("stderr"), ChatHistory::truncateOutput(ChatHistory::stripAnsi(SensitiveDataMasker::mask(entry.result.stderrText)))},
                {QStringLiteral("note"), entry.text}});
        }
        m_conversation.addToolResult(id, QString::fromUtf8(QJsonDocument(results).toJson(QJsonDocument::Compact)));
        // 工具活动展示实际发给模型的脱敏结果，与输入参数并列；单条命令保留捕获输出。
        // 遍历快照，因为 updateActivity 会修改条目并发出通知，不持有原容器迭代器。
        const auto entries = turn->entries;
        for (const auto &entry : entries)
            if (entry.kind == AiConversationEntry::Activity && entry.activity.callId == id)
                m_display->updateActivity(entry.id, entry.state,
                    QString::fromUtf8(QJsonDocument(results).toJson(QJsonDocument::Indented)), entry.activity.durationMs);
    }
    // 旧式文本命令没有工具调用 ID，仍以文本消息回传实际结果，不能生成无关联的 tool 消息。
    for (const auto &command : m_batchCommands) {
        if (!command.toolCallId.isEmpty()) continue;
        for (const auto &entry : turn->entries) if (entry.id == command.stepId) {
            m_conversation.addCommandResult(command.cmd, entry.result.stdoutText, entry.result.stderrText,
                                            entry.result.exitCode, aiStepLabel(entry.state));
        }
    }
    m_pendingToolCalls = {};
    m_batchCommands.clear();
}

// 执行线程退出后统一收尾：未运行项标跳过，先补齐工具结果，再决定诊断或验证。
// 未知状态不能安全重试，直接停止自动操作；已知失败才可在限次内请求诊断。
void SshAiAgent::onAllExecFinished()
{
    if (!m_display->isBusy()) return;
    bool failed = false;
    bool unknown = false;
    for (const auto &command : m_batchCommands) {
        const auto entries = m_display->currentTurn()->entries;
        for (const auto &entry : entries) if (entry.id == command.stepId) {
            if (entry.state == AiStepState::Pending)
                m_display->setCommandState(entry.id, AiStepState::Skipped, tr("前序命令未成功，后续未执行"));
            failed |= entry.state == AiStepState::Failed;
            unknown |= entry.state == AiStepState::Unknown;
        }
    }
    closeToolBatch();
    if (unknown) { finishLocally(AiTurnOutcome::Incomplete, tr("未能确认命令退出状态；为避免重复操作，已停止自动执行。")); return; }
    if (failed) {
        if (++m_diagnoseCount > kMaxDiagnoseRounds) { finalizeTask(tr("自动诊断已达到轮次上限")); return; }
        emit diagnosingStarted();
        requestFollowup(tr("部分命令失败，结果已回传。请分析原因；如需修复请使用工具。原始任务：%1").arg(m_originalGoal), AiPhase::Diagnosing);
    } else if (m_goalVerifyCount < kMaxVerifyRounds) {
        ++m_goalVerifyCount;
        verifyGoalCompletion();
    } else finalizeTask(tr("自动验证已达到轮次上限，不能据此认定目标已完成"));
}

void SshAiAgent::verifyGoalCompletion()
{
    requestFollowup(tr("命令执行结果已回传。请判断原始任务是否真正完成：%1\n"
                       "必要时用工具验证；如已有足够证据，请直接给出最终报告，说明结果和未验证项。")
                        .arg(m_originalGoal), AiPhase::Verifying);
}

void SshAiAgent::diagnoseError(const QString &command, const QString &out, const QString &err, int code)
{
    if (!m_display->isBusy() || m_cancelled || m_aiWorker->isRunning() || m_execThread) return;
    m_conversation.addCommandResult(command, out, err, code);
    if (++m_diagnoseCount > kMaxDiagnoseRounds) finalizeTask(tr("自动诊断已达到轮次上限"));
    else requestFollowup(tr("请根据刚才的失败结果诊断原始任务：%1").arg(m_originalGoal), AiPhase::Diagnosing);
}

// 达到轮次上限或回复不完整时只追加一次总结请求；再次失败交给本地摘要，不能无限总结。
// m_finalizing 同时控制请求工具字段和后续审批入口，提示词本身不是唯一限制。
void SshAiAgent::finalizeTask(const QString &reason)
{
    if (m_finalizing) { finishLocally(AiTurnOutcome::Incomplete, reason); return; }
    m_finalizing = true;
    m_display->addNotice(reason);
    requestFollowup(tr("%1。现在停止执行任何命令，仅根据已有执行记录提供最终报告。\n"
                       "原始任务：%2\n说明已完成的操作、关键结果、失败/跳过项及尚未验证的事项。"
                       "不要把命令退出成功等同于目标已完成，不要再调用工具。")
                        .arg(reason, m_originalGoal), AiPhase::Finalizing);
}

void SshAiAgent::finishTurn(AiTurnOutcome outcome, const QString &text, bool local)
{
    if (!m_display->isBusy()) return;
    // 普通正文已在完成分支写入协议历史；总结和本地报告尚未写入，此处补入一次。
    if (local || m_finalizing) m_conversation.addAssistantMessage(SensitiveDataMasker::mask(text));
    m_pendingCommands.clear();
    m_finalizing = false;
    m_originalGoal.clear();
    m_goalVerifyCount = 0;
    m_diagnoseCount = 0;
    m_display->finishTurn(outcome, text, local);
    emit turnFinished();
}

void SshAiAgent::finishLocally(AiTurnOutcome outcome, const QString &reason)
{
    finishTurn(outcome, m_display->factualSummary(reason), true);
}

void SshAiAgent::rejectPendingCommands(const QString &reason)
{
    if (m_pendingCommands.isEmpty() || m_cancelled) return;
    const QString message = reason.isEmpty() ? tr("用户取消了命令执行") : reason;
    for (const auto &command : m_pendingCommands)
        m_display->setCommandState(command.stepId, AiStepState::Cancelled, message);
    m_pendingCommands.clear();
    closeToolBatch();
    finishLocally(AiTurnOutcome::Cancelled, message);
}

void SshAiAgent::stop()
{
    cancelTask(AiTurnOutcome::Cancelled, tr("已停止 AI 后续操作。停止等待不代表远端进程已终止，请在终端确认。"));
}

void SshAiAgent::cancelTask(AiTurnOutcome outcome, const QString &reason)
{
    if (!m_display->isBusy() && !m_execThread) return;
    // 中止传输可能同步触发 requestFinished，必须先封住调度入口并使旧线程回调过期。
    m_cancelled = true;
    ++m_generation;
    m_aiWorker->stop();
    if (m_execThread) m_execThread->requestStop();
    m_terminalExecutor->cancel();
    if (const auto *turn = m_display->currentTurn()) {
        const auto entries = turn->entries;
        for (const auto &entry : entries) if (entry.kind == AiConversationEntry::Command) {
            if (entry.state == AiStepState::Pending) m_display->setCommandState(entry.id, AiStepState::Cancelled);
            else if (entry.state == AiStepState::Running) m_display->setCommandState(entry.id, AiStepState::Unknown);
        }
    }
    closeToolBatch();
    m_runningStep = 0;
    finishLocally(outcome, reason);
}

void SshAiAgent::connectionLost()
{
    cancelTask(AiTurnOutcome::Incomplete, tr("SSH 连接中断，未能确认的命令结果已标记为未知。请重连后核实远端状态，不要直接重跑命令。"));
    m_terminalExecutor->resetConnection();
}

// waitMs 只用于首次等待，超时后仍需 join；线程引用执行器，不能遗留到对象销毁之后。
void SshAiAgent::shutdown(int waitMs)
{
    stop();
    if (m_execThread && !m_execThread->wait(waitMs)) m_execThread->wait();
}

void SshAiAgent::clearConversation()
{
    if (isBusy()) return;
    m_display->clear();
    m_conversation.clear();
    m_conversation.setSystemPrompt(buildSystemPrompt());
}

QJsonArray SshAiAgent::toolsDefinition()
{
    return QJsonDocument::fromJson(R"JSON([
      {"type":"function","function":{
        "name":"execute_ssh_commands",
        "description":"在远程 SSH 服务器执行一组命令。根据当前用户权限决定是否需要 sudo。",
        "parameters":{"type":"object","properties":{
          "commands":{"type":"array","items":{"type":"object","properties":{
            "cmd":{"type":"string","description":"完整 shell 命令，需超级权限时使用 sudo"},
            "description":{"type":"string","description":"命令用途"},
            "allow_failure":{"type":"boolean","description":"允许非零退出后继续"},
            "interactive":{"type":"boolean","description":"在用户终端执行，默认 true"}
          },"required":["cmd","description"]}},
          "explanation":{"type":"string","description":"整体方案说明"}
        },"required":["commands","explanation"]}
      }}
    ])JSON").array();
}

QList<AiCommand> SshAiAgent::extractCommandsFromText(const QString &text)
{
    QList<AiCommand> commands;
    static const QRegularExpression blockRe(QStringLiteral("```(?:bash|shell|sh)\\s*\\n(.*?)```"),
                                             QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression cjkRe(QStringLiteral("[\\x{4e00}-\\x{9fff}]"));
    static const QRegularExpression orderedListRe(QStringLiteral("^\\d+[\\.\\)]\\s"));
    auto it = blockRe.globalMatch(text);
    while (it.hasNext()) {
        for (const auto &raw : it.next().captured(1).trimmed().split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1String("- "))
                || line.startsWith(QLatin1String("* ")) || line.startsWith(QLatin1String("+ "))
                || line.startsWith(QLatin1String("> ")) || line == QLatin1String("-")
                || line == QLatin1String("*") || line == QLatin1String("+")
                || orderedListRe.match(line).hasMatch() || cjkRe.match(line).hasMatch()) continue;
            AiCommand command;
            command.cmd = line;
            command.description = QStringLiteral("从 AI 回复中提取的命令");
            commands.append(command);
        }
    }
    return commands;
}

QList<AiCommand> SshAiAgent::checkCommandsSafety(const QList<AiCommand> &commands) const
{
    QList<AiCommand> checked;
    for (auto command : commands) {
        command.safety = m_safetyChecker.check(command.cmd);
        checked.append(command);
    }
    return checked;
}

QString SshAiAgent::buildSystemPrompt() const
{
    QString prompt = QStringLiteral(
        "你是专业的 Linux 运维助手，帮助用户完成系统管理、服务部署、故障诊断等任务。\n"
        "根据用户需求生成安全的命令方案，操作请求使用 execute_ssh_commands 工具；解释问题直接用文本回复。\n"
        "禁止导致数据丢失的危险操作；涉及服务重启、关机先告知风险，修改配置前建议备份。"
        "优先使用只读命令获取信息，不确定时解释方案等待确认。\n"
        "工具结果会回传实际退出码与输出。命令成功退出不代表用户目标已完成。"
        "获得足够证据后请给出最终结果报告，区分已验证、失败和未完成项。"
        "最终报告中不要重新提供待执行的命令批次。\n"
        "每条命令提供用途说明；允许失败的检查命令设置 allow_failure。"
        "根据当前用户权限决定是否添加 sudo。不要调起分页器：systemctl/journalctl 使用 --no-pager，"
        "git 使用 --no-pager，避免 less/more/man。\n");
    if (!m_serverProfile.isEmpty()) prompt += QStringLiteral("\n") + m_serverProfile;
    return prompt;
}

} // namespace cubeshell
