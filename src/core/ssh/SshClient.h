#pragma once

// SshClient.h — libssh2-backed SSH client.
//
// C++ replacement for the subset of paramiko used by cube-shell
// (function/ssh_func.py): password + public-key auth (Ed25519/RSA/ECDSA/DSS),
// keyboard-interactive MFA, opening a shell channel with a pty, and SFTP.
//
// This class is thread-hostile by design (like paramiko's SSHClient): one
// instance is driven from a single worker thread. The bridge layer
// (SshBridge) runs the read loop; the UI talks to it via Qt signals.

#include <QByteArray>
#include <QDeadlineTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

#include <QMutex>
#include <QRecursiveMutex>

#include "net/ProxyConnector.h"

struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_SFTP;
struct _LIBSSH2_LISTENER;

namespace cubeshell {

// Authentication methods supported, mirroring ssh_func.py.
enum class SshAuthMethod {
    Password,
    PublicKey,
    KeyboardInteractive // OTP / MFA prompt
};

// Result of a connection attempt.
struct SshError {
    int code = 0;            // libssh2 error code
    QString message;         // human-readable
    bool authFailed = false; // authentication specifically failed
};

// Callback invoked for each keyboard-interactive prompt. Return the response
// for the given prompt text. Used for OTP/MFA (see ssh_func.py's mfa_callback).
using SshPromptCallback = std::function<QString(const QString &prompt, bool echo)>;

class SshClient : public QObject {
    Q_OBJECT
public:
    explicit SshClient(QObject *parent = nullptr);
    ~SshClient() override;

    // Configure the connection target (call before connectToHost()).
    void setHost(const QString &host, quint16 port);
    void setUsername(const QString &username);
    void setPassword(const QString &password);
    // keyType mirrors paramiko key class names: "Ed25519Key", "RSAKey",
    // "ECDSAKey", "DSSKey". keyFile is the private-key path; passphrase optional.
    void setPrivateKey(const QString &keyType, const QString &keyFile, const QString &passphrase = QString());

    // Establish the TCP connection + SSH handshake + authenticate.
    // promptCallback is used for keyboard-interactive MFA; may be null.
    // Returns true on success; on failure returns false and fills error.
    bool connectToHost(SshPromptCallback promptCallback, SshError &error);

    // 用一个**已经连接好**的 fd 完成握手 + 认证，跳过拨号。
    //
    // 给跳板链的中间跳用（见 ssh/SshJumpChain）：第 2 跳及之后的传输是上一跳的
    // direct-tcpip 通道（经 socket pair 变成一个普通 fd），压根没有"拨号"这一步，
    // 也不该再去读它自己的 ProxyConfig——那条链已经在展平时算进去了。
    //
    // 本对象接管 sock 的所有权（disconnectFromHost 负责回收），失败时也一样。
    // transport 非空时一并接管，拆除顺序与 connectToHost 完全一致。
    bool connectOverSocket(qintptr sock, ProxyTransportPtr transport,
                           SshPromptCallback promptCallback, SshError &error);

    // 建连预算（毫秒），覆盖 TCP 建连（含代理握手）+ SSH 握手 + 认证三段。
    // <=0 表示用「设置 → 通用」里那个「SSH 连接超时」（见 effectiveConnectTimeoutMs）。
    //
    // 在此之前这三段**一个超时都没有**：TCP 是裸阻塞 ::connect，只能靠内核 SYN
    // 重传兜底（macOS 约 75 秒）；握手和认证则完全不设限。设置页那个
    // 「SSH 连接超时」spinbox 一直是死设置，没有任何调用点读它。
    void setConnectTimeoutMs(int timeoutMs) { m_connectTimeoutMs = timeoutMs; }
    int connectTimeoutMs() const { return m_connectTimeoutMs; }

    // 本次建连实际会用的预算：显式设过就用设置值，否则取「设置 → 通用」里那个
    // 「SSH 连接超时」（缺省 15 秒）。ConnectionTester 的兜底定时器要按它定时长，
    // 否则用户把超时调大之后兜底表会先到点，把正常握手误报成"连接超时"。
    int effectiveConnectTimeoutMs() const;

    // --- 代理 -------------------------------------------------------------

    // 本连接走哪种代理。proxy.type == Global 时取 globalProxy 那一份
    // （见 resolveGlobalProxy）；globalProxy 省略不传时由 connectToHost 自己去
    // 「设置 → 代理」里取。不调用等于直连，行为与加代理之前一致。
    //
    // 两份一起设是刻意的：SftpTransferPool::spawnClone 复制凭据时必须两份都
    // 复制过去，分成两个 setter 早晚漏一个——漏了的后果是克隆连接绕过代理
    // 直连，在内网里就是无声失败。
    void setProxyConfig(const ProxyConfig &proxy,
                        const ProxyConfig &globalProxy = ProxyConfig{});
    const ProxyConfig &proxyConfig() const { return m_proxy; }
    const ProxyConfig &globalProxyConfig() const { return m_globalProxy; }

    // 跳板机 / 代理命令的拨号器。net/ 层不许依赖 ssh/、也不起子进程，所以这两种
    // 类型走依赖倒置由外部注入（见 ProxyConnector.h 的分层说明）。
    // 没注入时对应类型给出可读错误，不会崩。
    void setJumpDialer(JumpDialer dialer) { m_jumpDialer = std::move(dialer); }
    void setCommandDialer(CommandDialer dialer) { m_commandDialer = std::move(dialer); }


    // Open an interactive shell channel with a pty.
    // term e.g. "xterm-256color"; width/height in characters.
    // Returns true on success.
    bool openShell(const QByteArray &term, int width, int height, SshError &error);

    bool isConnected() const;
    bool isChannelOpen() const;

    // Read-only accessors (ADDITIVE — used by CommandExecutor for the sudo -S
    // password feed and the root-user shortcut, mirroring ssh_func.py which
    // keeps username/password on the client).
    // 对应Python: function/ssh_func.py::SshClient.username / password 属性
    QString username() const { return m_username; }
    QString password() const { return m_password; }

    // 完整凭据只读访问（ADDITIVE —— 供 SftpTransferPool 克隆出并行传输专用的
    // 兄弟连接：libssh2 单个 LIBSSH2_SESSION 上的 SFTP 调用必须全程串行，
    // 要真并行就得每条流各占一个 session，即各开一条 SSH 连接）。
    QString host() const { return m_host; }
    quint16 port() const { return m_port; }
    QString keyType() const { return m_keyType; }
    QString keyFile() const { return m_keyFile; }
    QString passphrase() const { return m_passphrase; }

    // 本连接的认证方式是否可在无人交互的前提下重放（password / publickey 可以，
    // keyboard-interactive 的 OTP 一次一密不可以）。连接池据此决定能否克隆：
    // 返回 false 时退回单连接串行传输，不去骚扰用户再输一次动态码。
    //
    // 走跳板链时**整条链**都要算进来：目标机用密码、某台跳板用 OTP，重建一次
    // 连接仍然要管用户要一次动态码。见 ProxyTransport::isReplayable()。
    bool isAuthReplayable() const;

    // --- channel I/O (called from the bridge's threads) ---
    // Returns bytes read, or empty on EOF/closed. Sets *wouldBlock if the read
    // would block (caller should wait on the socket).
    QByteArray readChannel(int maxBytes, bool *wouldBlock = nullptr);
    // Writes all bytes; returns number written or -1 on error.
    qint64 writeChannel(const QByteArray &data);

    void resizePty(int width, int height);
    void closeChannel();
    void disconnectFromHost();

    // Forcibly shutdown() the underlying socket so any thread blocked in a
    // select()/read() on it wakes up immediately and returns an error. Used on
    // the tab-close path to unblock the monitor/reader threads BEFORE joining
    // them, so the UI thread never waits out a long network timeout. Does not
    // free the fd (that happens in disconnectFromHost); safe to call when the
    // socket is already closed.
    void shutdownSocket();

    // Access the raw socket fd (for poll/select in the read loop).
    qintptr socketFd() const { return m_sock; }

    // 把 session 与底层 fd 一起翻成非阻塞。
    //
    // 原本是 openShell() 内联的一段，提取出来是因为**中转跳板机的 session 也必须
    // 这么翻**，而它们从不调用 openShell（跳板机上没有 shell，只有 direct-tcpip
    // 通道）。阻塞模式的 session 上 libssh2_channel_read 会**持着 m_sessionMutex
    // 阻塞**（见 readChannel），双向中继于是必然死锁：单线程时那个读永远不返回、
    // 没有任何东西被写出去；双线程时写线程等的正是读线程持着的那把锁。
    //
    // fd 也要翻而不只是 session：libssh2 在 session_handshake 之后会把 socket
    // 恢复成原来的阻塞状态，不翻 fd 的话 recv() 仍会阻塞在 libssh2_channel_read
    // 里面，读循环永远等不到 EAGAIN。
    void setTransportNonBlocking();

    // Block until the channel socket is readable (or timeoutMs elapses).
    // Used by the bridge's read loop to avoid a busy spin on non-blocking reads.
    bool waitReadable(int timeoutMs) { SshError e; return waitSocket(timeoutMs, e); }

    // Internal: invoked by the libssh2 keyboard-interactive C callback for
    // each prompt. Public only so the C trampoline can reach it.
    QString handleKbdIntPrompt(const QString &prompt, bool echo);

    // ------------------------------------------------------------------
    // Port-forwarding support (ADDITIVE — used by the forwarder module).
    //
    // libssh2 multiplexes all channels over one LIBSSH2_SESSION and is NOT
    // thread-safe for concurrent channel operations on the same session. The
    // forwarder threads (local/remote/dynamic) each run on their own QThread
    // and open/read/write channels concurrently with the interactive shell
    // channel, so EVERY libssh2 call must be serialized through sessionLock().
    //
    // The helpers below already take the lock internally. Callers that need to
    // issue several libssh2 calls atomically (e.g. the pump loop's select +
    // read + write across a raw channel) may take sessionLock() themselves;
    // it is recursive so nested locking is safe.
    // ------------------------------------------------------------------

    // Recursive mutex guarding every libssh2 call on this session.
    QRecursiveMutex &sessionLock() { return m_sessionMutex; }

    // Raw LIBSSH2_SESSION accessor (ADDITIVE — used by SftpClient).
    // libssh2 multiplexes SFTP over the same session as the shell channel, so
    // every SFTP call must hold sessionLock(). After openShell() the session is
    // non-blocking, so SFTP callers must retry on LIBSSH2_ERROR_EAGAIN.
    // May be nullptr if not connected.
    _LIBSSH2_SESSION *rawSession() const { return m_session; }

    // Open a direct-tcpip channel (client-side of a local/dynamic forward).
    // dest is where the SSH *server* should connect; src is the originator
    // address reported to the server. Returns nullptr on failure (fills error).
    // The returned channel is owned by the caller; free with freeChannel().
    _LIBSSH2_CHANNEL *openDirectTcpip(const QString &destHost, quint16 destPort,
                                      const QString &srcHost, quint16 srcPort,
                                      SshError &error);

    // Request remote (server-side) port forwarding: ask the server to listen on
    // bindHost:bindPort and forward connections back to us. Returns a listener
    // handle on success; *boundPort (if non-null) receives the actual bound port
    // (relevant when bindPort == 0). Cancel with forwardCancel().
    _LIBSSH2_LISTENER *forwardListen(const QString &bindHost, quint16 bindPort,
                                     int *boundPort, SshError &error);
    // Accept one forwarded connection from a listener. Returns nullptr when no
    // connection is pending (non-blocking) — poll with a small sleep.
    _LIBSSH2_CHANNEL *forwardAccept(_LIBSSH2_LISTENER *listener);
    // Cancel a remote forwarding request and free the listener.
    void forwardCancel(_LIBSSH2_LISTENER *listener);

    // --- raw channel I/O for forwarding (lock taken internally) ---
    // Non-blocking read. Sets *wouldBlock on EAGAIN; returns empty on EOF/err.
    QByteArray channelRead(_LIBSSH2_CHANNEL *channel, int maxBytes, bool *wouldBlock = nullptr);
    // Blocking write of all bytes; returns bytes written or -1 on error.
    qint64 channelWrite(_LIBSSH2_CHANNEL *channel, const QByteArray &data);
    // True when the channel has reached EOF / been closed by the peer.
    bool channelEof(_LIBSSH2_CHANNEL *channel);
    // Close + free a forwarding channel obtained from openDirectTcpip/forwardAccept.
    void freeChannel(_LIBSSH2_CHANNEL *channel);

    // True if the SSH session transport is still alive and authenticated.
    bool isTransportAlive() const;

private:
    bool authenticate(SshError &error);
    bool authPassword(SshError &error);
    bool authPublicKey(SshError &error);
    bool authKeyboardInteractive(SshPromptCallback cb, SshError &error);
    bool waitSocket(int timeoutMs, SshError &error);

    // 接管一个已连接的 fd，做 session_init → handshake → authenticate。
    // connectToHost（拨号之后）与 connectOverSocket（跳板链的中间跳）共用这一段
    // ——两者从这里往下逐字节相同，分开写早晚有一处漏掉超时或诊断。
    bool finishHandshake(qintptr sock, QDeadlineTimer budget, SshError &error);
    // 在已持有 m_sessionMutex 的前提下非阻塞地关闭并释放 shell channel。
    // 供 closeChannel()/disconnectFromHost() 在持锁后调用（见 .cpp 注释）。
    void closeChannelLocked();

    // 拆掉代理链的载体（若有）。必须**在 socket 关闭之后**调用，理由见 .cpp。
    void releaseTransport();

    // 把载体侧攒下的诊断（子进程 stderr、退出码）附到错误消息后面。
    // 要在 disconnectFromHost() 之前调用——那一步会把载体连带诊断一起拆掉。
    void appendTransportDiagnostics(SshError &error) const;

    QString m_host;
    quint16 m_port = 22;
    QString m_username;
    QString m_password;
    QString m_keyType;
    QString m_keyFile;
    QString m_passphrase;
    SshPromptCallback m_promptCallback;

    // authenticate() 成功走的是 password/publickey（可重放）还是
    // keyboard-interactive（不可重放）。见 isAuthReplayable()。
    bool m_authReplayable = false;

    // 建连预算（毫秒）。<=0 = 由 effectiveConnectTimeoutMs() 去「设置」里取。
    // 见 setConnectTimeoutMs 的注释：在此之前这三段一个超时都没有。
    int m_connectTimeoutMs = 0;

    ProxyConfig m_proxy;
    ProxyConfig m_globalProxy;
    JumpDialer    m_jumpDialer;
    CommandDialer m_commandDialer;

    // 代理链的载体（代理命令的子进程 + 泵线程；跳板链的整条 SSH 会话链）。
    // 直连 / Http / Socks5 / 系统代理都是空的——那几种握手完就是同一个 fd，
    // 背后没有额外活物。它的存活期必须**恰好**覆盖 m_sock 的存活期，见
    // ProxyTransport 的注释和 releaseTransport()。
    //
    // 单独一把锁：shutdownSocket() 明确允许在任意线程无锁调用，而它要碰这个
    // 成员；拿 m_sessionMutex 会让「关标签页」重新变成可能死等 I/O 的操作，
    // 那正是 shutdownSocket 存在的意义。这把锁**不许跨 I/O 持有**。
    mutable QMutex m_transportMutex;
    ProxyTransportPtr m_transport;

    // 建连（含整条代理链）途中的取消标志。
    //
    // 为什么不能只靠 shutdownSocket()：那个函数在 m_sock < 0 时直接 return，
    // 而建代理链的整个过程里 m_sock 都还是 -1（要等最后拿到 fd 才赋值）。用户在
    // 第 3 跳还在连的时候关掉标签页，没有这个标志就只能等到 OS 超时。
    std::atomic<bool> m_dialCancelled{false};

    qintptr m_sock = -1;
    // socket 已被 shutdownSocket() 打断（取消传输/关标签页）。isTransportAlive
    // 必须把它算进去——libssh2_userauth_authenticated 只是本地标志，socket
    // 死了它仍为真，连接池据此会把死连接继续租出去（open 必失败）。
    std::atomic<bool> m_socketShutdown{false};
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_CHANNEL *m_channel = nullptr;
    QRecursiveMutex m_sessionMutex; // serializes all libssh2 calls on m_session
};

} // namespace cubeshell
