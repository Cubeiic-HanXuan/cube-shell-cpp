// ClaudeSessionData.cpp — 历史日志的增量解析、轮次归并与 Markdown 导出。
// 只保留可展示的数据；原始字节的无损导出由 ClaudeSessionStore 负责。

#include "claude_code/ClaudeSessionData.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

namespace cubeshell {
namespace {
qint64 timestamp(const QJsonValue &value)
{
    if (value.isDouble())
        return static_cast<qint64>(value.toDouble());
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
}
QString jsonText(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}
}

QString ClaudeSessionEntry::shortToolName() const
{
    return toolName.section(QLatin1Char('.'), -1);
}
QString ClaudeSessionEntry::filePath() const
{
    return input.value(QStringLiteral("file_path")).toString(
        input.value(QStringLiteral("notebook_path")).toString(
            input.value(QStringLiteral("path")).toString()));
}
bool ClaudeSessionEntry::isChange() const
{
    const auto name = shortToolName();
    return kind == Tool && (name == QLatin1String("Edit") || name == QLatin1String("Write")
        || name == QLatin1String("NotebookEdit") || name == QLatin1String("MultiEdit"));
}
QString ClaudeSessionEntry::searchText() const
{
    return text + QLatin1Char('\n') + toolName + QLatin1Char('\n') + jsonText(input)
        + QLatin1Char('\n') + result;
}

QString ClaudeSessionData::contentText(const QJsonValue &content)
{
    if (content.isString())
        return content.toString();
    QStringList parts;
    if (content.isArray()) {
        for (const auto &value : content.toArray()) {
            const auto block = value.toObject();
            const auto type = block.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("text") || type.isEmpty())
                parts.append(block.value(QStringLiteral("text")).toString());
            else if (type == QLatin1String("image"))
                parts.append(QStringLiteral("[图片附件]"));
            else if (type == QLatin1String("document"))
                parts.append(QStringLiteral("[文档附件]"));
        }
    }
    return parts.join(QLatin1Char('\n'));
}

QString ClaudeSessionData::cleanMessage(const QString &text)
{
    QString result = text;
    static const QRegularExpression metadata(
        QStringLiteral("<(system-reminder|total_tokens|local-command-caveat|environment_context)[^>]*>.*?</\\1>"),
        QRegularExpression::DotMatchesEverythingOption);
    result.remove(metadata);
    return result.trimmed();
}
QString ClaudeSessionData::cleanPreview(const QString &text)
{
    return cleanMessage(text).simplified();
}

void ClaudeSessionData::append(const QByteArray &bytes, bool endOfFile)
{
    // 损坏记录或巨大的附件不能让半行缓冲无限增长；超过上限后跳过整行。
    // 界面计入不可读记录，原始字节仍可通过 JSONL 导出保留。
    constexpr qsizetype maxLine = 64 * 1024 * 1024;
    qsizetype start = 0;
    while (start < bytes.size()) {
        const auto newline = bytes.indexOf('\n', start);
        const auto end = newline < 0 ? bytes.size() : newline;
        if (!m_skippingLine) {
            if (m_pending.size() + end - start > maxLine) {
                m_pending.clear();
                m_skippingLine = true;
                ++m_malformedLines;
            } else {
                m_pending.append(bytes.constData() + start, end - start);
            }
        }
        if (newline >= 0) {
            if (!m_skippingLine)
                parseLine(m_pending);
            m_pending.clear();
            m_skippingLine = false;
        }
        start = end + 1;
    }
    if (endOfFile && !m_pending.isEmpty()) {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(m_pending, &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            accept(doc.object());
            m_pending.clear();
        }
        // 暂时到达文件末尾不等于记录完整；保留半行，后续追加字节后仍可继续解析。
    }
}

void ClaudeSessionData::parseLine(const QByteArray &line)
{
    if (line.trimmed().isEmpty())
        return;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        ++m_malformedLines;
        return;
    }
    accept(doc.object());
}

void ClaudeSessionData::setTitle(const QString &title, int priority)
{
    const auto cleaned = cleanPreview(title);
    if (!cleaned.isEmpty() && priority >= m_titlePriority) {
        m_summary.name = cleaned.left(240);
        m_titlePriority = priority;
    }
}

void ClaudeSessionData::addEntry(ClaudeSessionEntry entry)
{
    entry.turn = m_turns.size() - 1;
    if (entry.kind == ClaudeSessionEntry::User) {
        entry.turn = m_turns.size();
        m_turns.append(m_entries.size());
    }
    if (entry.kind == ClaudeSessionEntry::User || entry.kind == ClaudeSessionEntry::Assistant) {
        const auto preview = cleanPreview(entry.text);
        if (!preview.isEmpty()) {
            m_summary.lastMessage = preview.left(600);
            if (entry.kind == ClaudeSessionEntry::User && m_summary.firstMessage.isEmpty()) {
                m_summary.firstMessage = preview.left(2000);
                if (!m_titlePriority)
                    setTitle(preview, 0);
            }
        }
    }
    m_entries.append(std::move(entry));
}

// ---------------------------------------------------------------------------
// 记录归并：标题优先级、UUID 去重、工具调用 / 结果关联
// ---------------------------------------------------------------------------

void ClaudeSessionData::accept(const QJsonObject &record)
{
    const QString type = record.value(QStringLiteral("type")).toString();
    // 主会话只展示自身记录；子代理的独立 sidechain 不计入主会话轮次。
    if (record.value(QStringLiteral("isSidechain")).toBool())
        return;
    if (type == QLatin1String("custom-title")) {
        setTitle(record.value(QStringLiteral("customTitle")).toString(), 3);
        return;
    }
    if (type == QLatin1String("ai-title")) {
        setTitle(record.value(QStringLiteral("aiTitle")).toString(), 2);
        return;
    }
    if (type == QLatin1String("summary")) {
        setTitle(record.value(QStringLiteral("summary")).toString(), 1);
        return;
    }
    if (type != QLatin1String("user") && type != QLatin1String("assistant")
        && type != QLatin1String("system"))
        return; // progress、queue-operation、file-history-snapshot 等辅助记录不形成对话轮次

    const QString uuid = record.value(QStringLiteral("uuid")).toString();
    if (!uuid.isEmpty()) {
        if (m_seen.contains(uuid))
            return;
        m_seen.insert(uuid);
    }
    if (m_summary.cwd.isEmpty())
        m_summary.cwd = record.value(QStringLiteral("cwd")).toString();
    if (m_summary.sessionId.isEmpty())
        m_summary.sessionId = record.value(QStringLiteral("sessionId")).toString();
    const qint64 ts = timestamp(record.value(QStringLiteral("timestamp")));
    if (ts > 0) {
        if (!m_summary.startedAtMs || ts < m_summary.startedAtMs)
            m_summary.startedAtMs = ts;
        m_summary.updatedAtMs = qMax(m_summary.updatedAtMs, ts);
    }
    const auto message = record.value(QStringLiteral("message")).toObject();
    const auto model = message.value(QStringLiteral("model")).toString();
    if (!model.isEmpty() && !m_models.contains(model))
        m_models.append(model);

    if (type == QLatin1String("system")) {
        ClaudeSessionEntry e;
        e.timestampMs = ts;
        e.text = contentText(record.value(QStringLiteral("content")));
        if (e.text.isEmpty())
            e.text = record.value(QStringLiteral("subtype")).toString();
        if (e.text == QLatin1String("compact_boundary"))
            e.text = QStringLiteral("上下文压缩边界");
        if (!e.text.isEmpty())
            addEntry(e);
        return;
    }
    const bool user = type == QLatin1String("user");
    const bool meta = record.value(QStringLiteral("isMeta")).toBool();
    const auto content = message.value(QStringLiteral("content"));
    QJsonArray blocks = content.toArray();
    if (content.isString())
        blocks.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), content.toString()}});
    // 用户消息中的文字和附件合成一条提问；工具结果块单独关联，不能额外生成轮次。
    if (user) {
        const auto text = contentText(content);
        if (!text.trimmed().isEmpty()) {
            ClaudeSessionEntry e;
            e.kind = meta || cleanPreview(text).isEmpty() ? ClaudeSessionEntry::System : ClaudeSessionEntry::User;
            e.text = text;
            e.timestampMs = ts;
            addEntry(e);
        }
    }
    for (const auto &value : blocks) {
        const auto block = value.toObject();
        const auto blockType = block.value(QStringLiteral("type")).toString();
        ClaudeSessionEntry e;
        e.timestampMs = ts;
        if (blockType == QLatin1String("tool_result")) {
            const auto id = block.value(QStringLiteral("tool_use_id")).toString();
            if (!m_tools.contains(id)) {
                auto pending = block;
                pending.insert(QStringLiteral("_resultTimestamp"), ts);
                m_pendingResults.insert(id, pending);
                // 先保留孤立结果供后续调用关联，同时显示提示，不静默丢弃已有历史。
                e.text = QStringLiteral("工具结果（%1，未找到调用记录）\n%2")
                    .arg(id, contentText(block.value(QStringLiteral("content"))));
                addEntry(e);
                continue;
            }
            auto &tool = m_entries[m_tools.value(id)];
            if (tool.failed)
                --m_failureCount;
            tool.hasResult = true;
            tool.resultTimestampMs = ts;
            tool.failed = block.value(QStringLiteral("is_error")).toBool();
            tool.result = contentText(block.value(QStringLiteral("content")));
            if (tool.failed)
                ++m_failureCount;
            continue;
        }
        if (user)
            continue;
        if (blockType == QLatin1String("text")) {
            e.kind = ClaudeSessionEntry::Assistant;
            e.text = block.value(QStringLiteral("text")).toString();
        } else if (blockType == QLatin1String("tool_use")) {
            e.kind = ClaudeSessionEntry::Tool;
            e.toolId = block.value(QStringLiteral("id")).toString();
            if (!e.toolId.isEmpty() && m_tools.contains(e.toolId))
                continue;
            e.toolName = block.value(QStringLiteral("name")).toString();
            e.input = block.value(QStringLiteral("input")).toObject();
            e.text = e.input.value(QStringLiteral("description")).toString();
            if (m_pendingResults.contains(e.toolId)) {
                const auto result = m_pendingResults.take(e.toolId);
                e.hasResult = true;
                e.resultTimestampMs = static_cast<qint64>(result.value(QStringLiteral("_resultTimestamp")).toDouble());
                e.failed = result.value(QStringLiteral("is_error")).toBool();
                e.result = contentText(result.value(QStringLiteral("content")));
                if (e.failed)
                    ++m_failureCount;
            }
            m_tools.insert(e.toolId, m_entries.size());
            ++m_toolCount;
        } else if (blockType == QLatin1String("thinking") || blockType == QLatin1String("redacted_thinking")) {
            e.kind = ClaudeSessionEntry::Thinking;
            e.text = blockType == QLatin1String("thinking")
                ? block.value(QStringLiteral("thinking")).toString() : QString();
            if (e.text.isEmpty())
                e.text = QStringLiteral("思考内容不可见");
        } else {
            e.kind = ClaudeSessionEntry::Attachment;
            e.text = QStringLiteral("[%1 内容块]").arg(blockType);
        }
        addEntry(e);
    }
}

// ---------------------------------------------------------------------------
// 展示轮次：按用户提问划分范围，区分执行过程与最后一次工具调用之后的回复
// ---------------------------------------------------------------------------

QList<ClaudeSessionTurn> ClaudeSessionData::presentationTurns() const
{
    QList<ClaudeSessionTurn> result;
    if (m_entries.isEmpty())
        return result;
    QList<int> starts = m_turns;
    if (starts.isEmpty() || starts.first() != 0)
        starts.prepend(0);
    for (int n = 0; n < starts.size(); ++n) {
        ClaudeSessionTurn turn;
        turn.firstEntry = starts[n];
        turn.endEntry = n + 1 < starts.size() ? starts[n + 1] : m_entries.size();
        if (m_entries[turn.firstEntry].kind == ClaudeSessionEntry::User) {
            turn.userEntry = turn.firstEntry;
            turn.question = cleanPreview(m_entries[turn.firstEntry].text);
        }
        int lastTool = turn.firstEntry - 1;
        QSet<QString> changed;
        for (int i = turn.firstEntry; i < turn.endEntry; ++i) {
            const auto &entry = m_entries[i];
            if (entry.timestampMs > 0) {
                if (!turn.startedAtMs)
                    turn.startedAtMs = entry.timestampMs;
                turn.endedAtMs = qMax(turn.endedAtMs, entry.timestampMs);
            }
            turn.endedAtMs = qMax(turn.endedAtMs, entry.resultTimestampMs);
            if (entry.kind != ClaudeSessionEntry::Tool)
                continue;
            lastTool = i;
            ++turn.tools;
            if (entry.shortToolName() == QLatin1String("Bash"))
                ++turn.commands;
            if (entry.failed)
                ++turn.failures;
            if (!entry.hasResult)
                ++turn.pendingTools;
            if (entry.isChange() && entry.hasResult && !entry.failed && !entry.filePath().isEmpty())
                changed.insert(entry.filePath());
        }
        turn.changedFiles = changed.size();
        for (int i = qMax(turn.firstEntry, lastTool + 1); i < turn.endEntry; ++i) {
            if (m_entries[i].kind == ClaudeSessionEntry::Assistant && !m_entries[i].text.trimmed().isEmpty()) {
                if (turn.firstReply < 0)
                    turn.firstReply = i;
                turn.replyPreview = cleanPreview(m_entries[i].text).left(300);
            }
        }
        const int processEnd = turn.firstReply >= 0 ? turn.firstReply : turn.endEntry;
        turn.steps = qMax(0, processEnd - turn.firstEntry - (turn.userEntry >= 0 ? 1 : 0));
        result.append(std::move(turn));
    }
    return result;
}

QString ClaudeSessionData::quote(const QString &text, bool powershell)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), powershell ? QStringLiteral("''") : QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}
QString ClaudeSessionData::resumeCommand(const ClaudeSessionInfo &session, bool powershell)
{
    // 会话 ID 只能是 UUID，不接受 CLI 参数片段；工作目录拒绝换行等终端控制字符。
    // 先成功切换到原项目目录再恢复，避免在错误目录启动会话。
    if (QUuid(session.sessionId).isNull() || session.cwd.isEmpty())
        return {};
    for (const auto ch : session.cwd) {
        if (ch.unicode() < 32 || ch.unicode() == 127)
            return {};
    }
    const QString command = QStringLiteral("claude --resume %1").arg(quote(session.sessionId, powershell));
    return powershell ? QStringLiteral("Set-Location -LiteralPath %1; if ($?) { %2 }").arg(quote(session.cwd, true), command)
                      : QStringLiteral("cd %1 && %2").arg(quote(session.cwd), command);
}
bool ClaudeSessionData::validRelativePath(const QString &path)
{
    const auto parts = path.split(QLatin1Char('/'));
    if (parts.size() != 2 || parts[0].isEmpty() || parts[0] == QLatin1String(".")
        || parts[0] == QLatin1String("..") || path.contains(QLatin1Char('\\'))
        || path.contains(QChar(0)) || !parts[1].endsWith(QLatin1String(".jsonl")))
        return false;
    return !QUuid(parts[1].chopped(6)).isNull();
}
bool ClaudeSessionData::matches(const ClaudeSessionInfo &session, const QString &query)
{
    const auto haystack = session.name + QLatin1Char('\n') + session.cwd + QLatin1Char('\n')
        + session.firstMessage + QLatin1Char('\n') + session.sessionId;
    for (const auto &word : query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!haystack.contains(word, Qt::CaseInsensitive))
            return false;
    }
    return true;
}
QString ClaudeSessionData::projectName(const QString &cwd)
{
    QString path = cwd;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    return path.isEmpty() ? QStringLiteral("未知项目") : path.section(QLatin1Char('/'), -1);
}
QString ClaudeSessionData::relativeTime(qint64 timestampMs, qint64 nowMs)
{
    if (timestampMs <= 0)
        return QStringLiteral("时间未知");
    if (!nowMs)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto seconds = qMax<qint64>(0, (nowMs - timestampMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("刚刚");
    if (seconds < 3600)
        return QStringLiteral("%1 分钟前").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 小时前").arg(seconds / 3600);
    if (seconds < 86400 * 30)
        return QStringLiteral("%1 天前").arg(seconds / 86400);
    return QDateTime::fromMSecsSinceEpoch(timestampMs).toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}
// 导出代码围栏必须长于正文内已有的反引号串，避免工具输出提前结束 Markdown 代码块。
QString ClaudeSessionData::fenced(const QString &text, const QString &language)
{
    QString fence = QStringLiteral("```");
    while (text.contains(fence))
        fence += QLatin1Char('`');
    return fence + language + QLatin1Char('\n') + text + QLatin1Char('\n') + fence + QLatin1Char('\n');
}
QString ClaudeSessionData::toMarkdown() const
{
    QString out = QStringLiteral("# %1\n\n项目：%2\n\n会话：%3\n\n")
        .arg(m_summary.name, m_summary.cwd, m_summary.sessionId);
    for (const auto &e : m_entries) {
        const QString role = e.kind == ClaudeSessionEntry::User ? QStringLiteral("你")
            : e.kind == ClaudeSessionEntry::Assistant ? QStringLiteral("Claude Code")
            : e.kind == ClaudeSessionEntry::Tool ? e.toolName
            : e.kind == ClaudeSessionEntry::Thinking ? QStringLiteral("思考摘要") : QStringLiteral("系统 / 附件");
        out += QStringLiteral("## %1 · %2\n\n").arg(role,
            QDateTime::fromMSecsSinceEpoch(e.timestampMs).toString(Qt::ISODate));
        if (e.kind == ClaudeSessionEntry::Tool) {
            out += fenced(jsonText(e.input), QStringLiteral("json"));
            out += e.hasResult ? (e.failed ? QStringLiteral("\n失败\n\n") : QStringLiteral("\n已完成\n\n"))
                               : QStringLiteral("\n未记录结果\n\n");
            out += fenced(e.result);
        } else {
            out += e.text + QStringLiteral("\n\n");
        }
    }
    if (m_malformedLines || hasIncompleteLine())
        out += QStringLiteral("\n> 读取提示：%1 条损坏/超过 64 MiB 的记录，%2 条不完整末行未渲染。原始 JSONL 导出保留所有字节。\n")
            .arg(m_malformedLines).arg(hasIncompleteLine() ? 1 : 0);
    return out;
}
} // namespace cubeshell
