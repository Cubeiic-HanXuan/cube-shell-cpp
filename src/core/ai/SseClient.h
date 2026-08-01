#pragma once

// SseClient.h — generic Server-Sent Events (SSE) streaming client.
//
// Core infrastructure shared by the AI chat stack (AiChatWorker) and, in a
// later phase, the Hermes integration. The pure line-oriented protocol
// parser is split out as SseEventParser so it can be unit tested without any
// network (tests/sse_parser_test.cpp).
//
// 对应Python: openai SDK 内部的 SSE 流式解析（worker.py / ssh_agent.py 中
// `for chunk in response` 迭代的底层协议层）。Python 侧由 SDK 隐藏，C++ 侧
// 用 QNetworkAccessManager POST + readyRead 增量解析显式实现。
//
// Protocol handling (subset of the WHATWG EventSource spec that OpenAI
// compatible endpoints actually use):
//   - byte stream is buffered and split on '\n' ('\r\n' tolerated);
//   - "data:", "event:", "id:" field lines (single leading space stripped);
//   - lines starting with ':' are comments and ignored;
//   - an empty line terminates the pending event and dispatches it;
//   - multiple "data:" lines in one event are joined with '\n';
//   - a data payload of "[DONE]" is the OpenAI end-of-stream marker: it is
//     swallowed (not dispatched) and marks the stream as done;
//   - an incomplete trailing line stays in the buffer until more bytes or
//     finish() arrive.

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace cubeshell {

// One dispatched SSE event.
struct SseEvent {
    QString type;   // "event:" field; defaults to "message" when absent
    QString data;   // joined "data:" lines
    QString id;     // "id:" field (may be empty)
};

// Pure incremental SSE protocol parser (no I/O, unit-testable).
// 命名为 SseEventParser 以区分 hermes/HermesBackend.h 内联的 SseParser
// （Phase 5 并行开发，同属 cubeshell 命名空间）。
class SseEventParser {
public:
    // Feed a raw chunk from the wire; returns the events completed by it.
    QList<SseEvent> feed(const QByteArray &chunk);

    // Flush at end of stream: an unterminated trailing line/event is
    // dispatched (lenient towards servers that omit the final blank line).
    QList<SseEvent> finish();

    // True once the "[DONE]" sentinel has been seen.
    bool isDone() const { return m_done; }

    void reset();

private:
    void consumeLine(const QByteArray &line, QList<SseEvent> *out);
    void dispatchPending(QList<SseEvent> *out);

    QByteArray m_buffer;
    QString m_eventType;
    QStringList m_dataLines;
    QString m_lastId;
    bool m_hasPendingData = false;
    bool m_done = false;
};

// Streaming HTTP client: POST a JSON body, parse the SSE response
// incrementally and emit one signal per completed event.
class SseClient : public QObject {
    Q_OBJECT
public:
    explicit SseClient(QObject *parent = nullptr);
    ~SseClient() override;

    // Start a streaming POST. Any previous stream is aborted first.
    // headers: extra raw headers (e.g. {"Authorization", "Bearer ..."}).
    void post(const QUrl &url, const QByteArray &jsonBody,
              const QList<QPair<QByteArray, QByteArray>> &headers = {},
              int transferTimeoutMs = kDefaultTransferTimeoutMs);

    // Abort the running stream (streamFinished is NOT emitted; a silent
    // cancel, matching worker.py 的"软停止"语义).
    void abort();

    bool isActive() const { return m_reply != nullptr; }

    // No incoming bytes for this long aborts the request (network timeout).
    static constexpr int kDefaultTransferTimeoutMs = 120 * 1000;

signals:
    void eventReceived(const QString &type, const QString &data);
    void streamFinished();
    void streamError(const QString &message);

private:
    void onReadyRead();
    void onFinished();
    void cleanupReply();

    QNetworkAccessManager *m_manager;
    QNetworkReply *m_reply = nullptr;
    SseEventParser m_parser;
    QByteArray m_errorBody;      // body accumulated when HTTP status >= 400
    bool m_httpError = false;
    bool m_aborted = false;
};

} // namespace cubeshell
