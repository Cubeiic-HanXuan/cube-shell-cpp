#pragma once

// SshKeepaliveTimer.h — periodic SSH keepalive sender.
//
// Runs a QTimer on the UI thread that periodically asks SshClient to emit an
// SSH_MSG_GLOBAL_REQUEST keepalive packet. If the transport has died (socket
// closed by peer or network), connectionDied is emitted.

#include <QObject>
#include <QString>

class QTimer;

namespace cubeshell {

class SshClient;

class SshKeepaliveTimer : public QObject {
    Q_OBJECT
public:
    explicit SshKeepaliveTimer(std::shared_ptr<SshClient> client, QObject *parent = nullptr);
    ~SshKeepaliveTimer() override;

    void start();
    void stop();
    bool isRunning() const;

signals:
    void connectionDied(const QString &reason);

private slots:
    void onTimeout();

private:
    std::shared_ptr<SshClient> m_client;
    QTimer *m_timer = nullptr;
};

} // namespace cubeshell
