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
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include <QMutex>
#include <QRecursiveMutex>

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
    // 在已持有 m_sessionMutex 的前提下非阻塞地关闭并释放 shell channel。
    // 供 closeChannel()/disconnectFromHost() 在持锁后调用（见 .cpp 注释）。
    void closeChannelLocked();

    QString m_host;
    quint16 m_port = 22;
    QString m_username;
    QString m_password;
    QString m_keyType;
    QString m_keyFile;
    QString m_passphrase;
    SshPromptCallback m_promptCallback;

    qintptr m_sock = -1;
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_CHANNEL *m_channel = nullptr;
    QRecursiveMutex m_sessionMutex; // serializes all libssh2 calls on m_session
};

} // namespace cubeshell
