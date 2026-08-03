// SshClient.cpp — libssh2-backed SSH client. See SshClient.h.

#include "SshClient.h"

#include <QLoggingCategory>
#include <QMutexLocker>

#include <libssh2.h>

#include <cstring>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef Q_OS_MACOS
#  include <sys/types.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(sshLog)
Q_LOGGING_CATEGORY(sshLog, "cubeshell.ssh")

namespace cubeshell {

// One-time libssh2 global init.
static void ensureLibssh2Init()
{
    static bool inited = false;
    if (!inited) {
#ifdef Q_OS_WIN
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 0), &wsa);
#endif
        libssh2_init(0);
        inited = true;
    }
}

SshClient::SshClient(QObject *parent)
    : QObject(parent)
{
    ensureLibssh2Init();
}

SshClient::~SshClient()
{
    disconnectFromHost();
}

void SshClient::setHost(const QString &host, quint16 port) { m_host = host; m_port = port; }
void SshClient::setUsername(const QString &username) { m_username = username; }
void SshClient::setPassword(const QString &password) { m_password = password; }

void SshClient::setPrivateKey(const QString &keyType, const QString &keyFile, const QString &passphrase)
{
    m_keyType = keyType;
    m_keyFile = keyFile;
    m_passphrase = passphrase;
}

bool SshClient::isConnected() const { return m_session != nullptr; }
bool SshClient::isChannelOpen() const { return m_channel != nullptr; }

// Fill an SshError from the current libssh2 session error.
static void fillSessionError(_LIBSSH2_SESSION *session, SshError &error, const QString &context)
{
    char *msg = nullptr;
    int len = 0;
    int code = session ? libssh2_session_last_error(session, &msg, &len, 0) : 0;
    error.code = code;
    error.message = context;
    if (msg && len > 0)
        error.message += QStringLiteral(": ") + QString::fromLatin1(msg, len);
}

bool SshClient::connectToHost(SshPromptCallback promptCallback, SshError &error)
{
    m_promptCallback = std::move(promptCallback);

    // --- TCP connect ---
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const QByteArray host = m_host.toUtf8();
    const QByteArray port = QByteArray::number(m_port);
    if (::getaddrinfo(host.constData(), port.constData(), &hints, &res) != 0) {
        error.message = QStringLiteral("Cannot resolve host %1").arg(m_host);
        return false;
    }

    qintptr sock = -1;
    for (auto *ai = res; ai; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0)
            continue;
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
#ifdef Q_OS_WIN
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        sock = -1;
    }
    ::freeaddrinfo(res);

    if (sock < 0) {
        error.message = QStringLiteral("Cannot connect to %1:%2").arg(m_host).arg(m_port);
        return false;
    }
    m_sock = sock;

#ifdef Q_OS_MACOS
    // Prevent SIGPIPE on this socket: macOS has no MSG_NOSIGNAL, so per-socket
    // SO_NOSIGPIPE is the only way to prevent send() from raising SIGPIPE when
    // the remote end has closed the connection (e.g. during SSH teardown).
    {
        int on = 1;
        ::setsockopt(m_sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    // --- SSH session + handshake ---
    m_session = libssh2_session_init();
    if (!m_session) {
        error.message = QStringLiteral("libssh2_session_init failed");
        return false;
    }
    libssh2_session_set_blocking(m_session, 1); // blocking for connect/auth simplicity

    if (libssh2_session_handshake(m_session, m_sock) != 0) {
        fillSessionError(m_session, error, QStringLiteral("SSH handshake failed"));
        disconnectFromHost();
        return false;
    }

    // --- authenticate ---
    if (!authenticate(error)) {
        disconnectFromHost();
        return false;
    }

    return true;
}

bool SshClient::authenticate(SshError &error)
{
    // Query supported auth methods.
    char *list = libssh2_userauth_list(m_session, m_username.toUtf8().constData(),
                                       m_username.toUtf8().size());
    const QString supported = list ? QString::fromUtf8(list) : QString();
    qCDebug(sshLog) << "server auth methods:" << supported;

    // Prefer public key if a key file was supplied.
    if (!m_keyFile.isEmpty() && supported.contains(QStringLiteral("publickey")))
        return authPublicKey(error);

    // Password first when one was configured: keyboard-interactive would
    // otherwise prompt the user for a value we already have. (ssh_func.py's
    // mfa_callback only kicks in when there is no plain password to try.)
    if (!m_password.isEmpty() && supported.contains(QStringLiteral("password"))) {
        if (authPassword(error))
            return true;
        // A wrong password is a hard failure; don't fall through to MFA prompts.
        if (error.authFailed)
            return false;
    }

    // keyboard-interactive (OTP / MFA) — ssh_func.py tries this when an MFA
    // callback is present, to avoid paramiko answering every prompt with the password.
    if (m_promptCallback && supported.contains(QStringLiteral("keyboard-interactive")))
        return authKeyboardInteractive(m_promptCallback, error);

    if (supported.contains(QStringLiteral("password")))
        return authPassword(error);

    // Fall back: try password then keyboard-interactive regardless of the list.
    if (authPassword(error))
        return true;
    if (m_promptCallback)
        return authKeyboardInteractive(m_promptCallback, error);

    error.authFailed = true;
    if (error.message.isEmpty())
        error.message = QStringLiteral("No supported authentication method");
    return false;
}

bool SshClient::authPassword(SshError &error)
{
    const QByteArray user = m_username.toUtf8();
    const QByteArray pass = m_password.toUtf8();
    if (libssh2_userauth_password(m_session, user.constData(), pass.constData()) == 0)
        return true;
    fillSessionError(m_session, error, QStringLiteral("Password authentication failed"));
    error.authFailed = true;
    return false;
}

bool SshClient::authPublicKey(SshError &error)
{
    const QByteArray user = m_username.toUtf8();
    const QByteArray keyFile = m_keyFile.toUtf8();
    const QByteArray passphrase = m_passphrase.toUtf8();

    // libssh2 derives the public key from the private one when no .pub is given.
    int rc = libssh2_userauth_publickey_fromfile(
        m_session, user.constData(),
        /*publickey*/ nullptr,
        keyFile.constData(),
        passphrase.isEmpty() ? nullptr : passphrase.constData());
    if (rc == 0)
        return true;
    fillSessionError(m_session, error, QStringLiteral("Public key authentication failed"));
    error.authFailed = true;
    return false;
}

// C trampoline for libssh2's keyboard-interactive callback.
// MFA auth is synchronous and single-shot, so a file-static pointer to the
// active client is safe here (no concurrent auth on the same client).
static SshClient *s_kbdintClient = nullptr;

static void kbdintCallback(const char *name, int name_len,
                           const char *instruction, int instruction_len,
                           int num_prompts,
                           const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                           LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                           void **abstract)
{
    Q_UNUSED(name); Q_UNUSED(name_len);
    Q_UNUSED(instruction); Q_UNUSED(instruction_len); Q_UNUSED(abstract);
    SshClient *self = s_kbdintClient;
    if (!self)
        return;
    for (int i = 0; i < num_prompts; ++i) {
        const QString promptText = QString::fromUtf8(reinterpret_cast<const char *>(prompts[i].text),
                                                     qsizetype(prompts[i].length));
        const bool echo = prompts[i].echo != 0;
        const QString answer = self->handleKbdIntPrompt(promptText, echo);
        const QByteArray a = answer.toUtf8();
        responses[i].text = static_cast<char *>(malloc(a.size()));
        std::memcpy(responses[i].text, a.constData(), a.size());
        responses[i].length = a.size();
    }
}

bool SshClient::authKeyboardInteractive(SshPromptCallback cb, SshError &error)
{
    m_promptCallback = std::move(cb);
    s_kbdintClient = this;
    const QByteArray user = m_username.toUtf8();
    int rc = libssh2_userauth_keyboard_interactive(m_session, user.constData(),
                                                   &kbdintCallback);
    s_kbdintClient = nullptr;
    if (rc == 0)
        return true;
    fillSessionError(m_session, error, QStringLiteral("Keyboard-interactive authentication failed"));
    error.authFailed = true;
    return false;
}

QString SshClient::handleKbdIntPrompt(const QString &prompt, bool echo)
{
    if (!m_promptCallback)
        return m_password; // default: answer with the password (paramiko-like)
    const QString answer = m_promptCallback(prompt, echo);
    return answer.isEmpty() ? m_password : answer;
}

bool SshClient::openShell(const QByteArray &term, int width, int height, SshError &error)
{
    if (!m_session) {
        error.message = QStringLiteral("Not connected");
        return false;
    }

    m_channel = libssh2_channel_open_session(m_session);
    if (!m_channel) {
        fillSessionError(m_session, error, QStringLiteral("Unable to open channel"));
        return false;
    }

    if (libssh2_channel_request_pty_ex(m_channel,
                                       term.constData(), term.size(),
                                       /*modes*/ nullptr, 0,
                                       width, height,
                                       LIBSSH2_TERM_WIDTH_PX, LIBSSH2_TERM_HEIGHT_PX) != 0) {
        fillSessionError(m_session, error, QStringLiteral("request_pty failed"));
        closeChannel();
        return false;
    }

    if (libssh2_channel_shell(m_channel) != 0) {
        fillSessionError(m_session, error, QStringLiteral("invoke_shell failed"));
        closeChannel();
        return false;
    }

    // Switch to non-blocking for the bridge's poll-driven read loop.
    libssh2_session_set_blocking(m_session, 0);

    // The socket must also be non-blocking: libssh2 restores the original
    // (blocking) socket state after session_handshake. Without this, recv()
    // blocks inside libssh2_channel_read even though the session API is
    // non-blocking, and the bridge read loop never returns EAGAIN.
#ifndef Q_OS_WIN
    {
        int flags = fcntl(m_sock, F_GETFL, 0);
        if (flags >= 0)
            fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);
    }
#else
    {
        u_long mode = 1;
        ioctlsocket(m_sock, FIONBIO, &mode);
    }
#endif

    return true;
}

QByteArray SshClient::readChannel(int maxBytes, bool *wouldBlock)
{
    if (wouldBlock)
        *wouldBlock = false;
    if (!m_channel)
        return {};

    QMutexLocker locker(&m_sessionMutex);
    QByteArray buf(maxBytes, 0);
    ssize_t n = libssh2_channel_read(m_channel, buf.data(), maxBytes);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        if (wouldBlock)
            *wouldBlock = true;
        return {};
    }
    if (n <= 0)
        return {}; // EOF or error
    buf.resize(int(n));
    return buf;
}

qint64 SshClient::writeChannel(const QByteArray &data)
{
    if (!m_channel)
        return -1;
    QMutexLocker locker(&m_sessionMutex);
    ssize_t written = 0;
    while (written < data.size()) {
        ssize_t n = libssh2_channel_write(m_channel, data.constData() + written, data.size() - written);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            locker.unlock();
            SshError dummy;
            if (!waitSocket(5000, dummy))
                return -1;
            locker.relock();
            continue;
        }
        if (n < 0)
            return -1;
        written += n;
    }
    return written;
}

void SshClient::resizePty(int width, int height)
{
    if (!m_channel)
        return;
    QMutexLocker locker(&m_sessionMutex);
    libssh2_channel_request_pty_size_ex(m_channel, width, height,
                                        LIBSSH2_TERM_WIDTH_PX, LIBSSH2_TERM_HEIGHT_PX);
}

bool SshClient::waitSocket(int timeoutMs, SshError &)
{
    if (!m_session)
        return false;
    // Read block directions under the lock, then release before select().
    int dir;
    {
        QMutexLocker locker(&m_sessionMutex);
        dir = libssh2_session_block_directions(m_session);
    }
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        FD_SET(m_sock, &rfds);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        FD_SET(m_sock, &wfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = ::select(int(m_sock) + 1, &rfds, &wfds, nullptr, &tv);
    return rc > 0;
}

// 关闭路径（主线程析构链：~SshSessionTab → ~SshBridge → closeChannel /
// disconnectFromHost）绝不能无限等 m_sessionMutex：若此刻另一线程（端口转发、
// 远程监控、shell 读循环）正持有该锁且阻塞在网络 IO 上，无限等锁会把主线程钉死
// 在 futex 上 → 窗口冻结、桌面环境弹"not responding"（gdb 实测主线程停在
// closeChannel → QBasicMutex::lockInternal → futex_wait_queue）。
// 因此这里用带超时的 tryLock：拿到锁才做优雅关闭；拿不到说明有线程正忙，放弃
// 优雅关闭——连接反正要断，本地句柄由 OS 在 socket close/进程退出时回收。
namespace {
constexpr int kCloseLockTimeoutMs = 1500;
}

// 在【已持有 m_sessionMutex】的前提下关闭并释放 shell channel。
// 关键：全程保持非阻塞，绝不 set_blocking(1)——阻塞模式下 libssh2_channel_close()
// 会一直等服务器回 SSH_MSG_CHANNEL_CLOSE，对端慢/半死时无限卡住主线程（这与上面
// 的锁等待是两个独立的卡死源）。非阻塞下只短暂重试：对端应答则优雅关闭，不应答
// 也直接 free——本地释放本就不依赖对端确认。
void SshClient::closeChannelLocked()
{
    if (!m_channel)
        return;
    LIBSSH2_CHANNEL *ch = m_channel;
    m_channel = nullptr;                  // 先摘下句柄，任何后续路径都不再复用它
    libssh2_channel_set_blocking(ch, 0);  // 保持非阻塞：绝不等服务器应答
    for (int i = 0; i < 20; ++i) {        // 最多 ~100ms 尝试优雅关闭
        if (libssh2_channel_close(ch) != LIBSSH2_ERROR_EAGAIN)
            break;
        if (m_sock < 0)
            break;                        // socket 已关，对端不可达
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000; // 让出 5ms 再重试
        ::select(0, nullptr, nullptr, nullptr, &tv);
    }
    libssh2_channel_free(ch);             // 无论对端是否应答，本地都释放
}

void SshClient::closeChannel()
{
    if (!m_sessionMutex.tryLock(kCloseLockTimeoutMs)) {
        qWarning() << "SshClient::closeChannel: session lock busy, skipping graceful"
                      " channel close to avoid blocking the UI thread";
        return; // 另一线程仍持有锁在做 IO；跳过优雅关闭，避免主线程死等。
    }
    struct UnlockGuard { QRecursiveMutex &m; ~UnlockGuard() { m.unlock(); } } guard{m_sessionMutex};
    closeChannelLocked();
}

void SshClient::shutdownSocket()
{
    // 只 shutdown，不 close：fd 的回收仍归 disconnectFromHost。shutdown(SHUT_RDWR)
    // 让阻塞在 select()/recv() 上的读循环/监控线程立刻拿到 EOF/错误返回，从而
    // 能快速重查各自的取消/运行标志退出——这是关闭标签页时避免 UI 线程死等
    // 网络超时的关键一步。线程安全（shutdown 可在任意线程调用，无需持锁）。
    if (m_sock < 0)
        return;
#ifdef Q_OS_WIN
    // m_sock 是 qintptr（有符号），SOCKET 是 UINT_PTR（无符号）：二者宽度相同
    // 但有符号性不同，reinterpret_cast 无法直接转（MSVC C2440）。经 uintptr_t
    // 过渡做等宽数值转换。
    ::shutdown(static_cast<SOCKET>(static_cast<uintptr_t>(m_sock)), SD_BOTH);
#else
    ::shutdown(static_cast<int>(m_sock), SHUT_RDWR);
#endif
}

void SshClient::disconnectFromHost()
{
    // 同 closeChannel：主线程关闭路径不能无限等 m_sessionMutex（见上注释）。
    // 拿不到锁就只关 socket（唤醒所有阻塞在 select/read 上的持锁线程），跳过
    // libssh2 层的优雅断开。
    if (!m_sessionMutex.tryLock(kCloseLockTimeoutMs)) {
        qWarning() << "SshClient::disconnectFromHost: session lock busy, closing"
                      " socket only (skipping graceful libssh2 disconnect)";
        if (m_sock >= 0) {
#ifdef Q_OS_WIN
            ::closesocket(m_sock);
#else
            ::close(m_sock);
#endif
            m_sock = -1;
        }
        return;
    }
    struct UnlockGuard { QRecursiveMutex &m; ~UnlockGuard() { m.unlock(); } } guard{m_sessionMutex};
    closeChannelLocked();
    if (m_session) {
        libssh2_session_disconnect(m_session, "bye");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_sock >= 0) {
#ifdef Q_OS_WIN
        ::closesocket(m_sock);
#else
        ::close(m_sock);
#endif
        m_sock = -1;
    }
}

// ==========================================================================
// Port-forwarding support (additive). Every libssh2 call below takes
// m_sessionMutex because channels from the forwarder threads share this
// session with the interactive shell channel and libssh2 is not thread-safe.
// ==========================================================================

_LIBSSH2_CHANNEL *SshClient::openDirectTcpip(const QString &destHost, quint16 destPort,
                                             const QString &srcHost, quint16 srcPort,
                                             SshError &error)
{
    QMutexLocker locker(&m_sessionMutex);
    if (!m_session) {
        error.message = QStringLiteral("Not connected");
        return nullptr;
    }
    const QByteArray dest = destHost.toUtf8();
    const QByteArray src = srcHost.toUtf8();
    // libssh2_channel_direct_tcpip_ex(session, host, port, shost, sport)
    _LIBSSH2_CHANNEL *ch = libssh2_channel_direct_tcpip_ex(
        m_session, dest.constData(), destPort, src.constData(), srcPort);
    if (!ch) {
        fillSessionError(m_session, error,
                         QStringLiteral("open direct-tcpip to %1:%2 failed")
                             .arg(destHost).arg(destPort));
        return nullptr;
    }
    return ch;
}

_LIBSSH2_LISTENER *SshClient::forwardListen(const QString &bindHost, quint16 bindPort,
                                            int *boundPort, SshError &error)
{
    QMutexLocker locker(&m_sessionMutex);
    if (!m_session) {
        error.message = QStringLiteral("Not connected");
        return nullptr;
    }
    const QByteArray host = bindHost.toUtf8();
    // Empty host == "" means listen on all interfaces server-side; libssh2
    // accepts nullptr for the default. We pass the host through as-is.
    _LIBSSH2_LISTENER *listener = libssh2_channel_forward_listen_ex(
        m_session,
        host.isEmpty() ? nullptr : host.constData(),
        bindPort, boundPort, /*queue_maxsize*/ 16);
    if (!listener) {
        fillSessionError(m_session, error,
                         QStringLiteral("request remote forward on port %1 failed")
                             .arg(bindPort));
        return nullptr;
    }
    return listener;
}

_LIBSSH2_CHANNEL *SshClient::forwardAccept(_LIBSSH2_LISTENER *listener)
{
    if (!listener)
        return nullptr;
    QMutexLocker locker(&m_sessionMutex);
    if (!m_session)
        return nullptr;
    // Non-blocking: returns nullptr and sets LIBSSH2_ERROR_EAGAIN when nothing
    // is pending. Caller is expected to poll with a small sleep.
    _LIBSSH2_CHANNEL *ch = libssh2_channel_forward_accept(listener);
    return ch; // nullptr on EAGAIN or error; caller treats both as "retry"
}

void SshClient::forwardCancel(_LIBSSH2_LISTENER *listener)
{
    if (!listener)
        return;
    QMutexLocker locker(&m_sessionMutex);
    if (!m_session)
        return;
    libssh2_channel_forward_cancel(listener);
}

QByteArray SshClient::channelRead(_LIBSSH2_CHANNEL *channel, int maxBytes, bool *wouldBlock)
{
    if (wouldBlock)
        *wouldBlock = false;
    if (!channel)
        return {};
    QMutexLocker locker(&m_sessionMutex);
    QByteArray buf(maxBytes, 0);
    ssize_t n = libssh2_channel_read(channel, buf.data(), maxBytes);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        if (wouldBlock)
            *wouldBlock = true;
        return {};
    }
    if (n <= 0)
        return {}; // EOF or error
    buf.resize(int(n));
    return buf;
}

qint64 SshClient::channelWrite(_LIBSSH2_CHANNEL *channel, const QByteArray &data)
{
    if (!channel)
        return -1;
    QMutexLocker locker(&m_sessionMutex);
    ssize_t written = 0;
    while (written < data.size()) {
        ssize_t n = libssh2_channel_write(channel, data.constData() + written,
                                          data.size() - written);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            // Drop the lock while waiting so the shell channel isn't starved.
            locker.unlock();
            SshError dummy;
            waitSocket(50, dummy);
            locker.relock();
            continue;
        }
        if (n < 0)
            return -1;
        written += n;
    }
    return written;
}

bool SshClient::channelEof(_LIBSSH2_CHANNEL *channel)
{
    if (!channel)
        return true;
    QMutexLocker locker(&m_sessionMutex);
    return libssh2_channel_eof(channel) != 0;
}

void SshClient::freeChannel(_LIBSSH2_CHANNEL *channel)
{
    if (!channel)
        return;
    QMutexLocker locker(&m_sessionMutex);
    libssh2_channel_set_blocking(channel, 1);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
}

bool SshClient::isTransportAlive() const
{
    QMutexLocker locker(&const_cast<SshClient *>(this)->m_sessionMutex);
    if (!m_session)
        return false;
    // authenticated() returns 0 when authenticated, non-zero otherwise.
    return libssh2_userauth_authenticated(const_cast<_LIBSSH2_SESSION *>(m_session)) != 0;
}

} // namespace cubeshell
