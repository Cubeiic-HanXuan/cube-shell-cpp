// SshKeepaliveTimer.cpp — periodic SSH keepalive sender.

#include "SshKeepaliveTimer.h"

#include <QTimer>

#include "config/GlobalState.h"
#include "ssh/SshClient.h"

namespace cubeshell {

SshKeepaliveTimer::SshKeepaliveTimer(std::shared_ptr<SshClient> client, QObject *parent)
    : QObject(parent)
    , m_client(std::move(client))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &SshKeepaliveTimer::onTimeout);
}

SshKeepaliveTimer::~SshKeepaliveTimer()
{
    stop();
}

void SshKeepaliveTimer::start()
{
    if (!m_client || !m_client->isConnected())
        return;

    const GlobalState &state = GlobalState::instance();
    if (!state.sshKeepaliveEnabled())
        return;

    const int intervalMs = state.sshKeepaliveIntervalSeconds() * 1000;
    m_timer->start(intervalMs);
}

void SshKeepaliveTimer::stop()
{
    if (m_timer)
        m_timer->stop();
}

bool SshKeepaliveTimer::isRunning() const
{
    return m_timer && m_timer->isActive();
}

void SshKeepaliveTimer::onTimeout()
{
    if (!m_client || !m_client->isConnected()) {
        stop();
        emit connectionDied(QStringLiteral("连接已断开"));
        return;
    }

    SshError err;
    if (!m_client->sendKeepalive(err)) {
        stop();
        emit connectionDied(err.message.isEmpty()
                                ? QStringLiteral("SSH keepalive 发送失败")
                                : err.message);
    }
}

} // namespace cubeshell
