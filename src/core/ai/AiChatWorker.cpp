// AiChatWorker.cpp — see AiChatWorker.h.
//
// 对应Python: core/ai/worker.py::AIChatWorker

#include "AiChatWorker.h"

#include "SseClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace cubeshell {

AiChatWorker::AiChatWorker(QObject *parent)
    : QObject(parent)
    , m_sse(new SseClient(this))
{
    connect(m_sse, &SseClient::eventReceived,
            this, &AiChatWorker::onSseEvent);
    connect(m_sse, &SseClient::streamFinished,
            this, &AiChatWorker::onSseFinished);
    connect(m_sse, &SseClient::streamError,
            this, &AiChatWorker::onSseError);
}

// 对应Python: worker.py::run 的 call_kwargs 组装 + extra_body 合并
QJsonObject AiChatWorker::buildRequestBody(const AiPreferences &prefs,
                                           const QJsonArray &messages,
                                           const QJsonArray &tools,
                                           const QJsonObject &extraBody)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), prefs.model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), prefs.stream);
    body.insert(QStringLiteral("max_tokens"), prefs.maxTokens);
    body.insert(QStringLiteral("temperature"), prefs.temperature);

    // 对应Python: preset["supports_thinking"] and prefs.thinking_enabled
    const ProviderPreset preset = AiPreferences::providerPreset(prefs.provider);
    if (preset.supportsThinking && prefs.thinkingEnabled) {
        QJsonObject thinking;
        thinking.insert(QStringLiteral("type"), QStringLiteral("enabled"));
        body.insert(QStringLiteral("thinking"), thinking);
    }

    // 对应Python: ssh_agent.py 的 tools + tool_choice="auto"
    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }

    // extra_body 透传：合并进请求根（openai SDK 的 extra_body 语义）
    for (auto it = extraBody.constBegin(); it != extraBody.constEnd(); ++it)
        body.insert(it.key(), it.value());

    return body;
}

// 对应Python: AIChatWorker.run
void AiChatWorker::start(const QJsonArray &messages)
{
    if (m_running)
        stop();

    m_fullText.clear();
    m_finishReason.clear();
    m_toolCallAcc.clear();
    m_stopRequested = false;

    const QString apiKey = m_prefs.apiKey();
    if (apiKey.isEmpty()) {
        // 对应Python: "未配置 API Key，请在「设置 -> AI 设置」中配置"
        emit failed(QStringLiteral("未配置 API Key，请在「设置 -> AI 设置」中配置"));
        return;
    }

    const QJsonObject bodyObj =
        buildRequestBody(m_prefs, messages, m_tools, m_extraBody);
    const QByteArray body = QJsonDocument(bodyObj).toJson(QJsonDocument::Compact);
    const QList<QPair<QByteArray, QByteArray>> headers = {
        {QByteArrayLiteral("Authorization"),
         QByteArrayLiteral("Bearer ") + apiKey.toUtf8()},
    };
    const QUrl url(m_prefs.chatCompletionsUrl());

    m_running = true;

    if (m_prefs.stream) {
        m_sse->post(url, body, headers);
        return;
    }

    // 非流式：普通 POST，一次性解析响应 JSON。
    // 对应Python: run 的 else 分支（response.choices[0].message.content）
    if (!m_plainManager)
        m_plainManager = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    for (const auto &h : headers)
        request.setRawHeader(h.first, h.second);
    request.setTransferTimeout(SseClient::kDefaultTransferTimeoutMs);
    m_plainReply = m_plainManager->post(request, body);
    connect(m_plainReply, &QNetworkReply::finished, this, [this]() {
        if (!m_plainReply)
            return;
        QNetworkReply *reply = m_plainReply;
        m_plainReply = nullptr;
        reply->deleteLater();
        m_running = false;
        if (m_stopRequested)
            return;
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            QString msg = (status > 0)
                ? QStringLiteral("HTTP %1: %2").arg(status)
                      .arg(QString::fromUtf8(payload).trimmed().left(500))
                : reply->errorString();
            emit failed(msg);
            return;
        }
        handleNonStreamResponse(payload);
    });
}

// 对应Python: AIChatWorker.request_stop（软停止后仍发 finished_text）
void AiChatWorker::stop()
{
    if (!m_running)
        return;
    m_stopRequested = true;
    m_sse->abort();
    if (m_plainReply) {
        m_plainReply->disconnect(this);
        m_plainReply->abort();
        m_plainReply->deleteLater();
        m_plainReply = nullptr;
    }
    m_running = false;
    // Python: 循环 break 后仍 emit finished_text(full_text)
    emit finishedText(m_fullText);
    emit completed(m_fullText, aggregatedToolCalls(), m_finishReason);
}

// 对应Python: run 的 `for chunk in response` 逐 chunk 解析
void AiChatWorker::onSseEvent(const QString &type, const QString &data)
{
    Q_UNUSED(type);
    if (m_stopRequested)
        return;

    // 对应Python: try/except pass — 单块 JSON 解析失败静默跳过
    QJsonParseError parseError{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(data.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject obj = doc.object();

    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;                        // usage-only chunk 等，忽略
    const QJsonObject choice = choices.at(0).toObject();

    // finish_reason 检测（"stop" / "tool_calls" / "length" ...）
    const QJsonValue finishVal = choice.value(QStringLiteral("finish_reason"));
    if (finishVal.isString() && !finishVal.toString().isEmpty())
        m_finishReason = finishVal.toString();

    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
    const QString content = delta.value(QStringLiteral("content")).toString();
    const QString reasoning =
        delta.value(QStringLiteral("reasoning_content")).toString();

    // 流式 tool_calls 增量按 index 聚合
    // 对应Python: ssh_agent.py 中 tc.index / id / function.name /
    //             function.arguments 累加
    const QJsonArray toolCalls =
        delta.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &v : toolCalls) {
        const QJsonObject tc = v.toObject();
        const int index = tc.value(QStringLiteral("index")).toInt(0);
        QJsonObject acc = m_toolCallAcc.value(index);
        const QString id = tc.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            acc.insert(QStringLiteral("id"), id);
        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            acc.insert(QStringLiteral("name"), name);
        const QString args = fn.value(QStringLiteral("arguments")).toString();
        if (!args.isEmpty()) {
            acc.insert(QStringLiteral("arguments"),
                       acc.value(QStringLiteral("arguments")).toString() + args);
        }
        m_toolCallAcc.insert(index, acc);
    }

    if (!content.isEmpty() || !reasoning.isEmpty()) {
        m_fullText += content;
        emit deltaReceived(content, reasoning);
    }
}

void AiChatWorker::onSseFinished()
{
    if (m_stopRequested)
        return;
    finishStream();
}

void AiChatWorker::onSseError(const QString &message)
{
    m_running = false;
    if (m_stopRequested)
        return;
    // 对应Python: except 分支 failed.emit(str(e))
    emit failed(message);
}

void AiChatWorker::finishStream()
{
    m_running = false;
    emit finishedText(m_fullText);
    emit completed(m_fullText, aggregatedToolCalls(), m_finishReason);
}

// 对应Python: run 非流式分支 response.choices[0].message.content
void AiChatWorker::handleNonStreamResponse(const QByteArray &body)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(QStringLiteral("响应 JSON 解析失败: %1")
                        .arg(parseError.errorString()));
        return;
    }
    const QJsonObject obj = doc.object();
    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    const QJsonObject choice = choices.isEmpty() ? QJsonObject()
                                                 : choices.at(0).toObject();
    m_finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
    m_fullText = message.value(QStringLiteral("content")).toString();

    // 非流式的 tool_calls 直接取完整数组
    const QJsonArray toolCalls =
        message.value(QStringLiteral("tool_calls")).toArray();
    for (int i = 0; i < toolCalls.size(); ++i) {
        const QJsonObject tc = toolCalls.at(i).toObject();
        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        QJsonObject acc;
        acc.insert(QStringLiteral("id"), tc.value(QStringLiteral("id")));
        acc.insert(QStringLiteral("name"), fn.value(QStringLiteral("name")));
        acc.insert(QStringLiteral("arguments"),
                   fn.value(QStringLiteral("arguments")));
        m_toolCallAcc.insert(i, acc);
    }

    emit finishedText(m_fullText);
    emit completed(m_fullText, aggregatedToolCalls(), m_finishReason);
}

QJsonArray AiChatWorker::aggregatedToolCalls() const
{
    QJsonArray arr;
    for (auto it = m_toolCallAcc.constBegin(); it != m_toolCallAcc.constEnd();
         ++it)
        arr.append(it.value());
    return arr;
}

} // namespace cubeshell
