// ChatHistory.cpp — see ChatHistory.h.
//
// 对应Python: core/ai/conversation.py

#include "ChatHistory.h"

#include "config/GlobalState.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace cubeshell {

namespace {

// 对应Python: conversation.py::_ANSI_ESCAPE_RE
const QRegularExpression &ansiEscapeRe()
{
    static const QRegularExpression re(QStringLiteral(
        "\x1b\\[[0-9;]*[a-zA-Z]|\x1b\\].*?\x07|\x1b\\[.*?\x1b\\\\"));
    return re;
}

} // namespace

ChatHistory::ChatHistory(const QString &systemPrompt, int maxHistory)
    : m_systemPrompt(systemPrompt)
    , m_maxHistory(maxHistory)
{
}

void ChatHistory::appendMessage(const QString &role, const QString &content)
{
    ChatMessage msg;
    msg.role = role;
    msg.content = content;
    msg.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_messages.append(msg);
    compressIfNeeded();
}

// 对应Python: ConversationManager.add_user_message
void ChatHistory::addUserMessage(const QString &content)
{
    appendMessage(QStringLiteral("user"), content);
}

// 对应Python: ConversationManager.add_assistant_message
void ChatHistory::addAssistantMessage(const QString &content)
{
    appendMessage(QStringLiteral("assistant"), content);
}

// 对应Python: ConversationManager.add_tool_response
void ChatHistory::addToolResponse(const QString &toolName, const QString &content)
{
    appendMessage(QStringLiteral("user"),
                  QStringLiteral("[Skill 执行结果] 工具: %1\n%2")
                      .arg(toolName, content));
}

// 对应Python: ConversationManager.add_command_result
void ChatHistory::addCommandResult(const QString &command,
                                   const QString &stdoutText,
                                   const QString &stderrText, int exitCode,
                                   const QString &description)
{
    const QString truncatedStdout = truncateOutput(stripAnsi(stdoutText));
    const QString truncatedStderr = truncateOutput(stripAnsi(stderrText));

    QStringList parts;
    parts << QStringLiteral("[命令执行结果]");
    if (!description.isEmpty())
        parts << QStringLiteral("描述: %1").arg(description);
    parts << QStringLiteral("命令: %1").arg(command);
    parts << QStringLiteral("退出码: %1").arg(exitCode);
    if (!truncatedStdout.isEmpty())
        parts << QStringLiteral("标准输出:\n%1").arg(truncatedStdout);
    if (!truncatedStderr.isEmpty())
        parts << QStringLiteral("标准错误:\n%1").arg(truncatedStderr);

    // 命令结果以 user 角色添加（模拟用户反馈执行结果给 AI）
    appendMessage(QStringLiteral("user"), parts.join(QLatin1Char('\n')));
}

// 对应Python: ConversationManager.build_messages
QJsonArray ChatHistory::buildMessages() const
{
    QJsonArray messages;
    if (!m_systemPrompt.isEmpty()) {
        QJsonObject sys;
        sys.insert(QStringLiteral("role"), QStringLiteral("system"));
        sys.insert(QStringLiteral("content"), m_systemPrompt);
        messages.append(sys);
    }
    for (const ChatMessage &m : m_messages) {
        QJsonObject obj;
        obj.insert(QStringLiteral("role"), m.role);
        obj.insert(QStringLiteral("content"), m.content);
        messages.append(obj);
    }
    return messages;
}

// 对应Python: ConversationManager.clear
void ChatHistory::clear()
{
    m_messages.clear();
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

QString ChatHistory::historyDir()
{
    const QString dir = GlobalState::dataDir() + QStringLiteral("/ai_history");
    QDir().mkpath(dir);
    return dir;
}

bool ChatHistory::saveToFile(const QString &conversationId,
                             QString *errorOut) const
{
    QJsonArray arr;
    for (const ChatMessage &m : m_messages) {
        QJsonObject obj;
        obj.insert(QStringLiteral("role"), m.role);
        obj.insert(QStringLiteral("content"), m.content);
        obj.insert(QStringLiteral("timestamp"), m.timestamp);
        arr.append(obj);
    }
    QJsonObject root;
    root.insert(QStringLiteral("messages"), arr);

    QSaveFile file(historyDir() + QStringLiteral("/%1.json").arg(conversationId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    return true;
}

bool ChatHistory::loadFromFile(const QString &conversationId, QString *errorOut)
{
    QFile file(historyDir() + QStringLiteral("/%1.json").arg(conversationId));
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = parseError.errorString();
        return false;
    }

    m_messages.clear();
    const QJsonArray arr = doc.object().value(QStringLiteral("messages")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        ChatMessage msg;
        msg.role = obj.value(QStringLiteral("role")).toString();
        msg.content = obj.value(QStringLiteral("content")).toString();
        msg.timestamp = obj.value(QStringLiteral("timestamp")).toString();
        if (!msg.role.isEmpty())
            m_messages.append(msg);
    }
    return true;
}

bool ChatHistory::removeConversation(const QString &conversationId)
{
    return QFile::remove(
        historyDir() + QStringLiteral("/%1.json").arg(conversationId));
}

QStringList ChatHistory::listConversations()
{
    QDir dir(historyDir());
    const QFileInfoList entries = dir.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    QStringList ids;
    for (const QFileInfo &info : entries)
        ids << info.completeBaseName();
    return ids;
}

// ---------------------------------------------------------------------------
// pure helpers
// ---------------------------------------------------------------------------

// 对应Python: conversation.py::_strip_ansi
QString ChatHistory::stripAnsi(const QString &text)
{
    if (text.isEmpty())
        return text;
    QString out = text;
    out.remove(ansiEscapeRe());
    return out;
}

// 对应Python: ConversationManager._truncate_output
QString ChatHistory::truncateOutput(const QString &text)
{
    if (text.size() <= kMaxOutputChars)
        return text;
    const int truncatedChars = text.size() - kTruncateKeepChars * 2;
    const QString head = text.left(kTruncateKeepChars);
    const QString tail = text.right(kTruncateKeepChars);
    return QStringLiteral("%1\n\n... [已截断 %2 字符] ...\n\n%3")
        .arg(head).arg(truncatedChars).arg(tail);
}

// 对应Python: ConversationManager._compress_if_needed
void ChatHistory::compressIfNeeded()
{
    const int keepCount = m_maxHistory * 2;
    if (m_messages.size() <= kCompressThreshold
        && m_messages.size() <= keepCount)
        return;
    if (m_messages.size() > keepCount)
        m_messages = m_messages.mid(m_messages.size() - keepCount);
}

} // namespace cubeshell
