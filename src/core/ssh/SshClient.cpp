// SshClient.cpp — libssh2-backed SSH client. See SshClient.h.

#include "SshClient.h"

#include <QDeadlineTimer>
#include <QLoggingCategory>
#include <QMutexLocker>

#include <libssh2.h>

#include <cstring>

#include "config/GlobalState.h"
#include "net/SocketUtil.h"
#include "SshJumpChain.h"

#ifdef CUBESHELL_WITH_LOCALPROC
#  include "net/ProxyCommandDialer.h"
#endif

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

// 本次建连的总预算（毫秒）。显式设过就用设置值，否则取设置页「SSH 连接超时」。
//
// 这个预算要覆盖 TCP 建连（含代理握手）+ SSH 握手 + 认证三段。在此之前这三段
// **一个超时都没有**：TCP 是裸阻塞 ::connect，只能靠内核 SYN 重传兜底
// （macOS 约 75 秒）；握手和认证则完全不设限。设置页那个 spinbox 一直是死设置，
// 没有任何调用点读它。
//
// 在工作线程上读 GlobalState 是安全的：那个单例的 m_theme 已由 m_themeMutex
// 保护（见 GlobalState.h——这正是为它加锁的直接原因）。
int SshClient::effectiveConnectTimeoutMs() const
{
    int base = m_connectTimeoutMs;
    if (base <= 0) {
        const int seconds = GlobalState::instance().sshConnectTimeoutSeconds();
        base = seconds > 0 ? seconds * 1000 : socket_util::kDefaultConnectTimeoutMs;
    }
    // 跳板机链按跳数放大。三跳链要做 4 次「TCP + 握手 + 认证」，共用一份 base
    // 必然到点：用户把超时设成 15 秒，每跳只剩不到 4 秒，于是**任何**多跳配置
    // 都会稳定误报超时。放大之后每跳拿到的仍是用户设定的那个量级。
    //
    // 一并修好了「测试连接」：ConnectionTester 的兜底表就是照这个函数算的
    //（见 sshGuardMsFor），不放大的话那个按钮对多跳配置永远显示失败。
    //
    // hopIds 是**直接**跳板数；嵌套引用（跳板自己又配了跳板）展平后更多，
    // 那时每跳分到的不足 base。这是有意的下界——宁可让极端嵌套配置偏紧，
    // 也不要让一个写坏的配置把超时放大到几分钟。
    if (m_proxy.type == ProxyType::JumpHost)
        return base * (1 + int(m_proxy.hopIds.size()));
    return base;
}

void SshClient::setProxyConfig(const ProxyConfig &proxy, const ProxyConfig &globalProxy)
{
    m_proxy = proxy;
    m_globalProxy = globalProxy;
}

bool SshClient::connectToHost(SshPromptCallback promptCallback, SshError &error)
{
    m_promptCallback = std::move(promptCallback);
    // 上一次连接留下的取消标记不能带进这一次（连接池会复用同一个对象重连）。
    m_dialCancelled = false;

    // 三段共享一份预算：先建连（含代理握手），剩下的给握手 + 认证。
    QDeadlineTimer budget(effectiveConnectTimeoutMs());

    // 选了「全局代理」但调用方没显式给出全局那一份时，自己去设置里取。
    // 这样 5 个建连入口（终端 / 测试连接 / 隧道 / SFTP 克隆 / FRP）都不必各自
    // 记得读一次设置——漏读的后果是"全局代理"静默退化成直连。
    ProxyConfig globalProxy = m_globalProxy;
    if (m_proxy.type == ProxyType::Global && globalProxy.type == ProxyType::None) {
        globalProxy = GlobalState::instance().sshProxyConfig();
        // 口令不在 theme.json 里（明文只进钥匙串），得单独补。
        // 不补的话"要认证的全局代理"会表现成一句代理认证失败——而设置页里
        // 口令明明填着，没人能从错误消息里看出是这一步丢了。
        globalProxy.password = GlobalState::instance().sshProxyPassword();
    }

    // --- TCP connect（按需经代理）---
    // 原来是 getaddrinfo + 逐地址裸阻塞 ::connect，没有超时也无法取消。
    // proxyConnect 无论走哪种代理，交回来的都是一个**真的、可 select 的**阻塞
    // 模式 fd，且已设好 SO_NOSIGPIPE / TCP_NODELAY——与那段裸 ::connect 的产物
    // 状态一致，所以下面的 handshake / openShell / socketFd() 一行都不用改。
    ProxyDialRequest dial;
    dial.host          = m_host;
    dial.port          = m_port;
    dial.user          = m_username;
    dial.proxy         = m_proxy;
    dial.globalProxy   = globalProxy;
    dial.timeoutMs     = int(budget.remainingTime());
    dial.cancelled     = &m_dialCancelled;
    // 默认的跳板拨号器也就地补上，理由与下面代理命令那段完全相同（5 个建连入口）。
    // 凭据取自 GlobalState 里那份由 UI 线程推来的快照——工作线程不许碰
    // DeviceConfigStore（它没有锁），见 GlobalState::setJumpHostCatalog。
    //
    // **每次现造，不缓存到 m_jumpDialer**：它要捕获本次连接的 promptCallback
    // （链上每跳都可能要动态码，提示文本得前缀上跳板机名才分得清是谁在问），
    // 而那个回调是 connectToHost 的入参，重连时可以是另一个。外部显式注入过的
    // （测试里塞的假拨号器）优先，不覆盖。
    dial.jumpDialer = m_jumpDialer
                          ? m_jumpDialer
                          : makeSshJumpDialer(GlobalState::instance().jumpHostCatalog(),
                                              m_promptCallback);
#ifdef CUBESHELL_WITH_LOCALPROC
    // 默认的代理命令拨号器就地补上，而不是让每个调用方各自注入：connectToHost
    // 有 5 个调用点（终端 / 连接测试 / 隧道 / SFTP 并行克隆 / frp），靠每处记得
    // 注入，早晚有一处漏掉——漏掉的表现是那条路径上「代理命令」静默不生效。
    // 已经注入过的（测试里塞的假拨号器）不覆盖。
    if (!m_commandDialer)
        m_commandDialer = makeProxyCommandDialer();
#endif
    dial.commandDialer = m_commandDialer;

    QString connectErr;
    // 载体先落在局部变量里，成功之后再在锁内挂到 m_transport 上。直接把
    // &m_transport 交给 proxyConnect 会让「拨号线程正在写」和「shutdownSocket
    // 在另一条线程读」撞上——shared_ptr 的赋值不是原子的。
    ProxyTransportPtr transport;
    const qintptr sock = proxyConnect(dial, &transport, &connectErr);
    {
        QMutexLocker lock(&m_transportMutex);
        m_transport = transport;
    }
    if (sock < 0) {
        error.message = connectErr.isEmpty()
                            ? QStringLiteral("Cannot connect to %1:%2").arg(m_host).arg(m_port)
                            : connectErr;
        // 拨号本身失败时载体一般已经自己拆干净了，但「起得来、随即退出」这种
        // 形态会留下 stderr——那正是唯一说清病因的东西。
        appendTransportDiagnostics(error);
        transport.reset();
        releaseTransport();
        return false;
    }

    m_sock = sock;
    m_socketShutdown = false; // 新 socket 就绪，清除可能残留的 shutdown 标记

    return finishHandshake(sock, budget, error);
}

// 跳板链的中间跳走这里：传输是上一跳的 direct-tcpip 通道（已经变成一个普通 fd），
// 没有拨号这一步。预算仍按 effectiveConnectTimeoutMs 算——链构建器会先给每跳
// setConnectTimeoutMs(逐跳预算)，所以这里取到的就是那一份。
bool SshClient::connectOverSocket(qintptr sock, ProxyTransportPtr transport,
                                  SshPromptCallback promptCallback, SshError &error)
{
    m_promptCallback = std::move(promptCallback);
    m_dialCancelled = false;

    if (sock < 0) {
        error.message = QStringLiteral("内部错误：跳板链交来的 socket 无效");
        return false;
    }

    {
        QMutexLocker lock(&m_transportMutex);
        m_transport = std::move(transport);
    }

    m_sock = sock;
    m_socketShutdown = false;

    return finishHandshake(sock, QDeadlineTimer(effectiveConnectTimeoutMs()), error);
}

// session_init → handshake → authenticate。调用方已经把 sock 挂到 m_sock 上。
bool SshClient::finishHandshake(qintptr sock, QDeadlineTimer budget, SshError &error)
{
    Q_UNUSED(sock);

    // --- SSH session + handshake ---
    m_session = libssh2_session_init();
    if (!m_session) {
        error.message = QStringLiteral("libssh2_session_init failed");
        return false;
    }
    libssh2_session_set_blocking(m_session, 1); // blocking for connect/auth simplicity

    // 阻塞模式下 libssh2 的每个 API 调用都受这个超时约束（0 = 永不超时，
    // 也就是改之前的行为）。握手和认证都要读远端应答，服务器只完成 TCP
    // 三次握手却不发 banner 时，没有它就是永久挂住。
    libssh2_session_set_timeout(m_session, long(qMax(qint64(1), budget.remainingTime())));

    if (libssh2_session_handshake(m_session, m_sock) != 0) {
        fillSessionError(m_session, error, QStringLiteral("SSH handshake failed"));
        // 走代理时这一步的失败**几乎总是**代理链的问题而不是 SSH 的问题：载体
        // 进程起得来、然后立刻因为 connection refused 之类退出，libssh2 只会说
        // 一句 "Failed getting banner"。诊断要在 disconnectFromHost 之前取——
        // 那一步会把载体连带它攒的 stderr 一起拆掉。
        appendTransportDiagnostics(error);
        disconnectFromHost();
        return false;
    }

    // --- authenticate ---
    libssh2_session_set_timeout(m_session, long(qMax(qint64(1), budget.remainingTime())));
    if (!authenticate(error)) {
        appendTransportDiagnostics(error);
        disconnectFromHost();
        return false;
    }

    // 建连预算只管建连。留着它会让后续所有阻塞调用（SFTP 传输、命令执行）
    // 都带上一个几秒的上限，大文件传输会被判成超时。
    libssh2_session_set_timeout(m_session, 0);

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
    if (libssh2_userauth_password(m_session, user.constData(), pass.constData()) == 0) {
        m_authReplayable = true; // 密码可重放 -> 允许克隆并行传输连接
        return true;
    }
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
    if (rc == 0) {
        m_authReplayable = true; // 密钥文件可重复使用 -> 允许克隆并行传输连接
        return true;
    }
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
    if (rc == 0) {
        // OTP 一次一密，无法静默重放 -> 传输连接池不得克隆本连接。
        m_authReplayable = false;
        return true;
    }
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

    setTransportNonBlocking();

    return true;
}

// 见 SshClient.h 里的注释：为什么 session 与 fd 两层都得翻，以及为什么这段
// 必须能被 openShell() 之外的调用方（中转跳板机的 session）单独调到。
void SshClient::setTransportNonBlocking()
{
    if (m_session)
        libssh2_session_set_blocking(m_session, 0);
    if (m_sock >= 0)
        socket_util::setNonBlocking(m_sock, true);
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
    // 先置建连取消标志，再看 m_sock。顺序很要紧：整条代理链还在建的时候
    // m_sock 仍是 -1，下面那个 early-return 会让这个函数什么都不做——用户在
    // 第 3 跳还在连的时候关标签页，就只能等到 OS 超时。标志由 proxyConnect
    // 一路传到每个 select 切片里，最多多等一个切片（kSelectSliceMs）。
    m_dialCancelled = true;

    // 载体也要一起叫醒（代理命令的泵线程 / 跳板链的中继线程）。不叫醒的话它会
    // 一直守着一个已经没人读的 fd，关掉一个标签页就留下一个线程 + 一个子进程。
    // 取本地拷贝再调用：shutdown() 里可能 join 线程，不能持锁做。
    ProxyTransportPtr transport;
    {
        QMutexLocker lock(&m_transportMutex);
        transport = m_transport;
    }
    if (transport)
        transport->shutdown();

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
    // 标记传输已死：isTransportAlive 据此返回 false，连接池不会把这条
    // 连接再租出去（取消下载后立刻重新下载踩到的就是这个）。
    m_socketShutdown = true;
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
        // 这条路径也必须拆载体。忘了它就是每次「锁忙」都漏一个子进程 + 一个
        // 泵线程，而锁忙恰恰发生在关标签页这种最常见的时刻。
        releaseTransport();
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
    // 最后才拆载体，顺序不能提前：上面那句 libssh2_session_disconnect 的 "bye"
    // 还要经过载体发出去，先拆等于在说话中间把话筒拔掉。
    releaseTransport();
}

void SshClient::releaseTransport()
{
    ProxyTransportPtr transport;
    {
        QMutexLocker lock(&m_transportMutex);
        transport.swap(m_transport);
    }
    // 出了上面那个作用域才析构（也就是才真正 join 泵线程）：持着
    // m_transportMutex 做 join 会把并发的 shutdownSocket() 一起堵住，
    // 而 shutdownSocket 的全部意义就是"不会堵"。
}

void SshClient::appendTransportDiagnostics(SshError &error) const
{
    ProxyTransportPtr transport;
    {
        QMutexLocker lock(&m_transportMutex);
        transport = m_transport;
    }
    if (!transport)
        return;
    const QString diag = transport->diagnostics().trimmed();
    if (diag.isEmpty())
        return;
    error.message += QStringLiteral("\n代理载体输出：%1").arg(diag);
}

// 本连接 + 整条载体链是否都能在无人交互下重放。见 isAuthReplayable() 的声明处。
bool SshClient::isAuthReplayable() const
{
    if (!m_authReplayable)
        return false;
    ProxyTransportPtr transport;
    {
        QMutexLocker lock(&m_transportMutex);
        transport = m_transport;
    }
    // 直连 / Http / Socks5 / 系统代理没有载体，只看自己那一段。
    return !transport || transport->isReplayable();
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
    // socket 已被 shutdown（取消传输/关标签页）：authenticated 标志仍是真，
    // 但传输已经死了，必须报死——否则连接池会把死连接租出去。
    if (m_socketShutdown.load())
        return false;
    QMutexLocker locker(&const_cast<SshClient *>(this)->m_sessionMutex);
    if (!m_session)
        return false;
    // authenticated() returns 0 when authenticated, non-zero otherwise.
    return libssh2_userauth_authenticated(const_cast<_LIBSSH2_SESSION *>(m_session)) != 0;
}

} // namespace cubeshell
