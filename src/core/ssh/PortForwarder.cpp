// PortForwarder.cpp — C++ port of core/forwarder.py's forwarder threads.
// See PortForwarder.h for the design and thread-safety notes.

#include "PortForwarder.h"

#include <QLoggingCategory>

#include <libssh2.h>

#include <cstring>

#include "net/SocketUtil.h"

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(fwdLog)
Q_LOGGING_CATEGORY(fwdLog, "cubeshell.forwarder")

namespace cubeshell {

// closeSocket / closeFd / recvSome / makeListenSocket / connectTcp 原本是本文件
// 匿名 namespace 里的私有件，现已提取到 net/SocketUtil.h 与 SSH 代理共用
// （代理要用同一套东西在真 socket 上就地完成 HTTP CONNECT / SOCKS5 握手）。
// 行为不变，只是 connectTcp 现在带超时——原来那个是裸阻塞 ::connect，
// 目标主机半死时会把转发线程钉在内核 SYN 重传上（macOS 约 75 秒）。
using namespace socket_util;

// ===========================================================================
// PortForwarder (base)
// ===========================================================================

PortForwarder::PortForwarder(const QString &tunnelId, SshClient *client, QObject *parent)
    : QObject(parent)
    , m_tunnelId(tunnelId)
    , m_client(client)
{
}

PortForwarder::~PortForwarder()
{
    stop();
}

void PortForwarder::start()
{
    if (m_running.load())
        return;
    m_stopRequested = false;
    m_running = true;
    m_thread = QThread::create([this]() { run(); });
    m_thread->start();
}

void PortForwarder::stop()
{
    m_stopRequested = true;
    if (m_thread) {
        if (!m_thread->wait(3000))
            qCWarning(fwdLog) << "forwarder thread did not stop in time:" << m_tunnelId;
        m_thread->deleteLater();
        m_thread = nullptr;
    }
    m_running = false;
}

// Pump bytes between a local TCP socket and an SSH channel until EOF/error.
// The SSH side is non-blocking; the local side is polled with select().
void PortForwarder::pumpBidirectional(qintptr localSock, _LIBSSH2_CHANNEL *channel)
{
    while (!m_stopRequested.load()) {
        if (!m_client || !m_client->isTransportAlive())
            break;

        // Wait for the local socket to be readable (bounded so we notice stop).
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(localSock, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; // 100ms
        int rc = ::select(int(localSock) + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0)
            break; // socket closed

        // local -> channel
        if (rc > 0 && FD_ISSET(localSock, &rfds)) {
            char buf[kForwardBufferSize];
            ssize_t n = ::recv(localSock, buf, sizeof(buf), 0);
            if (n <= 0)
                break; // client closed / error
            if (m_client->channelWrite(channel, QByteArray(buf, int(n))) < 0)
                break;
        }

        // channel -> local (non-blocking read; drain whatever is available)
        for (;;) {
            bool wouldBlock = false;
            QByteArray data = m_client->channelRead(channel, kForwardBufferSize, &wouldBlock);
            if (wouldBlock)
                break;
            if (data.isEmpty()) {
                // EOF or hard error on the channel.
                if (m_client->channelEof(channel))
                    return;
                return;
            }
            ssize_t off = 0;
            while (off < data.size()) {
                ssize_t w = ::send(localSock, data.constData() + off, data.size() - off, 0);
                if (w <= 0)
                    return;
                off += w;
            }
        }
    }
}

// ===========================================================================
// LocalPortForwarder
// ===========================================================================

LocalPortForwarder::LocalPortForwarder(const QString &tunnelId, SshClient *client,
                                       const QString &remoteHost, quint16 remotePort,
                                       const QString &localHost, quint16 localPort,
                                       QObject *parent)
    : PortForwarder(tunnelId, client, parent)
    , m_remoteHost(remoteHost)
    , m_remotePort(remotePort)
    , m_localHost(localHost)
    , m_localPort(localPort)
{
}

void LocalPortForwarder::stop()
{
    m_stopRequested = true;
    // Closing the listen socket breaks a blocking accept().
    closeSocket(m_listenSock);
    PortForwarder::stop();
}

void LocalPortForwarder::run()
{
    QString err;
    m_listenSock = makeListenSocket(m_localHost, m_localPort, err);
    if (m_listenSock < 0) {
        qCWarning(fwdLog) << err;
        emit errorOccurred(err);
        emit stopped();
        return;
    }

    emit logMessage(QStringLiteral("Local forward listening on %1:%2 -> %3:%4")
                        .arg(m_localHost).arg(m_localPort).arg(m_remoteHost).arg(m_remotePort));
    emit started();

    while (!m_stopRequested.load()) {
        // Bounded accept via select so we can notice stop().
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_listenSock, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        int rc = ::select(int(m_listenSock) + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0)
            break; // listen socket closed by stop()
        if (rc == 0)
            continue; // timeout, re-check stop flag

        struct sockaddr_storage addr{};
        socklen_t addrLen = sizeof(addr);
        qintptr clientSock = ::accept(m_listenSock, reinterpret_cast<struct sockaddr *>(&addr), &addrLen);
        if (clientSock < 0) {
            if (m_stopRequested.load())
                break;
            continue;
        }

        // Transport alive? Python closes the connection and stops on dead transport.
        if (!m_client || !m_client->isTransportAlive()) {
            emit errorOccurred(QStringLiteral("SSH transport is not active; stopping local forward"));
            closeSocket(clientSock);
            m_stopRequested = true;
            break;
        }

        // Source address string for the direct-tcpip request.
        char hostBuf[NI_MAXHOST] = {0};
        char servBuf[NI_MAXSERV] = {0};
        ::getnameinfo(reinterpret_cast<struct sockaddr *>(&addr), addrLen,
                      hostBuf, sizeof(hostBuf), servBuf, sizeof(servBuf),
                      NI_NUMERICHOST | NI_NUMERICSERV);

        SshError chErr;
        _LIBSSH2_CHANNEL *channel = m_client->openDirectTcpip(
            m_remoteHost, m_remotePort,
            QString::fromLatin1(hostBuf), quint16(QString::fromLatin1(servBuf).toUShort()),
            chErr);
        if (!channel) {
            emit logMessage(QStringLiteral("Failed to open SSH channel: %1").arg(chErr.message));
            closeSocket(clientSock);
            continue;
        }

        // Python spawns a thread per connection; here we handle each connection
        // on its own short-lived worker so one slow client can't block accept.
        // The pump loop checks m_stopRequested each iteration, so stop() still
        // tears down in-flight connections.
        QThread *conn = QThread::create([this, clientSock, channel]() {
            pumpBidirectional(clientSock, channel);
            closeFd(clientSock);
            if (m_client)
                m_client->freeChannel(channel);
        });
        // Free the channel/socket if the forwarder is stopped mid-connection:
        // the pump loop checks m_stopRequested each iteration.
        QObject::connect(conn, &QThread::finished, conn, &QObject::deleteLater);
        conn->start();
    }

    closeSocket(m_listenSock);
    emit stopped();
}

// ===========================================================================
// RemotePortForwarder
// ===========================================================================

RemotePortForwarder::RemotePortForwarder(const QString &tunnelId, SshClient *client,
                                         const QString &targetHost, quint16 targetPort,
                                         quint16 serverListenPort,
                                         QObject *parent)
    : PortForwarder(tunnelId, client, parent)
    , m_targetHost(targetHost)
    , m_targetPort(targetPort)
    , m_serverListenPort(serverListenPort)
{
}

void RemotePortForwarder::stop()
{
    m_stopRequested = true;
    // Cancel the server-side forward. Python swallows errors here.
    if (m_listener && m_client) {
        m_client->forwardCancel(m_listener);
        m_listener = nullptr;
    }
    PortForwarder::stop();
}

void RemotePortForwarder::run()
{
    SshError err;
    int boundPort = 0;
    // Python passes ('', port) — empty host == all interfaces server-side.
    m_listener = m_client->forwardListen(QString(), m_serverListenPort, &boundPort, err);
    if (!m_listener) {
        emit errorOccurred(err.message.isEmpty()
                               ? QStringLiteral("request remote forward failed")
                               : err.message);
        emit stopped();
        return;
    }

    emit logMessage(QStringLiteral("Remote forward started on server port %1")
                        .arg(m_serverListenPort));
    emit started();

    while (!m_stopRequested.load()) {
        if (!m_client->isTransportAlive()) {
            emit errorOccurred(QStringLiteral("SSH transport is not active"));
            break;
        }

        // forwardAccept is non-blocking; poll with a small sleep so we notice stop.
        _LIBSSH2_CHANNEL *channel = m_client->forwardAccept(m_listener);
        if (!channel) {
            m_client->waitReadable(200); // block briefly on the shared socket
            continue;
        }

        // Relay this accepted channel to the local target on a worker thread.
        QThread *conn = QThread::create([this, channel]() {
            handleConnection(channel);
        });
        QObject::connect(conn, &QThread::finished, conn, &QObject::deleteLater);
        conn->start();
    }

    if (m_listener && m_client) {
        m_client->forwardCancel(m_listener);
        m_listener = nullptr;
    }
    emit stopped();
}

void RemotePortForwarder::handleConnection(_LIBSSH2_CHANNEL *channel)
{
    // Connect to the local target.
    // 目标一般在本机或同一局域网，默认 15 秒预算足够；关键是别再无限等——
    // 之前这里是裸 ::connect，目标半死就会永久占住一条转发线程。
    qintptr sock = connectTcp(m_targetHost, m_targetPort, kDefaultConnectTimeoutMs);
    if (sock < 0) {
        emit logMessage(QStringLiteral("Remote forward: cannot reach %1:%2")
                            .arg(m_targetHost).arg(m_targetPort));
        if (m_client)
            m_client->freeChannel(channel);
        return;
    }

    pumpBidirectional(sock, channel);

    closeSocket(sock);
    if (m_client)
        m_client->freeChannel(channel);
}

// ===========================================================================
// DynamicPortForwarder (SOCKS5)
// ===========================================================================

// SOCKS5 protocol constants (mirror forwarder.py).
namespace {
constexpr quint8 SOCKS_VERSION     = 0x05;
constexpr quint8 SOCKS_AUTH_NONE   = 0x00;
constexpr quint8 SOCKS_CMD_CONNECT = 0x01;
constexpr quint8 SOCKS_ATYP_IPV4   = 0x01;
constexpr quint8 SOCKS_ATYP_DOMAIN = 0x03;
constexpr quint8 SOCKS_ATYP_IPV6   = 0x04;
} // namespace

DynamicPortForwarder::DynamicPortForwarder(const QString &tunnelId, SshClient *client,
                                           const QString &localHost, quint16 localPort,
                                           QObject *parent)
    : PortForwarder(tunnelId, client, parent)
    , m_localHost(localHost)
    , m_localPort(localPort)
{
}

void DynamicPortForwarder::stop()
{
    m_stopRequested = true;
    closeSocket(m_serverSock);
    PortForwarder::stop();
}

void DynamicPortForwarder::run()
{
    QString err;
    m_serverSock = makeListenSocket(m_localHost, m_localPort, err);
    if (m_serverSock < 0) {
        emit errorOccurred(err);
        emit stopped();
        return;
    }

    emit logMessage(QStringLiteral("SOCKS5 proxy listening on %1:%2")
                        .arg(m_localHost).arg(m_localPort));
    emit started();

    while (!m_stopRequested.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_serverSock, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        int rc = ::select(int(m_serverSock) + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        struct sockaddr_storage addr{};
        socklen_t addrLen = sizeof(addr);
        qintptr clientSock = ::accept(m_serverSock, reinterpret_cast<struct sockaddr *>(&addr), &addrLen);
        if (clientSock < 0) {
            if (m_stopRequested.load())
                break;
            continue;
        }

        if (!m_client || !m_client->isTransportAlive()) {
            emit logMessage(QStringLiteral("SSH transport not active; rejecting SOCKS5 client"));
            closeSocket(clientSock);
            continue;
        }

        QThread *conn = QThread::create([this, clientSock]() {
            handleClient(clientSock);
        });
        QObject::connect(conn, &QThread::finished, conn, &QObject::deleteLater);
        conn->start();
    }

    closeSocket(m_serverSock);
    emit stopped();
}

void DynamicPortForwarder::handleClient(qintptr clientSock)
{
    _LIBSSH2_CHANNEL *channel = nullptr;

    // SOCKS5 greeting.
    if (!socks5Handshake(clientSock)) {
        closeSocket(clientSock);
        return;
    }

    // CONNECT request.
    QString dstHost;
    quint16 dstPort = 0;
    if (!parseSocks5Request(clientSock, dstHost, dstPort)) {
        closeSocket(clientSock);
        return;
    }

    emit logMessage(QStringLiteral("SOCKS5 connect to %1:%2").arg(dstHost).arg(dstPort));

    SshError chErr;
    channel = m_client->openDirectTcpip(dstHost, dstPort, m_localHost, 0, chErr);
    if (!channel) {
        // 0x05 connection refused vs 0x01 general failure: Python distinguishes
        // ChannelException from generic errors; libssh2 collapses these, so use
        // general failure when no message, else host-unreachable.
        sendSocks5Reply(clientSock, 0x05, dstPort);
        closeSocket(clientSock);
        return;
    }

    // Success reply: VER REP=0 RSV ATYP=IPv4 BND.ADDR=0.0.0.0 BND.PORT.
    sendSocks5Reply(clientSock, 0x00, dstPort);

    pumpBidirectional(clientSock, channel);

    closeSocket(clientSock);
    if (m_client)
        m_client->freeChannel(channel);
}

bool DynamicPortForwarder::socks5Handshake(qintptr clientSock)
{
    char buf[256];
    qint64 n = recvSome(clientSock, buf, sizeof(buf), 5000);
    if (n < 2)
        return false;
    if (quint8(buf[0]) != SOCKS_VERSION) {
        emit logMessage(QStringLiteral("Unsupported SOCKS version: %1").arg(int(quint8(buf[0]))));
        return false;
    }
    // No authentication.
    const char reply[2] = {char(SOCKS_VERSION), char(SOCKS_AUTH_NONE)};
    return ::send(clientSock, reply, 2, 0) == 2;
}

bool DynamicPortForwarder::parseSocks5Request(qintptr clientSock, QString &host, quint16 &port)
{
    char buf[300];
    qint64 n = recvSome(clientSock, buf, sizeof(buf), 5000);
    if (n < 4) {
        sendSocks5Reply(clientSock, 0x01, 0);
        return false;
    }

    const quint8 version = quint8(buf[0]);
    const quint8 cmd = quint8(buf[1]);
    const quint8 atyp = quint8(buf[3]);

    if (version != SOCKS_VERSION) {
        sendSocks5Reply(clientSock, 0x01, 0);
        return false;
    }
    if (cmd != SOCKS_CMD_CONNECT) {
        sendSocks5Reply(clientSock, 0x07, 0); // command not supported
        return false;
    }

    if (atyp == SOCKS_ATYP_IPV4) {
        if (n < 10) {
            sendSocks5Reply(clientSock, 0x01, 0);
            return false;
        }
        char addr[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, buf + 4, addr, sizeof(addr));
        host = QString::fromLatin1(addr);
        port = (quint16(quint8(buf[8])) << 8) | quint8(buf[9]);
    } else if (atyp == SOCKS_ATYP_DOMAIN) {
        if (n < 5) {
            sendSocks5Reply(clientSock, 0x01, 0);
            return false;
        }
        const quint8 dlen = quint8(buf[4]);
        if (n < 5 + dlen + 2) {
            sendSocks5Reply(clientSock, 0x01, 0);
            return false;
        }
        host = QString::fromUtf8(buf + 5, dlen);
        port = (quint16(quint8(buf[5 + dlen])) << 8) | quint8(buf[6 + dlen]);
    } else if (atyp == SOCKS_ATYP_IPV6) {
        if (n < 22) {
            sendSocks5Reply(clientSock, 0x01, 0);
            return false;
        }
        char addr[INET6_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET6, buf + 4, addr, sizeof(addr));
        host = QString::fromLatin1(addr);
        port = (quint16(quint8(buf[20])) << 8) | quint8(buf[21]);
    } else {
        sendSocks5Reply(clientSock, 0x08, 0); // address type not supported
        return false;
    }
    return true;
}

void DynamicPortForwarder::sendSocks5Reply(qintptr clientSock, quint8 replyCode, quint16 port)
{
    const char reply[10] = {
        char(SOCKS_VERSION), char(replyCode), 0x00, char(SOCKS_ATYP_IPV4),
        0, 0, 0, 0, // BND.ADDR = 0.0.0.0
        char((port >> 8) & 0xff), char(port & 0xff)
    };
    ::send(clientSock, reply, sizeof(reply), 0);
}

} // namespace cubeshell
