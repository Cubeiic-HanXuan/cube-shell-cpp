#pragma once

// 串口多行发送队列。只控制发送节奏，不解释命令，也不推断设备执行结果。

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVector>

namespace cubeshell {

class SerialBridge;
class SerialClient;

class SerialLineSender : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Automatic, Paused };
    Q_ENUM(State)
    enum class LineState { Pending, Sending, Sent, Uncertain };
    Q_ENUM(LineState)
    struct Line {
        QString text;
        LineState state = LineState::Pending;
    };

    static constexpr int kMaxTextCharacters = 1024 * 1024;
    static constexpr int kMaxLines = 2000;
    static QStringList splitLines(QString text);

    SerialLineSender(SerialClient *client, SerialBridge *bridge, QObject *parent = nullptr);
    ~SerialLineSender() override;

    const QVector<Line> &lines() const { return m_lines; }
    State state() const { return m_state; }
    QString message() const { return m_message; }
    bool isConnected() const;
    bool isBusy() const;
    bool canSendManually() const;
    int nextPendingLine() const;
    int sentCount() const;

    bool setText(const QString &text);
    bool setLineText(int index, const QString &text);
    bool sendLine(int index);
    bool sendNext();
    bool startAutomatic(int intervalMs);
    void pause();
    void resume();
    void stop();

signals:
    void changed();

private:
    bool submitLine(int index);
    void advance();
    void onBytesWritten();
    void abort(const QString &message);
    bool reject(const QString &message);

    QPointer<SerialClient> m_client;
    QPointer<SerialBridge> m_bridge;
    QVector<Line> m_lines;
    State m_state = State::Idle;
    QTimer m_timer;
    int m_intervalMs = 200;
    int m_inFlight = -1;
    bool m_submitting = false;
    bool m_waitingForDrain = false;
    QString m_message;
};

} // namespace cubeshell
