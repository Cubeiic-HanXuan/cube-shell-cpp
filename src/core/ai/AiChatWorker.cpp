#include "AiChatWorker.h"
#include "SseClient.h"
#include "SensitiveDataMasker.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace cubeshell {

AiChatWorker::AiChatWorker(QObject *parent)
    : QObject(parent), m_sse(new SseClient(this))
{
    connect(m_sse, &SseClient::eventReceived, this, &AiChatWorker::onSseEvent);
    connect(m_sse, &SseClient::streamFinished, this, &AiChatWorker::finishResponse);
    connect(m_sse, &SseClient::jsonReceived, this, &AiChatWorker::handleNonStreamResponse);
    connect(m_sse, &SseClient::streamError, this, [this](const QString &error) {
        settle(AiRequestOutcome::Failed, error);
    });
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        settle(AiRequestOutcome::Failed, tr("模型请求超时，未取得完整回复"));
    });
}

AiChatWorker::~AiChatWorker()
{
    m_running = false;
    cancelTransport();
}

QJsonObject AiChatWorker::buildRequestBody(const AiPreferences &prefs,
                                          const QJsonArray &messages,
                                          const QJsonArray &tools,
                                          const QJsonObject &extraBody)
{
    QJsonObject body{{QStringLiteral("model"), prefs.model},
                     {QStringLiteral("messages"), messages},
                     {QStringLiteral("stream"), prefs.stream},
                     {QStringLiteral("max_tokens"), prefs.maxTokens},
                     {QStringLiteral("temperature"), prefs.temperature}};
    const auto preset = AiPreferences::providerPreset(prefs.provider);
    if (preset.supportsThinking && prefs.thinkingEnabled)
        body.insert(QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"), QStringLiteral("enabled")}});
    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }
    for (auto it = extraBody.constBegin(); it != extraBody.constEnd(); ++it)
        body.insert(it.key(), it.value());
    return body;
}

void AiChatWorker::start(const QJsonArray &messages)
{
    // The controller owns single-flight scheduling. Never complete an old task
    // synchronously just because another caller wants to replace its request.
    if (m_running) return;
    ++m_requestId;
    m_running = true;
    m_fullText.clear();
    m_finishReason.clear();
    m_toolCallAcc.clear();
    m_hadReasoning = false;
    m_malformed = false;
    const QString key = m_prefs.apiKey();
    if (key.isEmpty()) {
        settle(AiRequestOutcome::Failed, tr("未配置 API Key，请在「设置 → AI 设置」中配置"));
        return;
    }
    QJsonObject body = buildRequestBody(m_prefs, messages, m_tools, m_extraBody);
    if (m_summaryOnly) {
        // Enforce this AFTER extraBody; a preset cannot re-enable execution.
        for (const auto &field : {"tools", "tool_choice", "parallel_tool_calls", "functions", "function_call"})
            body.remove(QLatin1String(field));
    }
    const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QList<QPair<QByteArray, QByteArray>> headers = {{"Authorization", "Bearer " + key.toUtf8()}};
    m_deadline.start(m_summaryOnly ? 120000 : 600000);
    if (m_prefs.stream) {
        m_sse->post(QUrl(m_prefs.chatCompletionsUrl()), bytes, headers);
        return;
    }
    if (!m_plainManager) m_plainManager = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(m_prefs.chatCompletionsUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    for (const auto &header : headers) request.setRawHeader(header.first, header.second);
    request.setTransferTimeout(SseClient::kDefaultTransferTimeoutMs);
    auto *reply = m_plainManager->post(request, bytes);
    m_plainReply = reply;
    const quint64 id = m_requestId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
        if (id != m_requestId || reply != m_plainReply || !m_running) return;
        m_plainReply = nullptr;
        const auto payload = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
        const QString error = status > 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(payload).left(500))
                                         : reply->errorString();
        reply->deleteLater();
        if (failed) settle(AiRequestOutcome::Failed, error);
        else handleNonStreamResponse(payload);
    });
}

void AiChatWorker::cancelTransport()
{
    m_deadline.stop();
    m_sse->abort();
    if (m_plainReply) {
        m_plainReply->disconnect(this);
        m_plainReply->abort();
        m_plainReply->deleteLater();
        m_plainReply = nullptr;
    }
}

void AiChatWorker::stop()
{
    settle(AiRequestOutcome::Cancelled, tr("已停止等待模型回复"));
}

void AiChatWorker::onSseEvent(const QString &type, const QString &data)
{
    if (!m_running) return;
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(data.toUtf8(), &error);
    if (type == QLatin1String("error") || (doc.isObject() && doc.object().contains(QStringLiteral("error")))) {
        settle(AiRequestOutcome::Failed, tr("模型服务返回错误：%1").arg(data.left(500)));
        return;
    }
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        m_malformed = true;
        return;
    }
    const auto choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) return; // usage/heartbeat frames are not answers
    const auto choice = choices.first().toObject();
    const QString reason = choice.value(QStringLiteral("finish_reason")).toString();
    if (!reason.isEmpty()) m_finishReason = reason;
    const auto delta = choice.value(QStringLiteral("delta")).toObject();
    const QString content = delta.value(QStringLiteral("content")).toString();
    const QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
    for (const auto &value : delta.value(QStringLiteral("tool_calls")).toArray()) {
        const auto call = value.toObject();
        const int index = call.value(QStringLiteral("index")).toInt(-1);
        if (index < 0) { m_malformed = true; continue; }
        auto acc = m_toolCallAcc.value(index);
        const auto function = call.value(QStringLiteral("function")).toObject();
        if (!call.value(QStringLiteral("id")).toString().isEmpty())
            acc.insert(QStringLiteral("id"), call.value(QStringLiteral("id")));
        if (!function.value(QStringLiteral("name")).toString().isEmpty())
            acc.insert(QStringLiteral("name"), function.value(QStringLiteral("name")));
        acc.insert(QStringLiteral("arguments"), acc.value(QStringLiteral("arguments")).toString()
                   + function.value(QStringLiteral("arguments")).toString());
        m_toolCallAcc.insert(index, acc);
    }
    m_fullText += content;
    m_hadReasoning |= !reasoning.isEmpty();
    if (!content.isEmpty() || !reasoning.isEmpty()) emit deltaReceived(content, reasoning);
}

void AiChatWorker::handleNonStreamResponse(const QByteArray &bytes)
{
    if (!m_running) return;
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (!doc.isObject() || error.error != QJsonParseError::NoError) {
        settle(AiRequestOutcome::Failed, tr("模型响应 JSON 格式无效"));
        return;
    }
    if (doc.object().contains(QStringLiteral("error"))) {
        settle(AiRequestOutcome::Failed, tr("模型服务返回错误：%1").arg(QString::fromUtf8(bytes).left(500)));
        return;
    }
    const auto choices = doc.object().value(QStringLiteral("choices")).toArray();
    const auto choice = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    m_finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    const auto message = choice.value(QStringLiteral("message")).toObject();
    m_fullText = message.value(QStringLiteral("content")).toString();
    m_hadReasoning = !message.value(QStringLiteral("reasoning_content")).toString().isEmpty();
    int index = 0;
    for (const auto &value : message.value(QStringLiteral("tool_calls")).toArray()) {
        const auto call = value.toObject();
        const auto function = call.value(QStringLiteral("function")).toObject();
        m_toolCallAcc.insert(index++, QJsonObject{{QStringLiteral("id"), call.value(QStringLiteral("id"))},
            {QStringLiteral("name"), function.value(QStringLiteral("name"))},
            {QStringLiteral("arguments"), function.value(QStringLiteral("arguments"))}});
    }
    finishResponse();
}

void AiChatWorker::finishResponse()
{
    if (!m_running) return;
    if (m_finishReason == QLatin1String("length")) {
        settle(AiRequestOutcome::Truncated, tr("回复达到输出上限，内容未完成"));
    } else if (m_malformed || m_finishReason == QLatin1String("content_filter")
               || m_finishReason == QLatin1String("refusal")) {
        settle(AiRequestOutcome::Failed, tr("模型回复不完整或被服务端中止（%1）").arg(m_finishReason));
    } else if (!m_finishReason.isEmpty() && m_finishReason != QLatin1String("stop")
               && m_finishReason != QLatin1String("tool_calls")) {
        settle(AiRequestOutcome::Failed, tr("无法确认模型回复的完成原因：%1").arg(m_finishReason));
    } else if (m_fullText.trimmed().isEmpty() && m_toolCallAcc.isEmpty()) {
        settle(AiRequestOutcome::Empty, m_hadReasoning ? tr("模型仅返回思考内容，没有最终正文")
                                                     : tr("模型没有返回有效正文或工具调用"));
    } else if (m_finishReason == QLatin1String("tool_calls") && m_toolCallAcc.isEmpty()) {
        settle(AiRequestOutcome::Failed, tr("模型声明工具调用结束，但没有返回工具参数"));
    } else {
        settle(AiRequestOutcome::Complete);
    }
}

void AiChatWorker::settle(AiRequestOutcome outcome, const QString &error)
{
    if (!m_running) return;
    AiRequestResult result;
    result.requestId = m_requestId;
    result.text = m_fullText;
    result.toolCalls = aggregatedToolCalls();
    result.finishReason = m_finishReason;
    result.outcome = outcome;
    result.error = SensitiveDataMasker::mask(error);
    m_running = false;
    cancelTransport();
    // Snapshot all state before emitting. Subscribers may start another request.
    if (outcome == AiRequestOutcome::Complete) {
        emit finishedText(result.text);
        emit completed(result.text, result.toolCalls, result.finishReason);
    } else if (outcome != AiRequestOutcome::Cancelled) {
        emit failed(result.error);
    }
    emit requestFinished(result);
}

QJsonArray AiChatWorker::aggregatedToolCalls() const
{
    QJsonArray calls;
    for (auto it = m_toolCallAcc.constBegin(); it != m_toolCallAcc.constEnd(); ++it) calls.append(it.value());
    return calls;
}

} // namespace cubeshell
