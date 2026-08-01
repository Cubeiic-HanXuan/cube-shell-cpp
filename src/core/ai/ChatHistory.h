#pragma once

// ChatHistory.h — conversation context manager + JSON persistence.
//
// 对应Python: core/ai/conversation.py (ConversationManager) — 上下文管理、
//             输出截断、ANSI 清理、上下文压缩；
//           + 任务规定的 JSON 持久化格式（messages 数组，每条
//             role/content/timestamp），与 Python 侧数据可互读。
//
// Persistence layout: <dataDir>/ai_history/<conversationId>.json
//     { "messages": [ {"role": "...", "content": "...",
//                      "timestamp": "ISO-8601"} ] }

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {

// One chat message (role: "user" | "assistant" | "system").
struct ChatMessage {
    QString role;
    QString content;
    QString timestamp;   // ISO-8601, e.g. "2026-07-29T12:00:00"
};

// 对应Python: conversation.py::ConversationManager
class ChatHistory {
public:
    // 配置常量 — 对应Python: ConversationManager 类常量
    static constexpr int kMaxHistoryRounds = 20;    // MAX_HISTORY_ROUNDS
    static constexpr int kMaxOutputChars = 4000;    // MAX_OUTPUT_CHARS
    static constexpr int kTruncateKeepChars = 2000; // TRUNCATE_KEEP_CHARS
    static constexpr int kCompressThreshold = 40;   // COMPRESS_THRESHOLD

    explicit ChatHistory(const QString &systemPrompt = QString(),
                         int maxHistory = kMaxHistoryRounds);

    QString systemPrompt() const { return m_systemPrompt; }
    void setSystemPrompt(const QString &prompt) { m_systemPrompt = prompt; }

    // 对应Python: add_user_message
    void addUserMessage(const QString &content);
    // 对应Python: add_assistant_message
    void addAssistantMessage(const QString &content);
    // 对应Python: add_tool_response（以 user 角色回填工具结果）
    void addToolResponse(const QString &toolName, const QString &content);
    // 对应Python: add_command_result（截断 + ANSI 清理 + user 角色回填）
    void addCommandResult(const QString &command, const QString &stdoutText,
                          const QString &stderrText, int exitCode,
                          const QString &description = QString());

    // 对应Python: build_messages — OpenAI Chat Completions 格式
    QJsonArray buildMessages() const;

    QList<ChatMessage> messages() const { return m_messages; }
    int messageCount() const { return m_messages.size(); }

    // 对应Python: clear
    void clear();

    // --- JSON 持久化（加载/保存/删除/列表） ---

    // <dataDir>/ai_history 目录（按需创建）。
    static QString historyDir();

    // 保存为 historyDir()/<conversationId>.json。
    bool saveToFile(const QString &conversationId,
                    QString *errorOut = nullptr) const;
    // 从 historyDir()/<conversationId>.json 加载（替换当前消息）。
    bool loadFromFile(const QString &conversationId,
                      QString *errorOut = nullptr);
    // 删除指定对话文件。
    static bool removeConversation(const QString &conversationId);
    // 列出全部对话 id（按修改时间倒序）。
    static QStringList listConversations();

    // --- 纯函数（供单测） ---

    // 对应Python: conversation.py::_strip_ansi
    static QString stripAnsi(const QString &text);
    // 对应Python: ConversationManager._truncate_output
    static QString truncateOutput(const QString &text);

private:
    // 对应Python: ConversationManager._compress_if_needed
    void compressIfNeeded();
    void appendMessage(const QString &role, const QString &content);

    QString m_systemPrompt;
    int m_maxHistory;
    QList<ChatMessage> m_messages;
};

} // namespace cubeshell
