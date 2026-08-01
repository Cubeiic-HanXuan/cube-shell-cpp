// SshAiAgent.cpp — see SshAiAgent.h.
//
// 对应Python: core/ai/ssh_agent.py

#include "SshAiAgent.h"

#include "ssh/CommandExecutor.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace cubeshell {

namespace {

void registerAiMetaTypes()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    qRegisterMetaType<AiCommand>("cubeshell::AiCommand");
    qRegisterMetaType<AiCommandResult>("cubeshell::AiCommandResult");
    qRegisterMetaType<QList<AiCommand>>("QList<cubeshell::AiCommand>");
    qRegisterMetaType<QList<AiCommandResult>>("QList<cubeshell::AiCommandResult>");
}

} // namespace

// ---------------------------------------------------------------------------
// AiCommandExecThread
// ---------------------------------------------------------------------------

AiCommandExecThread::AiCommandExecThread(CommandExecutor *executor,
                                         const QList<AiCommand> &commands,
                                         QObject *parent)
    : QThread(parent)
    , m_executor(executor)
    , m_commands(commands)
{
}

// 对应Python: _CommandExecThread.request_stop
void AiCommandExecThread::requestStop()
{
    m_stopFlag.store(true);
    if (m_executor)
        m_executor->cancel();
}

// 对应Python: _CommandExecThread.run
void AiCommandExecThread::run()
{
    QList<AiCommandResult> results;
    const int total = m_commands.size();

    static const QRegularExpression sudoRe(QStringLiteral("\\bsudo\\b"));
    static const QRegularExpression leadingSudoRe(
        QStringLiteral("^\\s*sudo\\s+(-\\S+\\s+)*"));

    for (int i = 0; i < total; ++i) {
        if (m_stopFlag.load())
            break;

        const AiCommand &cmdInfo = m_commands.at(i);
        emit progress(i + 1, total);
        emit commandStarted(cmdInfo.cmd);

        QElapsedTimer timer;
        timer.start();

        AiCommandResult result;
        result.cmd = cmdInfo.cmd;
        result.description = cmdInfo.description;
        result.allowFailure = cmdInfo.allowFailure;

        if (!m_executor) {
            result.exitCode = -1;
            result.stderrText = QStringLiteral(
                "(未绑定 SSH 连接，无法执行命令)");
        } else {
            // 超时选择 — 对应Python: timeout = 600 if long_running else 120
            const int timeoutMs =
                CommandExecutor::isLongRunningCommand(cmdInfo.cmd)
                    ? CommandExecutor::kLongRunningTimeoutMs
                    : CommandExecutor::kNormalStreamTimeoutMs;

            ExecResult exec;
            if (sudoRe.match(cmdInfo.cmd).hasMatch()) {
                // sudo 命令：剥离前导 sudo 后走 sudoExec（-S + stdin 密码）。
                // 对应Python: _needs_sudo_password + _inject_sudo_stdin_flag
                QString bare = cmdInfo.cmd;
                bare.remove(leadingSudoRe);
                exec = m_executor->sudoExec(bare, false, timeoutMs);
            } else {
                exec = m_executor->exec(cmdInfo.cmd, false, timeoutMs);
            }
            result.exitCode = exec.exitCode;
            result.stdoutText = exec.stdoutText;
            result.stderrText = exec.stderrText;
            if (!exec.errorMessage.isEmpty()) {
                result.exitCode = -1;
                if (result.stderrText.isEmpty())
                    result.stderrText = exec.errorMessage;
                emit execError(QStringLiteral("执行命令失败 [%1]: %2")
                                   .arg(cmdInfo.cmd, exec.errorMessage));
            } else if (exec.timedOut) {
                result.exitCode = -1;
                result.stderrText += QStringLiteral("\n(命令执行超时)");
            }
        }
        result.durationMs = timer.elapsed();

        results.append(result);
        emit commandFinished(result);

        // 失败且不允许失败 → 停止后续执行
        // 对应Python: if exit_code != 0 and not allow_failure: break
        if (result.exitCode != 0 && !result.allowFailure)
            break;
    }

    emit allFinished(results);
}

// ---------------------------------------------------------------------------
// SshAiAgent
// ---------------------------------------------------------------------------

SshAiAgent::SshAiAgent(CommandExecutor *executor, const AiPreferences &prefs,
                       QObject *parent)
    : QObject(parent)
    , m_executor(executor)
    , m_prefs(prefs)
    , m_aiWorker(new AiChatWorker(this))
    , m_conversation(QString(), ChatHistory::kMaxHistoryRounds)
{
    registerAiMetaTypes();

    m_conversation.setSystemPrompt(buildSystemPrompt());
    m_aiWorker->setPreferences(m_prefs);
    m_aiWorker->setTools(toolsDefinition());

    // AiChatWorker 与 agent 同线程，直接连接即可。
    connect(m_aiWorker, &AiChatWorker::deltaReceived,
            this, &SshAiAgent::onAiDelta);
    connect(m_aiWorker, &AiChatWorker::completed,
            this, &SshAiAgent::onAiCompleted);
    connect(m_aiWorker, &AiChatWorker::failed,
            this, &SshAiAgent::onAiFailed);
}

SshAiAgent::~SshAiAgent()
{
    shutdown();
}

void SshAiAgent::setPreferences(const AiPreferences &prefs)
{
    m_prefs = prefs;
    m_aiWorker->setPreferences(prefs);
}

// 对应Python: SSHAIAgent.process_user_input
void SshAiAgent::processUserInput(const QString &userInput)
{
    if (userInput.trimmed().isEmpty())
        return;

    // 只记录用户真实输入为原始目标（诊断/验证提示词不覆盖）
    if (!m_isDiagnosing) {
        m_originalGoal = userInput;
        m_goalVerifyCount = 0;
    }

    m_conversation.setSystemPrompt(buildSystemPrompt());
    m_conversation.addUserMessage(userInput);

    emit thinkingStarted();
    m_aiWorker->setPreferences(m_prefs);
    m_aiWorker->start(m_conversation.buildMessages());
}

// 对应Python: SSHAIAgent.execute_commands
void SshAiAgent::executeCommands(const QList<AiCommand> &commands)
{
    if (commands.isEmpty())
        return;

    // 自愈防线：上一轮执行线程若还在跑，先取消
    if (m_execThread && m_execThread->isRunning()) {
        m_execThread->requestStop();
        m_execThread->wait(1000);
    }
    if (m_execThread) {
        m_execThread->deleteLater();
        m_execThread = nullptr;
    }

    m_lastDispatchedCount = commands.size();
    m_execThread = new AiCommandExecThread(m_executor, commands, this);

    // 跨线程信号 — 全链路显式 Qt::QueuedConnection
    // 对应Python: execute_commands 里的 Qt.QueuedConnection 连接
    connect(m_execThread, &AiCommandExecThread::commandStarted,
            this, &SshAiAgent::executionStarted, Qt::QueuedConnection);
    connect(m_execThread, &AiCommandExecThread::commandFinished,
            this, &SshAiAgent::onExecFinished, Qt::QueuedConnection);
    connect(m_execThread, &AiCommandExecThread::allFinished,
            this, &SshAiAgent::onAllExecFinished, Qt::QueuedConnection);
    connect(m_execThread, &AiCommandExecThread::execError,
            this, &SshAiAgent::errorOccurred, Qt::QueuedConnection);
    connect(m_execThread, &AiCommandExecThread::progress,
            this, &SshAiAgent::executionProgress, Qt::QueuedConnection);
    m_execThread->start();
}

// 对应Python: SSHAIAgent.diagnose_error
void SshAiAgent::diagnoseError(const QString &command,
                               const QString &stdoutText,
                               const QString &stderrText, int exitCode)
{
    m_isDiagnosing = true;
    QString prompt = QStringLiteral(
        "刚才执行的命令失败了，请帮我诊断原因并给出修复建议。\n\n"
        "命令: %1\n退出码: %2\n").arg(command).arg(exitCode);
    if (!stdoutText.isEmpty())
        prompt += QStringLiteral("标准输出:\n%1\n").arg(stdoutText.left(2000));
    if (!stderrText.isEmpty())
        prompt += QStringLiteral("标准错误:\n%1\n").arg(stderrText.left(2000));
    prompt += QStringLiteral(
        "\n用户的原始任务是：%1\n"
        "请分析问题原因并提供修复方案，确保最终完成用户的任务。"
        "如果需要执行修复命令，请使用 execute_ssh_commands 工具。")
        .arg(m_originalGoal);

    processUserInput(prompt);
}

// 对应Python: SSHAIAgent.stop
void SshAiAgent::stop()
{
    if (m_aiWorker->isRunning())
        m_aiWorker->stop();
    if (m_execThread && m_execThread->isRunning())
        m_execThread->requestStop();
}

// 对应Python: SSHAIAgent.shutdown
void SshAiAgent::shutdown(int waitMs)
{
    stop();
    if (m_execThread && m_execThread->isRunning()) {
        if (!m_execThread->wait(waitMs)) {
            m_execThread->terminate();
            m_execThread->wait(500);
        }
    }
}

// 对应Python: SSHAIAgent.clear_conversation
void SshAiAgent::clearConversation()
{
    m_conversation.clear();
    m_originalGoal.clear();
    m_isDiagnosing = false;
    m_goalVerifyCount = 0;
    m_conversation.setSystemPrompt(buildSystemPrompt());
}

// 对应Python: ServerProfile.inject_to_system_prompt
// 画像仅存起来：processUserInput 每轮都会重建系统提示词。
void SshAiAgent::setServerProfile(const QString &profile)
{
    m_serverProfile = profile;
}

// ---------------------------------------------------------------------------
// AI callbacks
// ---------------------------------------------------------------------------

void SshAiAgent::onAiDelta(const QString &content, const QString &reasoning)
{
    // 对应Python: _on_ai_delta → ai_message(reasoning, content)
    emit aiMessage(reasoning, content);
}

// 对应Python: SSHAIAgent._on_ai_result
void SshAiAgent::onAiCompleted(const QString &fullText,
                               const QJsonArray &toolCalls,
                               const QString &finishReason)
{
    Q_UNUSED(finishReason);
    emit thinkingFinished();       // 对应Python: _on_ai_worker_finished

    if (!toolCalls.isEmpty()) {
        bool handled = false;
        const QList<AiCommand> commands = parseToolCalls(toolCalls, &handled);
        if (handled) {
            if (!fullText.isEmpty())
                m_conversation.addAssistantMessage(fullText);
            if (!commands.isEmpty())
                emit commandReady(checkCommandsSafety(commands));
            return;
        }
    }

    // 没有工具调用 → 文本 fallback 提取
    if (!fullText.isEmpty()) {
        m_conversation.addAssistantMessage(fullText);
        const QList<AiCommand> fallback = extractCommandsFromText(fullText);
        if (!fallback.isEmpty())
            emit commandReady(checkCommandsSafety(fallback));
    }
}

void SshAiAgent::onAiFailed(const QString &message)
{
    emit thinkingFinished();
    emit errorOccurred(message);   // 对应Python: _on_ai_error
}

// ---------------------------------------------------------------------------
// execution callbacks
// ---------------------------------------------------------------------------

// 对应Python: SSHAIAgent._on_exec_finished
void SshAiAgent::onExecFinished(const AiCommandResult &result)
{
    // 命令结果回填对话（截断 + ANSI 清理在 ChatHistory 内完成）
    m_conversation.addCommandResult(result.cmd, result.stdoutText,
                                    result.stderrText, result.exitCode,
                                    result.description);
    emit executionFinished(result);
}

// 对应Python: SSHAIAgent._on_all_exec_finished
void SshAiAgent::onAllExecFinished(const QList<AiCommandResult> &results)
{
    if (results.isEmpty())
        return;

    const int total = results.size();
    int successCount = 0;
    for (const AiCommandResult &r : results) {
        if (r.exitCode == 0 || r.allowFailure)
            ++successCount;
    }
    const int failCount = total - successCount;
    const int skipped = m_lastDispatchedCount - total;

    if (failCount == 0) {
        emit taskSummary(QStringLiteral("全部 %1 条命令执行成功").arg(total));

        // 目标验证闭环 — 全部成功不代表任务完成
        if (!m_originalGoal.isEmpty() && m_goalVerifyCount < kMaxVerifyRounds) {
            ++m_goalVerifyCount;
            verifyGoalCompletion();
        } else {
            m_isDiagnosing = false;
        }
        return;
    }

    QString summary = QStringLiteral("%1 条命令中 %2 条成功、%3 条失败")
                          .arg(m_lastDispatchedCount)
                          .arg(successCount)
                          .arg(failCount);
    if (skipped > 0)
        summary += QStringLiteral("（后续 %1 条未执行）").arg(skipped);
    summary += QStringLiteral("，正在自动分析失败原因...");
    emit taskSummary(summary);

    // 找到第一条失败命令并触发诊断
    for (const AiCommandResult &r : results) {
        if (r.exitCode != 0 && !r.allowFailure) {
            emit diagnosingStarted();
            diagnoseError(r.cmd, r.stdoutText, r.stderrText, r.exitCode);
            break;
        }
    }
}

// 对应Python: SSHAIAgent._verify_goal_completion
void SshAiAgent::verifyGoalCompletion()
{
    m_isDiagnosing = true;
    const QString prompt = QStringLiteral(
        "命令已执行成功。现在请验证用户的原始任务是否真正完成。\n\n"
        "用户的原始任务：%1\n\n"
        "请执行必要的验证命令（如检查版本号、检查服务状态等）来确认任务实际完成。\n"
        "如果任务已完成，请给出最终确认消息。\n"
        "如果任务未完成，请继续执行剩余步骤直到完成。")
        .arg(m_originalGoal);
    processUserInput(prompt);
}

// ---------------------------------------------------------------------------
// parsing / safety / prompt
// ---------------------------------------------------------------------------

// 对应Python: ssh_agent.py::TOOLS
QJsonArray SshAiAgent::toolsDefinition()
{
    const QByteArray json = R"JSON([
      {
        "type": "function",
        "function": {
          "name": "execute_ssh_commands",
          "description": "在远程SSH服务器上执行一组命令。注意根据当前连接用户的权限级别决定是否添加sudo前缀",
          "parameters": {
            "type": "object",
            "properties": {
              "commands": {
                "type": "array",
                "items": {
                  "type": "object",
                  "properties": {
                    "cmd": {"type": "string", "description": "要执行的命令。重要：若当前非 root 用户，对需要超级权限的操作（apt/yum/dnf/systemctl/service/chmod/chown/mkdir -p /etc/等系统目录写入）必须以 sudo 开头"},
                    "description": {"type": "string", "description": "命令的用途说明"},
                    "allow_failure": {"type": "boolean", "description": "是否允许失败"},
                    "interactive": {"type": "boolean", "description": "是否在用户的终端中执行（默认 true）。仅在需要静默后台执行并精确抓取 stdout/stderr 的查询类场景才设为 false。"}
                  },
                  "required": ["cmd", "description"]
                }
              },
              "explanation": {"type": "string", "description": "整体方案说明"}
            },
            "required": ["commands", "explanation"]
          }
        }
      }
    ])JSON";
    return QJsonDocument::fromJson(json).array();
}

// 对应Python: SSHAIAgent._parse_tool_calls
QList<AiCommand> SshAiAgent::parseToolCalls(const QJsonArray &toolCalls,
                                            bool *handled)
{
    if (handled)
        *handled = false;

    for (const QJsonValue &v : toolCalls) {
        const QJsonObject call = v.toObject();
        const QString name = call.value(QStringLiteral("name")).toString();
        if (name != QLatin1String("execute_ssh_commands"))
            continue;                    // Skill 工具等未移植，忽略

        const QString arguments =
            call.value(QStringLiteral("arguments")).toString();
        QJsonParseError parseError{};
        const QJsonDocument doc =
            QJsonDocument::fromJson(arguments.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            return {};                   // 对应Python: JSONDecodeError → None

        const QJsonObject data = doc.object();
        const QString explanation =
            data.value(QStringLiteral("explanation")).toString();
        // 将 explanation 作为 AI 消息发出
        if (!explanation.isEmpty())
            emit aiMessage(QString(), explanation);

        QList<AiCommand> commands;
        const QJsonArray arr = data.value(QStringLiteral("commands")).toArray();
        for (const QJsonValue &cv : arr) {
            const QJsonObject obj = cv.toObject();
            AiCommand cmd;
            cmd.cmd = obj.value(QStringLiteral("cmd")).toString();
            cmd.description =
                obj.value(QStringLiteral("description")).toString();
            cmd.allowFailure =
                obj.value(QStringLiteral("allow_failure")).toBool(false);
            cmd.interactive =
                obj.value(QStringLiteral("interactive")).toBool(true);
            if (!cmd.cmd.isEmpty())
                commands.append(cmd);
        }
        if (handled)
            *handled = true;
        return commands;
    }
    return {};
}

// 对应Python: SSHAIAgent._extract_commands_from_text
QList<AiCommand> SshAiAgent::extractCommandsFromText(const QString &text)
{
    QList<AiCommand> commands;

    // 仅识别明确声明语言为 bash/shell/sh 的代码块
    static const QRegularExpression blockRe(
        QStringLiteral("```(?:bash|shell|sh)\\s*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    // CJK 字符：shell 命令几乎不会出现中文，出现即为总结/注释行
    static const QRegularExpression cjkRe(QStringLiteral("[\\x{4e00}-\\x{9fff}]"));
    // markdown 有序列表 "1. xxx" / "2) xxx"
    static const QRegularExpression orderedListRe(
        QStringLiteral("^\\d+[\\.\\)]\\s"));

    auto it = blockRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QStringList rawLines =
            m.captured(1).trimmed().split(QLatin1Char('\n'));
        for (const QString &raw : rawLines) {
            const QString line = raw.trimmed();
            if (line.isEmpty())
                continue;
            // 跳过 shell 注释、markdown 无序列表项、引用块
            if (line.startsWith(QLatin1Char('#'))
                || line.startsWith(QLatin1String("- "))
                || line.startsWith(QLatin1String("* "))
                || line.startsWith(QLatin1String("+ "))
                || line.startsWith(QLatin1String("> ")))
                continue;
            // 跳过 "-"、"*"、"+" 独占一行的分隔符
            if (line == QLatin1String("-") || line == QLatin1String("*")
                || line == QLatin1String("+"))
                continue;
            // 跳过 markdown 有序列表项
            if (orderedListRe.match(line).hasMatch())
                continue;
            // 跳过含中文字符的行
            if (cjkRe.match(line).hasMatch())
                continue;
            AiCommand cmd;
            cmd.cmd = line;
            cmd.description = QStringLiteral("从 AI 回复中提取的命令");
            cmd.allowFailure = false;
            commands.append(cmd);
        }
    }
    return commands;
}

// 对应Python: SSHAIAgent._check_commands_safety
QList<AiCommand> SshAiAgent::checkCommandsSafety(
    const QList<AiCommand> &commands) const
{
    QList<AiCommand> checked;
    checked.reserve(commands.size());
    for (AiCommand cmd : commands) {
        cmd.safety = m_safetyChecker.check(cmd.cmd);
        checked.append(cmd);
    }
    return checked;
}

// 对应Python: SSHAIAgent._build_system_prompt（Skill 段落未移植）
QString SshAiAgent::buildSystemPrompt() const
{
    QString prompt = QStringLiteral(
        "你是一个专业的 Linux 运维助手，可以帮助用户在远程服务器上执行操作。\n"
        "你的职责包括：系统管理、服务部署、故障诊断、性能优化等。\n"
        "请根据用户的需求，生成安全、高效的命令方案。\n");

    prompt += QStringLiteral(
        "\n\n安全约束：\n"
        "1. 绝对禁止执行会导致数据丢失的危险命令（如 rm -rf /、dd 覆写磁盘等）\n"
        "2. 涉及服务重启、关机等操作前需明确告知用户风险\n"
        "3. 修改系统配置前建议先备份\n"
        "4. 优先使用只读命令获取信息后再决定操作方案\n"
        "5. 对于不确定的操作，先解释方案等待用户确认\n");

    prompt += QStringLiteral(
        "\n当用户请求执行操作时，请使用 execute_ssh_commands 工具来提供命令方案。\n"
        "每条命令需包含用途说明。如果某些命令允许失败（如检查命令），请设置 allow_failure 为 true。\n"
        "命令的退出码与输出会自动回填给你用于诊断。\n"
        "重要：不要生成会调起分页器的命令。例如：\n"
        "  - systemctl 请加 --no-pager（如 systemctl status xxx --no-pager）\n"
        "  - journalctl 请加 --no-pager\n"
        "  - git log/diff/show 请加 --no-pager 或 --oneline -n N\n"
        "  - 避免使用 less/more/man，如需查看详细信息请用 cat/head/tail\n"
        "如果用户只是提问或需要解释，直接用文本回复即可，不必使用工具。");

    // 服务器画像（ServerProfileBuilder 异步构建，未就绪时为空）
    if (!m_serverProfile.isEmpty())
        prompt += QStringLiteral("\n\n") + m_serverProfile;

    return prompt;
}

} // namespace cubeshell
