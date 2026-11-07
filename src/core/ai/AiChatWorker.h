#pragma once

// Asynchronous OpenAI-compatible request driver. Deltas are previews; every
// request (including cancellation) has exactly one authoritative result.
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
    void setPreferences(const AiPreferences &prefs) { m_prefs = prefs; }
    AiPreferences preferences() const { return m_prefs; }
    void setTools(const QJsonArray &tools) { m_tools = tools; }
    void setExtraBody(const QJsonObject &body) { m_extraBody = body; }
    void setSummaryOnly(bool value) { m_summaryOnly = value; }
    void start(const QJsonArray &messages);
    void stop();
    bool isRunning() const { return m_running; }
    quint64 requestId() const { return m_requestId; }

    static QJsonObject buildRequestBody(const AiPreferences &prefs,
                                       const QJsonArray &messages,
                                       const QJsonArray &tools,
                                       const QJsonObject &extraBody);
signals:
    void deltaReceived(const QString &content, const QString &reasoning);
    void requestFinished(const cubeshell::AiRequestResult &result);
    // Compatibility signals: cancellation is never a normal completed result.
    void finishedText(const QString &fullText);
    void completed(const QString &fullText, const QJsonArray &toolCalls,
                   const QString &finishReason);
    void failed(const QString &message);
private:
    void onSseEvent(const QString &type, const QString &data);
    void handleNonStreamResponse(const QByteArray &body);
    void finishResponse();
    void settle(AiRequestOutcome outcome, const QString &error = {});
    void cancelTransport();
    QJsonArray aggregatedToolCalls() const;

    AiPreferences m_prefs;
    QJsonArray m_tools;
    QJsonObject m_extraBody;
    SseClient *m_sse;
    QNetworkAccessManager *m_plainManager = nullptr;
    QNetworkReply *m_plainReply = nullptr;
    QTimer m_deadline;
    bool m_running = false;
    bool m_summaryOnly = false;
    bool m_hadReasoning = false;
    bool m_malformed = false;
    quint64 m_requestId = 0;
    QString m_fullText;
    QString m_finishReason;
    QMap<int, QJsonObject> m_toolCallAcc;
};
} // namespace cubeshell
