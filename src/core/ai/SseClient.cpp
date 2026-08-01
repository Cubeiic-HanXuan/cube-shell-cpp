// SseClient.cpp — see SseClient.h for the protocol notes.
//
// 对应Python: openai SDK 的 SSE 解码逻辑（cube-shell Python 侧通过
// `for chunk in client.chat.completions.create(stream=True)` 间接使用）。

#include "SseClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace cubeshell {

// ---------------------------------------------------------------------------
// SseEventParser
// ---------------------------------------------------------------------------

QList<SseEvent> SseEventParser::feed(const QByteArray &chunk)
{
    QList<SseEvent> out;
    if (m_done)
        return out;

    m_buffer.append(chunk);

    // Split off complete lines; the trailing partial line stays buffered.
    int start = 0;
    while (true) {
        const int nl = m_buffer.indexOf('\n', start);
        if (nl < 0)
            break;
        QByteArray line = m_buffer.mid(start, nl - start);
        if (line.endsWith('\r'))
            line.chop(1);                    // tolerate CRLF
        consumeLine(line, &out);
        start = nl + 1;
        if (m_done)
            break;
    }
    m_buffer.remove(0, start);
    return out;
}

QList<SseEvent> SseEventParser::finish()
{
    QList<SseEvent> out;
    if (m_done)
        return out;
    // Lenient flush: process a dangling last line without '\n', then
    // dispatch any half-open event.
    if (!m_buffer.isEmpty()) {
        QByteArray line = m_buffer;
        m_buffer.clear();
        if (line.endsWith('\r'))
            line.chop(1);
        consumeLine(line, &out);
    }
    if (!m_done)
        dispatchPending(&out);
    return out;
}

void SseEventParser::reset()
{
    m_buffer.clear();
    m_eventType.clear();
    m_dataLines.clear();
    m_lastId.clear();
    m_hasPendingData = false;
    m_done = false;
}

void SseEventParser::consumeLine(const QByteArray &line, QList<SseEvent> *out)
{
    if (line.isEmpty()) {                    // blank line => event complete
        dispatchPending(out);
        return;
    }
    if (line.startsWith(':'))                // comment line
        return;

    QByteArray field;
    QByteArray value;
    const int colon = line.indexOf(':');
    if (colon < 0) {
        field = line;                        // field with empty value
    } else {
        field = line.left(colon);
        value = line.mid(colon + 1);
        if (value.startsWith(' '))           // strip single leading space
            value.remove(0, 1);
    }

    if (field == "data") {
        m_dataLines.append(QString::fromUtf8(value));
        m_hasPendingData = true;
    } else if (field == "event") {
        m_eventType = QString::fromUtf8(value);
    } else if (field == "id") {
        m_lastId = QString::fromUtf8(value);
    }
    // unknown fields (e.g. "retry") are ignored
}

void SseEventParser::dispatchPending(QList<SseEvent> *out)
{
    if (!m_hasPendingData) {
        // WHATWG 规范：data 缓冲为空时不分发事件（含仅有 event: 字段的块
        // 与连续空行），但仍重置 event type。
        m_eventType.clear();
        m_dataLines.clear();
        return;
    }

    const QString data = m_dataLines.join(QLatin1Char('\n'));
    const QString type = m_eventType.isEmpty() ? QStringLiteral("message")
                                               : m_eventType;
    m_eventType.clear();
    m_dataLines.clear();
    m_hasPendingData = false;

    // OpenAI end-of-stream marker: swallow, mark done.
    if (data.trimmed() == QLatin1String("[DONE]")) {
        m_done = true;
        return;
    }

    SseEvent ev;
    ev.type = type;
    ev.data = data;
    ev.id = m_lastId;
    out->append(ev);
}

// ---------------------------------------------------------------------------
// SseClient
// ---------------------------------------------------------------------------

SseClient::SseClient(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
}

SseClient::~SseClient()
{
    abort();
}

void SseClient::post(const QUrl &url, const QByteArray &jsonBody,
                     const QList<QPair<QByteArray, QByteArray>> &headers,
                     int transferTimeoutMs)
{
    abort();

    m_parser.reset();
    m_errorBody.clear();
    m_httpError = false;
    m_aborted = false;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    for (const auto &h : headers)
        request.setRawHeader(h.first, h.second);
    request.setTransferTimeout(transferTimeoutMs);

    m_reply = m_manager->post(request, jsonBody);
    connect(m_reply, &QNetworkReply::readyRead,
            this, &SseClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished,
            this, &SseClient::onFinished);
}

void SseClient::abort()
{
    if (!m_reply)
        return;
    m_aborted = true;
    m_reply->disconnect(this);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
}

void SseClient::onReadyRead()
{
    if (!m_reply)
        return;

    // A 4xx/5xx response carries a JSON error object, not an SSE stream —
    // accumulate the body verbatim for the error message.
    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        m_httpError = true;
        m_errorBody.append(m_reply->readAll());
        return;
    }

    const QList<SseEvent> events = m_parser.feed(m_reply->readAll());
    for (const SseEvent &ev : events)
        emit eventReceived(ev.type, ev.data);
    if (m_parser.isDone() && m_reply) {
        // Stop reading further bytes; onFinished will emit streamFinished.
        m_reply->close();
    }
}

void SseClient::onFinished()
{
    if (!m_reply || m_aborted)
        return;

    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = m_reply->error();

    if (m_httpError || status >= 400) {
        m_errorBody.append(m_reply->readAll());
        QString msg = QStringLiteral("HTTP %1").arg(status);
        const QString body = QString::fromUtf8(m_errorBody).trimmed();
        if (!body.isEmpty())
            msg += QStringLiteral(": %1").arg(body.left(500));
        cleanupReply();
        emit streamError(msg);
        return;
    }

    // OperationCanceledError after close() following [DONE] is a normal end.
    if (netError != QNetworkReply::NoError
        && !(netError == QNetworkReply::OperationCanceledError
             && m_parser.isDone())) {
        const QString msg = m_reply->errorString();
        cleanupReply();
        emit streamError(msg);
        return;
    }

    // Flush any unterminated trailing event.
    const QList<SseEvent> events = m_parser.finish();
    cleanupReply();
    for (const SseEvent &ev : events)
        emit eventReceived(ev.type, ev.data);
    emit streamFinished();
}

void SseClient::cleanupReply()
{
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

} // namespace cubeshell
