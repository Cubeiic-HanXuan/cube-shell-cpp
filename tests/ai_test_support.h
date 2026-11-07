#pragma once

#include "ai/AiPreferences.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>

namespace aitest {
inline bool waitFor(const std::function<bool()> &predicate, int timeout = 6000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
inline void drain(int milliseconds = 80)
{
    QElapsedTimer timer; timer.start();
    waitFor([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 100);
}

struct Response {
    QByteArray body;
    QByteArray contentType = "text/event-stream";
    int status = 200;
    int chunkSize = 0;
    bool holdOpen = false;
};

class Server : public QTcpServer {
public:
    QList<QJsonObject> requests;
    std::function<Response(const QJsonObject &, int)> respond;
    Server()
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer = QByteArray(), handled = false]() mutable {
                    buffer += socket->readAll();
                    if (handled) return;
                    const int end = buffer.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto &line : buffer.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer.size() < end + 4 + length) return;
                    handled = true;
                    const auto body = QJsonDocument::fromJson(buffer.mid(end + 4, length)).object();
                    requests.append(body);
                    const auto response = respond ? respond(body, requests.size()) : Response{};
                    const auto headers = "HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: "
                        + response.contentType + "\r\nContent-Length: " + QByteArray::number(response.body.size() + (response.holdOpen ? 100 : 0))
                        + "\r\nConnection: close\r\n\r\n";
                    socket->write(headers);
                    if (!response.chunkSize) {
                        socket->write(response.body);
                        if (!response.holdOpen) socket->disconnectFromHost();
                    } else {
                        auto *timer = new QTimer(socket);
                        timer->setInterval(1);
                        connect(timer, &QTimer::timeout, socket, [socket, timer, response, offset = 0]() mutable {
                            socket->write(response.body.mid(offset, response.chunkSize));
                            offset += response.chunkSize;
                            if (offset >= response.body.size()) {
                                timer->stop();
                                if (!response.holdOpen) socket->disconnectFromHost();
                            }
                        });
                        timer->start();
                    }
                });
            }
        });
    }
    cubeshell::AiPreferences preferences(bool stream = true) const
    {
        cubeshell::AiPreferences prefs;
        prefs.provider = QStringLiteral("custom"); prefs.model = QStringLiteral("synthetic-model");
        prefs.baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(serverPort());
        prefs.thinkingEnabled = false; prefs.stream = stream;
        return prefs;
    }
};

inline QByteArray frame(const QJsonObject &delta, const QString &finish = {})
{
    return "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{
        QJsonObject{{"index", 0}, {"delta", delta}, {"finish_reason", finish.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(finish)}}
    }}}).toJson(QJsonDocument::Compact) + "\r\n\r\n";
}
inline Response text(const QString &text, bool stream = true, QString finish = QStringLiteral("stop"))
{
    if (stream) return {frame(QJsonObject{{"content", text}}, finish) + "data: [DONE]\n\n"};
    return {QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", finish},
        {"message", QJsonObject{{"role", "assistant"}, {"content", text}}}}}}}).toJson(QJsonDocument::Compact), "application/json"};
}
inline QJsonObject tool(const QString &id, const QString &command = QStringLiteral("printf fixture"), int index = 0)
{
    const auto arguments = QJsonDocument(QJsonObject{{"commands", QJsonArray{QJsonObject{{"cmd", command}, {"description", "fixture"}}}},
        {"explanation", "fixture plan"}}).toJson(QJsonDocument::Compact);
    return QJsonObject{{"id", id}, {"index", index}, {"type", "function"},
        {"function", QJsonObject{{"name", "execute_ssh_commands"}, {"arguments", QString::fromUtf8(arguments)}}}};
}
inline Response tools(const QJsonArray &calls)
{
    return {frame(QJsonObject{{"tool_calls", calls}}, QStringLiteral("tool_calls")) + "data: [DONE]\n\n"};
}
} // namespace aitest
