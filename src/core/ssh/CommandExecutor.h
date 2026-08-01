#pragma once

// CommandExecutor.h — one-shot / streaming remote command execution over an
// existing SshClient session (libssh2 exec channels).
//
// C++ port of the command-execution family of function/ssh_func.py plus the
// streaming/sudo helpers of core/ai/ssh_agent.py::_CommandExecutorWorker:
//   - exec()       对应Python: function/ssh_func.py::SshClient._exec / exec
//   - sudoExec()   对应Python: function/ssh_func.py::SshClient._sudo_exec / sudo_exec
//   - execScript() 对应Python: core/frp_manager.py 上传+执行模式（sftp.put + exec 组合）
//   - execStream() 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._stream_read_output
//
// Threading model: the synchronous calls (exec/sudoExec/execScript) may be
// invoked from any worker thread — every libssh2 call is serialized through
// SshClient::sessionLock(), and the lock is dropped while waiting on the
// socket so the interactive shell channel is never starved. execStream()
// spawns its own QThread and emits outputChunk() from that thread; consumers
// on the UI thread MUST connect with Qt::QueuedConnection.

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

class QThread;

namespace cubeshell {

class SshClient;

// Result of a synchronous remote command.
// 对应Python: paramiko exec_command 的 (stdout, stderr, exit_status) 三元组
struct ExecResult {
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    bool timedOut = false;
    bool cancelled = false;
    QString errorMessage; // transport/channel level failure (empty on success)

    bool ok() const { return errorMessage.isEmpty() && !timedOut && !cancelled; }
};

class CommandExecutor : public QObject {
    Q_OBJECT
public:
    // 对应Python: ssh_func.py::_exec 的 timeout=30（秒）
    static constexpr int kDefaultTimeoutMs = 30 * 1000;
    // 对应Python: ssh_agent.py::_execute_single 的 timeout = 600 if long else 120
    static constexpr int kLongRunningTimeoutMs = 600 * 1000;
    static constexpr int kNormalStreamTimeoutMs = 120 * 1000;
    // Throttle interval for outputChunk emission (UI SIGABRT protection).
    // 对应Python: ssh_agent.py::_CommandExecutorWorker._EMIT_INTERVAL = 0.2
    static constexpr int kEmitIntervalMs = 200;

    // client must outlive this executor. The executor does not take ownership.
    explicit CommandExecutor(SshClient *client, QObject *parent = nullptr);
    ~CommandExecutor() override;

    // --- synchronous execution (blocking; call from a worker thread) ---

    // Run a one-shot command; returns stdout/stderr/exit code.
    // 对应Python: function/ssh_func.py::SshClient.exec / _exec
    ExecResult exec(const QString &cmd, bool pty = false, int timeoutMs = kDefaultTimeoutMs);

    // Run a command with sudo: for non-root users the command is wrapped in
    // `sudo -S <cmd>` and the login password is written to stdin.
    // 对应Python: function/ssh_func.py::SshClient.sudo_exec / _sudo_exec
    ExecResult sudoExec(const QString &cmd, bool pty = false, int timeoutMs = kDefaultTimeoutMs);

    // Upload a script to a remote temp file, chmod +x, execute it with the
    // given arguments and remove it afterwards; the script's exit code is
    // propagated. Upload uses `cat > file` on an exec channel (no SFTP
    // dependency), mirroring the upload+execute pattern of the Python side.
    // 对应Python: core/frp_manager.py::deploy_frps（sftp.put + exec 组合）
    ExecResult execScript(const QByteArray &scriptContent,
                          const QStringList &args = QStringList(),
                          int timeoutMs = kLongRunningTimeoutMs);

    // --- streaming execution (asynchronous) ---

    // Execute a long-running command on a dedicated QThread and emit
    // outputChunk() for every block read from the channel (stdout+stderr
    // interleaved in arrival order, throttled to kEmitIntervalMs). \r progress
    // updates from wget/curl are forwarded verbatim. When the command needs a
    // sudo password the prompt is detected and the password written to stdin.
    // timeoutMs < 0 selects automatically: kLongRunningTimeoutMs for
    // isLongRunningCommand(), kNormalStreamTimeoutMs otherwise.
    // Only one stream may run at a time; returns false if one is active.
    // 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._stream_read_output
    bool execStream(const QString &cmd, bool pty = false, int timeoutMs = -1);

    bool isStreaming() const { return m_streaming.load(); }

    // Request cancellation of the running command (sync or stream). The
    // worker notices the flag at the next read iteration.
    // 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker.request_stop
    void cancel() { m_cancelRequested.store(true); }
    bool isCancelRequested() const { return m_cancelRequested.load(); }

    // Block until the streaming thread has finished (joins the QThread).
    void waitForFinished();

    // --- pure helpers (unit-testable, no network) ---

    // True when cmd contains a bare `sudo` (no -S) and username != root.
    // 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._needs_sudo_password
    static bool needsSudoPassword(const QString &cmd, const QString &username);

    // Rewrite `sudo` → `sudo -S` so the password can be fed via stdin.
    // 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._inject_sudo_stdin_flag
    static QString injectSudoStdinFlag(const QString &cmd);

    // Heuristic for download/install commands that need streaming output.
    // 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._is_long_running_command
    static bool isLongRunningCommand(const QString &cmd);

    // True when a chunk of output looks like a sudo password prompt.
    // 对应Python: ssh_func.py::_sudo_exec 写密码时机的显式化（[sudo] password for ...）
    static bool looksLikeSudoPasswordPrompt(const QString &text);

    // Low-level blocking runner shared with RemoteMonitor (no signals, safe
    // to call from any thread). stdinData (if non-empty) is written to the
    // command's stdin followed by EOF. cancelFlag may be null.
    // 对应Python: function/ssh_func.py::SshClient._exec（paramiko exec_command 等价）
    static ExecResult runCommand(SshClient *client, const QString &cmd, bool pty,
                                 int timeoutMs,
                                 const QByteArray &stdinData = QByteArray(),
                                 std::atomic<bool> *cancelFlag = nullptr);

signals:
    // Emitted from the streaming worker thread — connect with Qt::QueuedConnection.
    // 对应Python: ssh_agent.py 的 output_stream 信号（_throttled_emit）
    void outputChunk(const QByteArray &chunk);
    // exitCode == -1 on transport failure/timeout/cancel.
    void streamFinished(int exitCode, const QString &stdoutText, const QString &stderrText);
    void streamError(const QString &message);

private:
    // Chunk callback: (data, isStderr) -> bytes to write back to stdin (or empty).
    using ChunkCallback = std::function<QByteArray(const QByteArray &, bool)>;

    static ExecResult runCommandInternal(SshClient *client, const QString &cmd, bool pty,
                                         int timeoutMs, const QByteArray &stdinData,
                                         std::atomic<bool> *cancelFlag,
                                         const ChunkCallback &onChunk);

    void streamLoop(const QString &cmd, bool pty, int timeoutMs);

    SshClient *m_client;
    QThread *m_streamThread = nullptr;
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_cancelRequested{false};
};

} // namespace cubeshell
