#pragma once

// AiChatWorker.h — OpenAI 兼容协议的异步请求驱动，统一处理流式与非流式响应。
//
// 职责边界：只组装请求、解析响应并报告结果，不解释命令、不调度执行。
// 增量只供界面预览；每个已启动的请求（含取消）由 settle 恰好结算一次。
// 本类使用 Qt 异步网络事件，不另建工作线程；上层负责单请求串行调度。
#include "AiPreferences.h"
#include "AiInteractionTypes.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {
class SseClient;

class AiChatWorker : public QObject {
    Q_OBJECT
public:
    explicit AiChatWorker(QObject *parent = nullptr);
    ~AiChatWorker() override;

    // --- 请求配置（在 start 前设置，供下一次请求使用） ---
    void setPreferences(const AiPreferences &prefs) { m_prefs = prefs; }
    AiPreferences preferences() const { return m_prefs; }
    void setTools(const QJsonArray &tools) { m_tools = tools; }
    // 额外字段最后合并，可覆盖常规参数；总结模式的工具禁用规则仍优先。
    void setExtraBody(const QJsonObject &body) { m_extraBody = body; }
    // 总结阶段强制剔除所有工具字段，不能被额外请求参数重新打开执行能力。
    void setSummaryOnly(bool value) { m_summaryOnly = value; }

    // --- 请求生命周期 ---
    // 忙碌时忽略重复启动；配置错误可能在此调用内同步发送完成信号。
    void start(const QJsonArray &messages);
    // 取消等待并结算为 Cancelled；空闲时无操作，不冒充正常回复完成。
    void stop();
    bool isRunning() const { return m_running; }
    quint64 requestId() const { return m_requestId; }

    // 纯请求体构造，便于脱离网络测试；总结模式的字段剔除由 start 额外执行。
    static QJsonObject buildRequestBody(const AiPreferences &prefs,
                                       const QJsonArray &messages,
                                       const QJsonArray &tools,
                                       const QJsonObject &extraBody);
signals:
    // 流式正文与思考片段分开传递；非流式响应只通过最终结果回填正文。
    void deltaReceived(const QString &content, const QString &reasoning);
    // 所有结束路径的统一出口，携带请求 ID，供控制器过滤过期结果。
    void requestFinished(const cubeshell::AiRequestResult &result);
    // 兼容旧调用方的信号；取消不发送正常完成信号，上层以 requestFinished 为准。
    void finishedText(const QString &fullText);
    void completed(const QString &fullText, const QJsonArray &toolCalls,
                   const QString &finishReason);
    void failed(const QString &message);
private:
    // --- 协议解析与统一结算 ---
    void onSseEvent(const QString &type, const QString &data);
    void handleNonStreamResponse(const QByteArray &body);
    void finishResponse(); // 根据正文、工具参数、完成原因判定响应是否完整
    void settle(AiRequestOutcome outcome, const QString &error = {});
    void cancelTransport(); // 只释放传输资源，不再发送业务完成信号
    QJsonArray aggregatedToolCalls() const;

    AiPreferences m_prefs;
    QJsonArray m_tools;
    QJsonObject m_extraBody;
    SseClient *m_sse; // 由 QObject 父子关系管理，可重复用于多次请求
    QNetworkAccessManager *m_plainManager = nullptr; // 非流式路径按需创建
    QNetworkReply *m_plainReply = nullptr; // 当前非流式回复，结束后 deleteLater
    QTimer m_deadline; // 整次请求的总时限，与网络传输的空闲超时分别生效
    bool m_running = false; // settle 的一次性结算门闩，发信号前必须先复位
    bool m_summaryOnly = false;
    bool m_hadReasoning = false; // 区分无内容响应和仅有思考、没有最终正文的响应
    bool m_malformed = false; // 记住任一坏帧，避免把其余可解析片段误判为完整回复
    quint64 m_requestId = 0; // 每次实际启动递增，取消不会复用旧编号
    QString m_fullText;
    QString m_finishReason;
    // 以 SSE tool_calls.index 为键拼接 arguments；QMap 按索引输出工具调用。
    QMap<int, QJsonObject> m_toolCallAcc;
};
} // namespace cubeshell
