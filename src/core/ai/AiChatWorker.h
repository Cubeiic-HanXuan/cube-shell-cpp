#pragma once

// AiChatWorker.h — OpenAI-compatible chat completion request driver.
//
// 对应Python: core/ai/worker.py::AIChatWorker (QThread)
//
// Python 侧用 QThread + openai SDK 阻塞迭代；C++ 侧改为纯异步：
// QNetworkAccessManager(SseClient) 在本线程事件循环里驱动，逐 chunk 发信号，
// 无需工作线程。语义保持一致：
//   - deltaReceived(content, reasoning)  对应 delta_ready(reasoning, content)
//     （参数顺序按任务约定调整为 content 在前）
//   - finishedText(fullText)             对应 finished_text
//   - failed(message)                    对应 failed
//   - stop() 为"软停止"：中断网络流后仍发 finishedText(已收到的部分)，
//     与 worker.py::request_stop 后 break + finished_text 的行为一致。
//
// 额外支持（供 SshAiAgent 的 function calling 循环复用）：
//   - setTools(tools) 传入 OpenAI tools 数组，自动附带 tool_choice="auto"；
//   - 流式 tool_calls 增量按 index 聚合，完成后通过
//     completed(fullText, toolCalls, finishReason) 一并给出。

#include "AiPreferences.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {

class SseClient;

class AiChatWorker : public QObject {
    Q_OBJECT
public:
    explicit AiChatWorker(QObject *parent = nullptr);

    void setPreferences(const AiPreferences &prefs) { m_prefs = prefs; }
    AiPreferences preferences() const { return m_prefs; }

    // OpenAI tools 数组（空 = 不带工具）。
    // 对应Python: ssh_agent.py 调用 chat.completions.create(tools=TOOLS)
    void setTools(const QJsonArray &tools) { m_tools = tools; }

    // 额外顶层字段（透传合并进请求根对象）。
    // 对应Python: worker.py 的 extra_body 透传语义
    void setExtraBody(const QJsonObject &extraBody) { m_extraBody = extraBody; }

    // 发起一次请求；messages 为 OpenAI Chat Completions 消息数组。
    // 对应Python: AIChatWorker.run
    void start(const QJsonArray &messages);

    // 软停止：中断流，随后发 finishedText(部分文本)。
    // 对应Python: AIChatWorker.request_stop
    void stop();

    bool isRunning() const { return m_running; }

    // 请求 JSON 构建（纯函数，供单测）。
    // 对应Python: worker.py::run 的 call_kwargs 组装
    static QJsonObject buildRequestBody(const AiPreferences &prefs,
                                        const QJsonArray &messages,
                                        const QJsonArray &tools,
                                        const QJsonObject &extraBody);

signals:
    // 对应Python: delta_ready(reasoning, content) — 注意参数顺序为任务约定
    void deltaReceived(const QString &content, const QString &reasoning);
    // 对应Python: finished_text(full_text)
    void finishedText(const QString &fullText);
    // 聚合结果（fullText + 聚合后的 tool_calls + finish_reason）
    void completed(const QString &fullText, const QJsonArray &toolCalls,
                   const QString &finishReason);
    // 对应Python: failed(str)
    void failed(const QString &message);

private:
    void onSseEvent(const QString &type, const QString &data);
    void onSseFinished();
    void onSseError(const QString &message);
    void handleNonStreamResponse(const QByteArray &body);
    void finishStream();
    QJsonArray aggregatedToolCalls() const;

    AiPreferences m_prefs;
    QJsonArray m_tools;
    QJsonObject m_extraBody;

    SseClient *m_sse = nullptr;
    QNetworkAccessManager *m_plainManager = nullptr;   // 非流式路径
    QNetworkReply *m_plainReply = nullptr;
    bool m_running = false;
    bool m_stopRequested = false;

    QString m_fullText;
    QString m_finishReason;
    // 流式 tool_calls 增量聚合（index -> {id,name,arguments}）
    // 对应Python: ssh_agent.py 中按 tc.index 聚合 arguments 累加
    QMap<int, QJsonObject> m_toolCallAcc;
};

} // namespace cubeshell
