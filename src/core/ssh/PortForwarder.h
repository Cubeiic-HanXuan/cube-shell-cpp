#pragma once

// PortForwarder.h — C++ port of core/forwarder.py's three forwarder thread
// types (LocalPortForwarder / RemotePortForwarder / DynamicPortForwarder).
//
// Each forwarder runs its accept/pump loops on a dedicated QThread worker and
// reports lifecycle events to the UI via Qt signals (queued). In Python these
// are threading.Thread subclasses sharing one paramiko Transport; here they
// share one cubeshell::SshClient (one LIBSSH2_SESSION).
//
// THREAD-SAFETY: libssh2 channels multiplexed over a single session are NOT
// safe for concurrent use. Every libssh2 call the forwarders make goes through
// SshClient's forwarding helpers, which serialize on SshClient::sessionLock()
// (a recursive mutex). The pump loop additionally uses select() on the raw TCP
// socket for the local side and a poll-and-yield for the SSH side so it never
// holds the session lock while sleeping.

#include <QObject>
#include <QThread>

#include <atomic>

#include "SshClient.h"

namespace cubeshell {

// 常量配置 (mirrors forwarder.py)
inline constexpr int kForwardBufferSize = 8192;   // BUFFER_SIZE
inline constexpr int kSocketTimeoutMs   = 1000;   // SOCKET_TIMEOUT (accept poll)
inline constexpr int kSelectTimeoutMs   = 1000;   // SELECT_TIMEOUT

// Base class for the three forwarders. Owns the worker thread and the stop
// flag; subclasses implement run() with their accept loop.
//
// 对应C++: LocalPortForwarder/RemotePortForwarder/DynamicPortForwarder 公共基类
class PortForwarder : public QObject {
    Q_OBJECT
public:
    // tunnelId: opaque id chosen by ForwarderManager (keyed in its map).
    // client:   a connected+authenticated SshClient (shared, NOT owned).
    PortForwarder(const QString &tunnelId, SshClient *client, QObject *parent = nullptr);
    ~PortForwarder() override;

    QString tunnelId() const { return m_tunnelId; }

    // Start the worker thread (calls run()).
    virtual void start();
    // Signal the loops to stop, close the listen socket, and join the thread.
    // Safe to call from any thread; idempotent.
    virtual void stop();

    bool isRunning() const { return m_running.load(); }

signals:
    // Emitted once the forwarder is bound/listening (or remote-forward granted).
    void started();
    // Emitted when the forwarder stops (for any reason — stop() or error).
    void stopped();
    // Human-readable progress / error line (queued to UI for logging).
    void logMessage(const QString &message);
    // A hard error occurred (e.g. bind failed, SSH transport died).
    void errorOccurred(const QString &message);

protected:
    // Subclass accept loop; executed on the worker thread.
    virtual void run() = 0;

    // Pump bytes both ways between a local TCP socket fd and an SSH channel
    // until either side closes or stop() is requested. Runs on a worker thread;
    // blocking. Used by all three forwarders' connection handlers.
    void pumpBidirectional(qintptr localSock, _LIBSSH2_CHANNEL *channel);

    QString m_tunnelId;
    SshClient *m_client; // shared, not owned

    QThread *m_thread = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};

// ---------------------------------------------------------------------------
// Local port forward: listen on localHost:localPort; for each accepted socket
// open a direct-tcpip channel to remoteHost:remotePort and pump bytes.
// 对应C++: class LocalPortForwarder(threading.Thread)
// ---------------------------------------------------------------------------
class LocalPortForwarder : public PortForwarder {
    Q_OBJECT
public:
    LocalPortForwarder(const QString &tunnelId, SshClient *client,
                       const QString &remoteHost, quint16 remotePort,
                       const QString &localHost, quint16 localPort,
                       QObject *parent = nullptr);

    void stop() override;

protected:
    void run() override;

private:
    QString m_remoteHost;
    quint16 m_remotePort;
    QString m_localHost;
    quint16 m_localPort;

    qintptr m_listenSock = -1;
};

// ---------------------------------------------------------------------------
// Remote port forward: ask the server to listen on remotePort and forward
// accepted channels to localHost:localPort (a target reachable from *this*
// machine).
// 对应C++: class RemotePortForwarder(threading.Thread)
// ---------------------------------------------------------------------------
class RemotePortForwarder : public PortForwarder {
    Q_OBJECT
public:
    // NOTE: the Python ctor signature is (…, local_host, local_port,
    // remote_host, remote_port) and it requests the forward on local_port then
    // connects to remote_host:remote_port — i.e. "local_port" is the port the
    // SERVER listens on and "remote_*" is the local target. We keep the Python
    // field semantics to stay behaviour-compatible, but name them clearly.
    RemotePortForwarder(const QString &tunnelId, SshClient *client,
                        const QString &targetHost, quint16 targetPort,
                        quint16 serverListenPort,
                        QObject *parent = nullptr);

    void stop() override;

protected:
    void run() override;

private:
    void handleConnection(_LIBSSH2_CHANNEL *channel);

    QString m_targetHost;   // where accepted connections are relayed TO (local side)
    quint16 m_targetPort;
    quint16 m_serverListenPort; // port the SSH server listens on

    _LIBSSH2_LISTENER *m_listener = nullptr;
};

// ---------------------------------------------------------------------------
// Dynamic port forward: local SOCKS5 proxy; each CONNECT is relayed over a
// direct-tcpip channel to the requested destination.
// 对应C++: class DynamicPortForwarder(threading.Thread)
// ---------------------------------------------------------------------------
class DynamicPortForwarder : public PortForwarder {
    Q_OBJECT
public:
    DynamicPortForwarder(const QString &tunnelId, SshClient *client,
                         const QString &localHost, quint16 localPort,
                         QObject *parent = nullptr);

    void stop() override;

protected:
    void run() override;

private:
    void handleClient(qintptr clientSock);
    bool socks5Handshake(qintptr clientSock);
    // Parses the CONNECT request; on success returns true and fills host/port.
    bool parseSocks5Request(qintptr clientSock, QString &host, quint16 &port);
    void sendSocks5Reply(qintptr clientSock, quint8 replyCode, quint16 port);

    QString m_localHost;
    quint16 m_localPort;

    qintptr m_serverSock = -1;
};

} // namespace cubeshell
