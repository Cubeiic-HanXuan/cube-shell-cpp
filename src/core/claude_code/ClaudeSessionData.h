#pragma once

// ClaudeSessionData.h — Claude Code 历史会话的数据模型与增量解析（纯逻辑，无文件 IO）。
//
// 将 JSONL 记录转换为列表摘要、消息/工具条目及「提问 → 执行过程 → 回复」轮次。
// 分页边界允许截断 UTF-8 和 JSON；工具结果可跨页或先于调用到达。
// 解析器由工作线程复制并追加数据，界面通过只读快照消费，避免跨线程修改同一份历史。

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QSet>
#include <QStringList>
#include <memory>

namespace cubeshell {

// 会话列表摘要；以 relativePath 标识文件，不依赖标题或列表行号。
struct ClaudeSessionInfo {
    QString sessionId;
    QString name;
    QString cwd;
    QString status = QStringLiteral("saved"); // 历史文件本身不代表 CLI 仍在运行
    bool running = false;
    qint64 startedAtMs = 0;
    qint64 updatedAtMs = 0; // 列表使用文件修改时间，解析摘要使用日志时间；单位均为毫秒
    qint64 fileSize = 0;    // 枚举时的大小，作为本次分页 / 导出的快照边界
    QString relativePath; // projects 下的「项目目录/UUID.jsonl」，不能当作 shell 片段
    QString firstMessage;
    QString lastMessage;
    bool partialSummary = false; // 首尾采样有间隔或读取失败，摘要不代表完整历史
};

// 一个可展示条目；工具调用和结果归并到同一条目，不把 tool_result 当成用户提问。
struct ClaudeSessionEntry {
    enum Kind { User, Assistant, Tool, Thinking, System, Attachment };
    Kind kind = System;
    QString text;
    QString toolId;
    QString toolName;
    QJsonObject input;
    QString result;
    bool hasResult = false; // 区分「空输出」和「日志尚未记录结果」
    bool failed = false;
    qint64 timestampMs = 0;
    qint64 resultTimestampMs = 0; // 与调用时间之差仅为日志记录间隔
    int turn = -1; // -1 表示第一条用户提问之前的记录

    QString shortToolName() const;
    QString filePath() const;
    bool isChange() const;
    // 搜索覆盖原文、工具名、完整输入参数和输出，不受界面预览截断影响。
    QString searchText() const;
};

// 一轮提问及其执行 / 回复的只读展示摘要；没有用户提问的前导记录也可组成一轮。
struct ClaudeSessionTurn {
    int userEntry = -1;
    int firstEntry = 0;
    int endEntry = 0; // 右开区间，不包含此索引
    int firstReply = -1; // 最后一次工具调用之后的首条助手文本；-1 表示未记录最终回复
    int steps = 0;
    int tools = 0;
    int commands = 0;
    int failures = 0;
    int pendingTools = 0;
    int changedFiles = 0; // 仅统计有成功结果的改动工具，文件路径去重
    qint64 startedAtMs = 0;
    qint64 endedAtMs = 0;
    QString question;
    QString replyPreview;
};

// 增量 JSONL 解析器，保留可展示内容，不展开图片字节或加密思考数据。
class ClaudeSessionData {
public:
    // 追加原始字节；到达快照末尾时尝试解析无换行的完整末行，半行仍保留待续。
    void append(const QByteArray &bytes, bool endOfFile = false);
    // 接收一条已解码记录；首尾采样之间不连续时用此入口，避免拼接无关半行。
    void accept(const QJsonObject &record);
    const QList<ClaudeSessionEntry> &entries() const { return m_entries; }
    const QList<int> &turns() const { return m_turns; }
    QList<ClaudeSessionTurn> presentationTurns() const;
    const ClaudeSessionInfo &summary() const { return m_summary; }
    QStringList models() const { return m_models; }
    int toolCount() const { return m_toolCount; }
    int failureCount() const { return m_failureCount; }
    int malformedLines() const { return m_malformedLines; }
    bool hasIncompleteLine() const { return !m_pending.isEmpty(); }
    QString toMarkdown() const;

    // --- 展示清理与命令 / 路径辅助（均不执行外部命令） ---
    static QString contentText(const QJsonValue &content); // 文本原样提取，附件替换为占位说明
    static QString cleanMessage(const QString &text);     // 隐藏注入的元数据，原文仍保留供展开 / 复制
    static QString cleanPreview(const QString &text);
    static QString quote(const QString &text, bool powershell = false);
    // 验证 UUID 与工作目录后生成恢复命令；无效数据返回空串。
    static QString resumeCommand(const ClaudeSessionInfo &session, bool powershell = false);
    static bool validRelativePath(const QString &path);
    static bool matches(const ClaudeSessionInfo &session, const QString &query);
    static QString projectName(const QString &cwd);
    static QString relativeTime(qint64 timestampMs, qint64 nowMs = 0);
    static QString fenced(const QString &text, const QString &language = QString());

private:
    void parseLine(const QByteArray &line);
    void addEntry(ClaudeSessionEntry entry);
    void setTitle(const QString &title, int priority);
    QByteArray m_pending; // 尚未遇到换行的原始字节，不能提前解码 UTF-8
    bool m_skippingLine = false; // 超过大小上限后持续跳到下一个换行，避免无限积累
    QSet<QString> m_seen; // 按记录 UUID 去重，允许跨页或重复采样
    QHash<QString, int> m_tools; // tool_use.id → 调用条目索引，结果到达时原位补齐
    QHash<QString, QJsonObject> m_pendingResults; // 先于调用出现的结果，待调用到达后关联
    QList<ClaudeSessionEntry> m_entries;
    QList<int> m_turns; // 用户提问在 m_entries 中的起始索引
    ClaudeSessionInfo m_summary;
    QStringList m_models;
    int m_titlePriority = 0; // 首条提问 < summary < ai-title < custom-title
    int m_toolCount = 0;
    int m_failureCount = 0;
    int m_malformedLines = 0;
};

// 界面只读已发布快照；后台必须复制解析器再追加，不能通过别名修改已发布实例。
using ClaudeSessionSnapshot = std::shared_ptr<const ClaudeSessionData>;

} // namespace cubeshell
Q_DECLARE_METATYPE(cubeshell::ClaudeSessionSnapshot)
Q_DECLARE_METATYPE(cubeshell::ClaudeSessionInfo)
Q_DECLARE_METATYPE(QList<cubeshell::ClaudeSessionInfo>)
