// ForwarderManager.cpp — C++ port of core/forwarder.py's ForwarderManager.

#include "ForwarderManager.h"

#include <QLoggingCategory>

#include "PortForwarder.h"

#ifndef Q_OS_WIN
#  include <unistd.h> // getuid
#endif

Q_LOGGING_CATEGORY(fwdLogMgr, "cubeshell.forwarder.manager")

namespace cubeshell {

ForwarderManager::ForwarderManager(QObject *parent)
    : QObject(parent)
{
}

ForwarderManager::~ForwarderManager()
{
    stopAll();
}

int ForwarderManager::tunnelCount() const
{
    QMutexLocker locker(&m_lock);
    return m_tunnels.size();
}

void ForwarderManager::addTunnel(const QString &tunnelId, PortForwarder *tunnel)
{
    QMutexLocker locker(&m_lock);
    m_tunnels.insert(tunnelId, tunnel);
}

void ForwarderManager::removeTunnel(const QString &tunnelId)
{
    PortForwarder *tunnel = nullptr;
    {
        QMutexLocker locker(&m_lock);
        auto it = m_tunnels.find(tunnelId);
        if (it == m_tunnels.end())
            return;
        tunnel = it.value();
        m_tunnels.erase(it);
    }
    tunnel->stop();               // joins the worker; emits stopped() -> closes client
    tunnel->deleteLater();
}

bool ForwarderManager::isTunnelAlive(const QString &tunnelId)
{
    QMutexLocker locker(&m_lock);
    auto it = m_tunnels.find(tunnelId);
    if (it == m_tunnels.end())
        return false;
    Q_UNUSED(it);
    return true; // reachability is implied by registration; per-client liveness
                 // is checked inside each forwarder loop via isTransportAlive().
}

void ForwarderManager::stopAll()
{
    QList<PortForwarder *> tunnels;
    {
        QMutexLocker locker(&m_lock);
        tunnels = m_tunnels.values();
        m_tunnels.clear();
    }
    // Each stop() emits stopped(), whose handler closes the tunnel's client.
    for (PortForwarder *tunnel : tunnels) {
        tunnel->stop();
        tunnel->deleteLater();
    }
}

SshClient *ForwarderManager::connectClient(const TunnelSpec &spec, QString &errorMessage)
{
    // The forwarder owns its own SshClient (one per tunnel, matching
    // forwarder.py which creates a fresh paramiko.SSHClient per tunnel).
    auto *client = new SshClient();
    client->setHost(spec.sshHost, spec.sshPort);
    client->setUsername(spec.sshUser);
    client->setPassword(spec.sshPassword);
    if (!spec.keyFile.isEmpty())
        client->setPrivateKey(spec.keyType, spec.keyFile, spec.keyPassphrase);

    // No interactive MFA prompt here — forwarders use password/key auth only
    // (forwarder.py passes no MFA callback either). Agent / default-key login is
    // not replicated (libssh2 supports agent auth separately; out of scope).
    SshError err;
    if (!client->connectToHost(/*promptCallback*/ nullptr, err)) {
        if (err.authFailed)
            errorMessage = QStringLiteral("SSH authentication failed — check username/password/key: %1")
                               .arg(err.message);
        else
            errorMessage = QStringLiteral("Cannot connect to SSH server %1:%2: %3")
                               .arg(spec.sshHost).arg(spec.sshPort).arg(err.message);
        client->deleteLater();
        return nullptr;
    }
    return client;
}

PortForwarder *ForwarderManager::startTunnel(const QString &tunnelId, const TunnelSpec &spec,
                                             QString &errorMessage)
{
    // Privileged-port check (POSIX only), mirroring start_tunnel().
#ifndef Q_OS_WIN
    if (spec.localPort < 1024 && ::getuid() != 0) {
        errorMessage = QStringLiteral(
            "Binding port %1 requires root privileges.\n"
            "Please use a port > 1024 (e.g. 1080, 8080), or run with sudo.")
                           .arg(spec.localPort);
        return nullptr;
    }
#endif

    SshClient *client = connectClient(spec, errorMessage);
    if (!client)
        return nullptr;

    // Build the right forwarder. Note the Python remote-forwarder argument
    // mapping: it requests the server listen on local_port and relays accepted
    // connections to remote_host:remote_port. We pass them through explicitly.
    PortForwarder *tunnel = nullptr;
    switch (spec.type) {
    case TunnelType::Local:
        tunnel = new LocalPortForwarder(tunnelId, client,
                                        spec.remoteHost, spec.remotePort,
                                        spec.localHost, spec.localPort);
        break;
    case TunnelType::Remote:
        tunnel = new RemotePortForwarder(tunnelId, client,
                                         spec.remoteHost, spec.remotePort, // local target
                                         spec.localPort);                  // server listen port
        break;
    case TunnelType::Dynamic:
        tunnel = new DynamicPortForwarder(tunnelId, client,
                                          spec.localHost, spec.localPort);
        break;
    }

    if (!tunnel) {
        errorMessage = QStringLiteral("Invalid tunnel type.");
        client->deleteLater();
        return nullptr;
    }

    // Wire up relayed signals (queued to the UI thread).
    connect(tunnel, &PortForwarder::started, this,
            [this, tunnelId]() { emit tunnelStarted(tunnelId); }, Qt::QueuedConnection);
    connect(tunnel, &PortForwarder::errorOccurred, this,
            [this, tunnelId](const QString &msg) { emit tunnelError(tunnelId, msg); },
            Qt::QueuedConnection);
    connect(tunnel, &PortForwarder::logMessage, this,
            [this, tunnelId](const QString &msg) { emit tunnelLog(tunnelId, msg); },
            Qt::QueuedConnection);
    // On stop, drop the registration (idempotent) and release the client.
    // The tunnel object itself is owned/destroyed by removeTunnel()/stopAll().
    connect(tunnel, &PortForwarder::stopped, this,
            [this, tunnelId, client]() {
                {
                    QMutexLocker locker(&m_lock);
                    m_tunnels.remove(tunnelId);
                }
                emit tunnelStopped(tunnelId);
                closeClientIfUnusedUnsafe(client);
            },
            Qt::QueuedConnection);

    {
        QMutexLocker locker(&m_lock);
        m_tunnels.insert(tunnelId, tunnel);
        m_clientRefs.insert(client, m_clientRefs.value(client, 0) + 1);
    }

    tunnel->start();
    return tunnel;
}

void ForwarderManager::closeClientIfUnusedUnsafe(SshClient *client)
{
    if (!client)
        return;
    // Decrement the refcount and close the client when the last tunnel using it
    // goes away (mirrors _close_ssh_client_unsafe). Clients we did not create
    // (registered via addTunnel) are not tracked and are left alone.
    bool lastUser = false;
    {
        QMutexLocker locker(&m_lock);
        auto it = m_clientRefs.find(client);
        if (it == m_clientRefs.end())
            return; // not ours
        it.value() -= 1;
        if (it.value() <= 0) {
            m_clientRefs.erase(it);
            lastUser = true;
        }
    }
    if (lastUser) {
        client->disconnectFromHost();
        client->deleteLater();
    }
}

} // namespace cubeshell
