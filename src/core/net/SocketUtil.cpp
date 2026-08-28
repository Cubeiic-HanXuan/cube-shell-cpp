// SocketUtil.cpp — 见 SocketUtil.h 的设计说明与提取来源。

#include "SocketUtil.h"

#include <QDeadlineTimer>

#include <cstdint>
#include <cstring>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socklen_t = int;
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace cubeshell {
namespace socket_util {

namespace {

// 本地 socket 对的建立预算。全程走回环，正常是微秒级；给 5 秒纯粹是防止
// 极端负载下 accept 排不上队时无限等。
constexpr int kPairTimeoutMs = 5000;

// connectOne 的错误出参哨兵（真实 errno / WSA 错误码一律为正）。
constexpr int kErrTimeout   = -1;
constexpr int kErrCancelled = -2;
constexpr int kErrNoAddress = -3;

// qintptr → 平台原生 socket 类型。Win32 的 SOCKET 是 UINT_PTR（无符号），
// 与 qintptr 等宽但符号性不同，必须经 uintptr_t 过渡做等宽数值转换
// （直接 reinterpret_cast 会触发 MSVC C2440，见 SshClient::shutdownSocket 的注释）。
#ifdef Q_OS_WIN
inline SOCKET toNative(qintptr s) { return static_cast<SOCKET>(static_cast<uintptr_t>(s)); }
#else
inline int toNative(qintptr s) { return static_cast<int>(s); }
#endif

// errMsg 是可选出参，统一走这个小助手，省掉满篇的 if (errMsg)。
void setErr(QString *errMsg, const QString &text)
{
    if (errMsg)
        *errMsg = text;
}

int lastSocketError()
{
#ifdef Q_OS_WIN
    return WSAGetLastError();
#else
    return errno;
#endif
}

// 非阻塞 connect 的「正在进行中」：POSIX 报 EINPROGRESS，Winsock 报 WSAEWOULDBLOCK。
bool isInProgress(int err)
{
#ifdef Q_OS_WIN
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS || err == EWOULDBLOCK || err == EAGAIN;
#endif
}

// select 说可读/可写之后 recv/send 仍可能返回 EAGAIN（伪唤醒），要当作"再等等"
// 而不是错误。
bool isWouldBlock(int err)
{
#ifdef Q_OS_WIN
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool isInterrupted(int err)
{
#ifdef Q_OS_WIN
    Q_UNUSED(err);
    return false;   // Winsock 没有 EINTR
#else
    return err == EINTR;
#endif
}

QString describeSystemError(int err)
{
#ifdef Q_OS_WIN
    return QStringLiteral("错误码 %1").arg(err);
#else
    return QStringLiteral("%1 (errno %2)")
        .arg(QString::fromLocal8Bit(::strerror(err))).arg(err);
#endif
}

// 建连失败的措辞。ECONNREFUSED / ETIMEDOUT / EHOSTUNREACH 对用户是三种完全
// 不同的处置（改端口 / 查防火墙 / 查路由），不能一律糊成"连接失败"。
QString describeConnectError(int err, const QString &host, quint16 port)
{
    const QString target = QStringLiteral("%1:%2").arg(host).arg(port);
    switch (err) {
    case kErrCancelled: return QStringLiteral("连接 %1 已取消").arg(target);
    case kErrTimeout:   return QStringLiteral("连接 %1 超时").arg(target);
    case kErrNoAddress: return QStringLiteral("主机 %1 没有可用地址").arg(target);
    default: break;
    }
#ifdef Q_OS_WIN
    switch (err) {
    case WSAECONNREFUSED: return QStringLiteral("连接 %1 被拒绝（该端口没有服务在监听）").arg(target);
    case WSAETIMEDOUT:    return QStringLiteral("连接 %1 超时").arg(target);
    case WSAEHOSTUNREACH: return QStringLiteral("主机 %1 不可达").arg(target);
    case WSAENETUNREACH:  return QStringLiteral("网络不可达：%1").arg(target);
    default: break;
    }
#else
    switch (err) {
    case ECONNREFUSED: return QStringLiteral("连接 %1 被拒绝（该端口没有服务在监听）").arg(target);
    case ETIMEDOUT:    return QStringLiteral("连接 %1 超时").arg(target);
    case EHOSTUNREACH: return QStringLiteral("主机 %1 不可达").arg(target);
    case ENETUNREACH:  return QStringLiteral("网络不可达：%1").arg(target);
    default: break;
    }
#endif
    return QStringLiteral("连接 %1 失败：%2").arg(target, describeSystemError(err));
}

quint16 portOfSockaddr(const struct sockaddr_storage &ss)
{
    if (ss.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const struct sockaddr_in *>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const struct sockaddr_in6 *>(&ss)->sin6_port);
    return 0;
}

bool isLoopbackSockaddr(const struct sockaddr_storage &ss)
{
    if (ss.ss_family == AF_INET) {
        const auto *v4 = reinterpret_cast<const struct sockaddr_in *>(&ss);
        return (ntohl(v4->sin_addr.s_addr) >> 24) == 127;   // 127.0.0.0/8
    }
    if (ss.ss_family == AF_INET6) {
        const auto *v6 = reinterpret_cast<const struct sockaddr_in6 *>(&ss);
        return IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) != 0;
    }
    return false;
}

// 把 select 的等待时长切成不超过 kSelectSliceMs 的一片，填进 timeval。
void fillSlice(struct timeval &tv, qint64 remainingMs)
{
    const qint64 slice = qMin<qint64>(qMax<qint64>(remainingMs, 0), kSelectSliceMs);
    tv.tv_sec  = static_cast<decltype(tv.tv_sec)>(slice / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((slice % 1000) * 1000);
}

// 对单个候选地址做一次带超时、可取消的非阻塞 connect。
// 成功返回已恢复成阻塞模式的 fd；失败返回 kInvalidSocket 并把原因写进 errOut。
qintptr connectOne(const struct addrinfo *ai, QDeadlineTimer deadline,
                   const std::atomic<bool> *cancelled, int *errOut)
{
    qintptr sock = static_cast<qintptr>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (sock < 0) {
        *errOut = lastSocketError();
        return kInvalidSocket;
    }
    // 先设选项再 connect：SIGPIPE 是进程级的，等到第一次 send 才设就已经晚了。
    setNoSigPipe(sock);
    setNoDelay(sock);

    if (!setNonBlocking(sock, true)) {
        *errOut = lastSocketError();
        closeFd(sock);
        return kInvalidSocket;
    }

    if (::connect(toNative(sock), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
        // 回环/同机常见：一次就连上了。
        if (setNonBlocking(sock, false))
            return sock;
        *errOut = lastSocketError();
        closeFd(sock);
        return kInvalidSocket;
    }
    const int connErr = lastSocketError();
    if (!isInProgress(connErr)) {
        *errOut = connErr;
        closeFd(sock);
        return kInvalidSocket;
    }

    for (;;) {
        if (cancelled && cancelled->load()) {
            *errOut = kErrCancelled;
            closeFd(sock);
            return kInvalidSocket;
        }
        const qint64 left = deadline.remainingTime();
        if (left <= 0) {
            *errOut = kErrTimeout;
            closeFd(sock);
            return kInvalidSocket;
        }

        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_SET(toNative(sock), &wfds);
        FD_ZERO(&efds);
        FD_SET(toNative(sock), &efds);
        struct timeval tv;
        fillSlice(tv, left);
        // exceptfds 不是可选的：Windows 上失败的 connect 只在异常集里报，
        // 不进可写集。只 select 可写会一直等到超时才发现连不上。
        const int rc = ::select(int(sock) + 1, nullptr, &wfds, &efds, &tv);
        if (rc < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e))
                continue;
            *errOut = e;
            closeFd(sock);
            return kInvalidSocket;
        }
        if (rc == 0)
            continue;   // 本切片没动静：回头看一眼取消标志和总预算

        // 可写不等于连上了（失败的 connect 在 POSIX 上同样报可写），
        // 必须查 SO_ERROR 才知道结果。
        int soErr = 0;
        socklen_t soLen = sizeof(soErr);
        if (::getsockopt(toNative(sock), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char *>(&soErr), &soLen) != 0) {
            *errOut = lastSocketError();
            closeFd(sock);
            return kInvalidSocket;
        }
        if (soErr != 0) {
            *errOut = soErr;
            closeFd(sock);
            return kInvalidSocket;
        }
        // 恢复阻塞模式：libssh2_session_handshake 之前的既有行为就是阻塞 fd，
        // 这里不改变它（真正翻成非阻塞是 openShell 的事）。
        if (!setNonBlocking(sock, false)) {
            *errOut = lastSocketError();
            closeFd(sock);
            return kInvalidSocket;
        }
        return sock;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------

void closeSocket(qintptr &sock)
{
    if (sock < 0)
        return;
#ifdef Q_OS_WIN
    ::closesocket(toNative(sock));
#else
    ::close(toNative(sock));
#endif
    sock = kInvalidSocket;
}

void closeFd(qintptr sock)
{
    if (sock < 0)
        return;
#ifdef Q_OS_WIN
    ::closesocket(toNative(sock));
#else
    ::close(toNative(sock));
#endif
}

void shutdownFd(qintptr sock)
{
    if (sock < 0)
        return;
#ifdef Q_OS_WIN
    ::shutdown(toNative(sock), SD_BOTH);
#else
    ::shutdown(toNative(sock), SHUT_RDWR);
#endif
}

// ---------------------------------------------------------------------------
// socket 选项
// ---------------------------------------------------------------------------

bool setNonBlocking(qintptr sock, bool on)
{
    if (sock < 0)
        return false;
#ifdef Q_OS_WIN
    u_long mode = on ? 1 : 0;
    return ::ioctlsocket(toNative(sock), FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(toNative(sock), F_GETFL, 0);
    if (flags < 0)
        return false;
    const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(toNative(sock), F_SETFL, want) == 0;
#endif
}

void setNoSigPipe(qintptr sock)
{
#ifdef SO_NOSIGPIPE
    // macOS/BSD 独有。Linux 走 send 的 MSG_NOSIGNAL 或进程级 SIG_IGN，
    // Windows 没有这个信号。
    if (sock < 0)
        return;
    int on = 1;
    ::setsockopt(toNative(sock), SOL_SOCKET, SO_NOSIGPIPE,
                 reinterpret_cast<const char *>(&on), sizeof(on));
#else
    Q_UNUSED(sock);
#endif
}

void setNoDelay(qintptr sock)
{
    if (sock < 0)
        return;
    int on = 1;
    ::setsockopt(toNative(sock), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char *>(&on), sizeof(on));
}

void setSocketBuffers(qintptr sock, int bytes)
{
    if (sock < 0 || bytes <= 0)
        return;
    // 内核通常只给一部分（Linux 还会翻倍记账），拿不到全额不算失败。
    ::setsockopt(toNative(sock), SOL_SOCKET, SO_SNDBUF,
                 reinterpret_cast<const char *>(&bytes), sizeof(bytes));
    ::setsockopt(toNative(sock), SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char *>(&bytes), sizeof(bytes));
}

// ---------------------------------------------------------------------------
// 建连
// ---------------------------------------------------------------------------

qintptr connectTcp(const QString &host, quint16 port, int timeoutMs,
                   QString *errMsg, const std::atomic<bool> *cancelled)
{
    if (host.isEmpty()) {
        setErr(errMsg, QStringLiteral("目标主机为空"));
        return kInvalidSocket;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const QByteArray h = host.toUtf8();
    const QByteArray p = QByteArray::number(port);
    struct addrinfo *res = nullptr;
    const int gaiRc = ::getaddrinfo(h.constData(), p.constData(), &hints, &res);
    if (gaiRc != 0) {
#ifdef Q_OS_WIN
        setErr(errMsg, QStringLiteral("无法解析主机 %1（错误码 %2）").arg(host).arg(gaiRc));
#else
        setErr(errMsg, QStringLiteral("无法解析主机 %1（%2）")
                           .arg(host, QString::fromLocal8Bit(::gai_strerror(gaiRc))));
#endif
        return kInvalidSocket;
    }
    if (!res) {
        setErr(errMsg, describeConnectError(kErrNoAddress, host, port));
        return kInvalidSocket;
    }

    // 整个调用共享一份预算（见头文件：逐地址各给一份会让双栈黑洞把总耗时翻倍）。
    // 0 表示预算已耗尽，QDeadlineTimer(0) 会立刻 hasExpired()，正是想要的。
    const QDeadlineTimer deadline(timeoutMs >= 0 ? timeoutMs : kDefaultConnectTimeoutMs);

    qintptr sock = kInvalidSocket;
    int lastErr = kErrNoAddress;      // 一个候选都没轮到时的兜底原因
    for (auto *ai = res; ai; ai = ai->ai_next) {
        if (cancelled && cancelled->load()) {
            lastErr = kErrCancelled;
            break;
        }
        if (deadline.hasExpired()) {
            lastErr = kErrTimeout;
            break;
        }
        sock = connectOne(ai, deadline, cancelled, &lastErr);
        if (sock >= 0)
            break;
        if (lastErr == kErrCancelled)
            break;      // 取消是终局，不再试下一个地址
    }
    ::freeaddrinfo(res);

    if (sock < 0)
        setErr(errMsg, describeConnectError(lastErr, host, port));
    return sock;
}

qintptr makeListenSocket(const QString &host, quint16 port, QString &errMsg)
{
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const QByteArray h = host.toUtf8();
    const QByteArray p = QByteArray::number(port);
    struct addrinfo *res = nullptr;
    if (::getaddrinfo(h.isEmpty() ? nullptr : h.constData(), p.constData(), &hints, &res) != 0) {
        errMsg = QStringLiteral("Cannot resolve bind address %1").arg(host);
        return kInvalidSocket;
    }

    qintptr sock = kInvalidSocket;
    for (auto *ai = res; ai; ai = ai->ai_next) {
        sock = static_cast<qintptr>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (sock < 0)
            continue;
        int one = 1;
        ::setsockopt(toNative(sock), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&one), sizeof(one));
        if (::bind(toNative(sock), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0
            && ::listen(toNative(sock), 100) == 0)
            break;
        closeSocket(sock);
    }
    ::freeaddrinfo(res);

    if (sock < 0) {
        // Distinguish "address in use" / "permission denied" like forwarder.py.
        const int e = lastSocketError();
#ifdef Q_OS_WIN
        if (e == WSAEADDRINUSE)
#else
        if (e == EADDRINUSE)
#endif
            errMsg = QStringLiteral("Port %1 is already in use").arg(port);
#ifndef Q_OS_WIN
        else if (e == EACCES)
            errMsg = QStringLiteral("No permission to bind port %1 (use a port > 1024)").arg(port);
#endif
        else
            errMsg = QStringLiteral("Failed to bind %1:%2 (errno %3)").arg(host).arg(port).arg(e);
    }
    return sock;
}

quint16 localPort(qintptr sock)
{
    if (sock < 0)
        return 0;
    struct sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(toNative(sock), reinterpret_cast<struct sockaddr *>(&ss), &len) != 0)
        return 0;
    return portOfSockaddr(ss);
}

bool makeSocketPair(qintptr &clientEnd, qintptr &serverEnd, QString *errMsg)
{
    clientEnd = kInvalidSocket;
    serverEnd = kInvalidSocket;

    const auto fail = [&](const QString &why) {
        setErr(errMsg, QStringLiteral("建立本地 socket 对失败：%1").arg(why));
        return false;
    };

    QString err;
    qintptr listenSock = makeListenSocket(QStringLiteral("127.0.0.1"), 0, err);
    if (listenSock < 0)
        return fail(err);

    const quint16 boundPort = localPort(listenSock);
    if (boundPort == 0) {
        closeSocket(listenSock);
        return fail(QStringLiteral("取不到内核分配的监听端口"));
    }

    const QDeadlineTimer deadline(kPairTimeoutMs);
    clientEnd = connectTcp(QStringLiteral("127.0.0.1"), boundPort, kPairTimeoutMs, &err);
    if (clientEnd < 0) {
        closeSocket(listenSock);
        return fail(err);
    }
    const quint16 myPort = localPort(clientEnd);
    if (myPort == 0) {
        closeSocket(listenSock);
        closeSocket(clientEnd);
        return fail(QStringLiteral("取不到本端端口，无法认领 accept 出来的连接"));
    }

    // accept 到的不一定是我们自己那一条：listen 的那一瞬间本机任何进程都能来连。
    // 按「对端是回环地址 + 对端端口 == clientEnd 的本端端口」认领；不匹配就关掉
    // 继续等。这把劫持窗口关死（见头文件说明）。
    for (;;) {
        if (deadline.hasExpired()) {
            closeSocket(listenSock);
            closeSocket(clientEnd);
            return fail(QStringLiteral("等待 accept 超时"));
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(toNative(listenSock), &rfds);
        struct timeval tv;
        fillSlice(tv, deadline.remainingTime());
        const int rc = ::select(int(listenSock) + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e))
                continue;
            closeSocket(listenSock);
            closeSocket(clientEnd);
            return fail(QStringLiteral("等待 accept 出错：%1").arg(describeSystemError(e)));
        }
        if (rc == 0)
            continue;

        struct sockaddr_storage peer{};
        socklen_t peerLen = sizeof(peer);
        const qintptr accepted = static_cast<qintptr>(
            ::accept(toNative(listenSock), reinterpret_cast<struct sockaddr *>(&peer), &peerLen));
        if (accepted < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e) || isWouldBlock(e))
                continue;
            closeSocket(listenSock);
            closeSocket(clientEnd);
            return fail(QStringLiteral("accept 失败：%1").arg(describeSystemError(e)));
        }
        if (isLoopbackSockaddr(peer) && portOfSockaddr(peer) == myPort) {
            serverEnd = accepted;
            break;
        }
        // 本机别的进程抢先连上来了，不是我们要的那一条。
        closeFd(accepted);
    }

    closeSocket(listenSock);

    // 两端都按「一端给 libssh2、一端给泵线程」的用法配好。
    for (const qintptr fd : {clientEnd, serverEnd}) {
        setNoSigPipe(fd);
        setNoDelay(fd);
        setSocketBuffers(fd, kPairSocketBufferBytes);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 收发
// ---------------------------------------------------------------------------

qint64 recvSome(qintptr sock, char *buf, int maxBytes, int timeoutMs)
{
    if (sock < 0 || !buf || maxBytes <= 0)
        return -1;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(toNative(sock), &rfds);
    struct timeval tv;
    tv.tv_sec  = static_cast<decltype(tv.tv_sec)>(timeoutMs / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeoutMs % 1000) * 1000);
    const int rc = ::select(int(sock) + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0)
        return -1;      // 超时(0) 与出错(<0) 对调用方是同一种处置：放弃本次握手
    const auto n = ::recv(toNative(sock), buf, maxBytes, 0);
    if (n < 0)
        return -1;
    return qint64(n);   // 0 == 对端正常关闭
}

qint64 recvNonBlocking(qintptr sock, char *buf, int maxBytes, QString *errMsg)
{
    if (sock < 0 || !buf || maxBytes <= 0) {
        setErr(errMsg, QStringLiteral("recvNonBlocking: 参数无效"));
        return -2;
    }
    const auto n = ::recv(toNative(sock), buf, maxBytes, 0);
    if (n >= 0)
        return qint64(n);   // 0 == 对端正常关闭
    const int e = lastSocketError();
    if (isWouldBlock(e) || isInterrupted(e))
        return -1;          // 没数据，不是错误
    setErr(errMsg, describeSystemError(e));
    return -2;
}

bool recvExact(qintptr sock, char *buf, qint64 size, int timeoutMs,
               QString *errMsg, const std::atomic<bool> *cancelled)
{
    if (sock < 0 || !buf || size < 0) {
        setErr(errMsg, QStringLiteral("recvExact: 参数无效"));
        return false;
    }
    // 0 = 预算已耗尽（约定见头文件 connectTcp 之前那段），不是"用默认值"。
    const QDeadlineTimer deadline(timeoutMs >= 0 ? timeoutMs : kDefaultConnectTimeoutMs);
    qint64 got = 0;
    while (got < size) {
        if (cancelled && cancelled->load()) {
            setErr(errMsg, QStringLiteral("读取已取消"));
            return false;
        }
        const qint64 left = deadline.remainingTime();
        if (left <= 0) {
            setErr(errMsg, QStringLiteral("读取超时（已收到 %1/%2 字节）").arg(got).arg(size));
            return false;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(toNative(sock), &rfds);
        struct timeval tv;
        fillSlice(tv, left);
        const int rc = ::select(int(sock) + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e))
                continue;
            setErr(errMsg, QStringLiteral("读取失败：%1").arg(describeSystemError(e)));
            return false;
        }
        if (rc == 0)
            continue;

        const int want = static_cast<int>(qMin<qint64>(size - got, 64 * 1024));
        const auto n = ::recv(toNative(sock), buf + got, want, 0);
        if (n == 0) {
            setErr(errMsg, QStringLiteral("对端提前关闭了连接（已收到 %1/%2 字节）")
                               .arg(got).arg(size));
            return false;
        }
        if (n < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e) || isWouldBlock(e))
                continue;   // 伪唤醒：select 说可读但没数据
            setErr(errMsg, QStringLiteral("读取失败：%1").arg(describeSystemError(e)));
            return false;
        }
        got += n;
    }
    return true;
}

bool sendAll(qintptr sock, const char *data, qint64 size, int timeoutMs,
             QString *errMsg, const std::atomic<bool> *cancelled)
{
    if (sock < 0 || (!data && size > 0) || size < 0) {
        setErr(errMsg, QStringLiteral("sendAll: 参数无效"));
        return false;
    }
    // 0 = 预算已耗尽（约定见头文件 connectTcp 之前那段），不是"用默认值"。
    const QDeadlineTimer deadline(timeoutMs >= 0 ? timeoutMs : kDefaultConnectTimeoutMs);
    qint64 sent = 0;
    while (sent < size) {
        if (cancelled && cancelled->load()) {
            setErr(errMsg, QStringLiteral("发送已取消"));
            return false;
        }
        const qint64 left = deadline.remainingTime();
        if (left <= 0) {
            setErr(errMsg, QStringLiteral("发送超时（已发出 %1/%2 字节）").arg(sent).arg(size));
            return false;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(toNative(sock), &wfds);
        struct timeval tv;
        fillSlice(tv, left);
        const int rc = ::select(int(sock) + 1, nullptr, &wfds, nullptr, &tv);
        if (rc < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e))
                continue;
            setErr(errMsg, QStringLiteral("发送失败：%1").arg(describeSystemError(e)));
            return false;
        }
        if (rc == 0)
            continue;

        const int want = static_cast<int>(qMin<qint64>(size - sent, 64 * 1024));
#ifdef MSG_NOSIGNAL
        // Linux：SO_NOSIGPIPE 不存在，靠这个标志避免对端已关时被 SIGPIPE 杀掉。
        const auto n = ::send(toNative(sock), data + sent, want, MSG_NOSIGNAL);
#else
        const auto n = ::send(toNative(sock), data + sent, want, 0);
#endif
        if (n < 0) {
            const int e = lastSocketError();
            if (isInterrupted(e) || isWouldBlock(e))
                continue;
            setErr(errMsg, QStringLiteral("发送失败：%1").arg(describeSystemError(e)));
            return false;
        }
        sent += n;
    }
    return true;
}

qint64 sendNonBlocking(qintptr sock, const char *data, qint64 size, QString *errMsg)
{
    if (sock < 0 || (!data && size > 0) || size < 0) {
        setErr(errMsg, QStringLiteral("sendNonBlocking: 参数无效"));
        return -2;
    }
    if (size == 0)
        return 0;
    const int want = static_cast<int>(qMin<qint64>(size, 64 * 1024));
#ifdef MSG_NOSIGNAL
    // Linux：SO_NOSIGPIPE 不存在，靠这个标志避免对端已关时被 SIGPIPE 杀掉。
    const auto n = ::send(toNative(sock), data, want, MSG_NOSIGNAL);
#else
    const auto n = ::send(toNative(sock), data, want, 0);
#endif
    if (n >= 0)
        return qint64(n);
    const int e = lastSocketError();
    if (isWouldBlock(e) || isInterrupted(e))
        return -1;          // 写不进去，不是错误
    setErr(errMsg, describeSystemError(e));
    return -2;
}

} // namespace socket_util
} // namespace cubeshell
