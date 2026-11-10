// CommandExecutor.cpp — remote command execution over libssh2 exec channels.
// See CommandExecutor.h for the paramiko/ssh_func.py mapping.

#include "CommandExecutor.h"
#include "SshClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStringList>
#include <QThread>

#include <libssh2.h>

Q_DECLARE_LOGGING_CATEGORY(cmdExecLog)
Q_LOGGING_CATEGORY(cmdExecLog, "cubeshell.ssh.exec")

namespace cubeshell {

// POSIX single-quote escaping: ' → '\'' , then wrap in single quotes.
// 对应Python: core/ai/ssh_agent.py::_execute_single 的 cmd.replace("'", "'\\''")
static QString shellQuote(const QString &s)
{
    QString escaped = s;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

CommandExecutor::CommandExecutor(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

CommandExecutor::CommandExecutor(std::shared_ptr<SshClient> client, QObject *parent)
    : CommandExecutor(client.get(), parent)
{
    m_ownedClient = std::move(client);
}

bool CommandExecutor::hasUsableConnection() const
{
    // UI 只读取已知连接状态，不等待可能正被其它通道占用的 session 锁；
    // 工作线程进入 runCommandInternal() 时再检查认证状态。
    return m_client && m_client->isConnected();
}

CommandExecutor::~CommandExecutor()
{
    // Never destroy while the worker still runs: cancel, then join the
    // thread (对应记忆规范：对象销毁前线程必须 join，禁止销毁后 emit)。
    cancel();
    waitForFinished();
}

void CommandExecutor::waitForFinished()
{
    if (m_streamThread) {
        m_streamThread->wait();
        delete m_streamThread;
        m_streamThread = nullptr;
    }
}

// ==========================================================================
// Pure helpers (unit-testable)
// ==========================================================================

// 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._needs_sudo_password
bool CommandExecutor::needsSudoPassword(const QString &cmd, const QString &username)
{
    if (username == QStringLiteral("root"))
        return false;
    static const QRegularExpression sudoRe(QStringLiteral("\\bsudo\\b"));
    if (!sudoRe.match(cmd).hasMatch())
        return false;
    static const QRegularExpression hasFlagRe1(QStringLiteral("\\bsudo\\s+.*-S"));
    static const QRegularExpression hasFlagRe2(QStringLiteral("\\bsudo\\s+-S"));
    if (hasFlagRe1.match(cmd).hasMatch() || hasFlagRe2.match(cmd).hasMatch())
        return false;
    return true;
}

// 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._inject_sudo_stdin_flag
QString CommandExecutor::injectSudoStdinFlag(const QString &cmd)
{
    static const QRegularExpression re(QStringLiteral("\\bsudo\\b(?!\\s*-S)"));
    QString out = cmd;
    out.replace(re, QStringLiteral("sudo -S"));
    return out;
}

// 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker._is_long_running_command
bool CommandExecutor::isLongRunningCommand(const QString &cmd)
{
    static const char *patterns[] = {
        R"(\bwget\b)", R"(\bcurl\b.*(-o|-O|--output))",
        R"(\bcurl\b.*\|)",
        R"(\bapt\b)", R"(\bapt-get\b)",
        R"(\byum\b)", R"(\bdnf\b)",
        R"(\bpacman\b)", R"(\bzypper\b)",
        R"(\bpip\s+install\b)", R"(\bnpm\s+install\b)",
        R"(\byarn\s+(add|install)\b)",
        R"(\bmake\b)", R"(\bmvn\b)", R"(\bgradle\b)",
        R"(\bdocker\s+(pull|build)\b)",
        R"(\bgit\s+clone\b)",
        R"(\brsync\b)", R"(\bscp\b)",
    };
    for (const char *p : patterns) {
        if (QRegularExpression(QString::fromLatin1(p)).match(cmd).hasMatch())
            return true;
    }
    return false;
}

// 对应Python: ssh_func.py::_sudo_exec 写密码时机（sudo -S 的提示出现在 stderr/pty）
bool CommandExecutor::looksLikeSudoPasswordPrompt(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral("(\\[sudo\\][^\\n]*(password|密码)[^:：\\n]*[:：])"
                       "|((^|\\n)\\s*password(\\s+for\\s+\\S+)?\\s*[:：]\\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

// ==========================================================================
// Low-level blocking runner
// ==========================================================================

// Fill an ExecResult error from the current libssh2 session error.
static void fillExecError(LIBSSH2_SESSION *session, ExecResult &result, const QString &context,
                          int returnedCode = 0)
{
    char *msg = nullptr;
    int len = 0;
    const int lastCode = session ? libssh2_session_last_error(session, &msg, &len, 0) : 0;
    const int code = returnedCode != 0 ? returnedCode : lastCode;
    result.sshErrorCode = code;
    result.errorMessage = context;
    if (code == lastCode && msg && len > 0)
        result.errorMessage += QStringLiteral(": ") + QString::fromLatin1(msg, len);
    result.errorMessage += QStringLiteral(" (code %1)").arg(code);
}

// Wait for the socket to become readable, but bail out early (returning false)
// as soon as the cancel flag is raised. The wait is sliced into short ~25ms
// polls so a stop() request is honored within tens of milliseconds instead of
// after the full timeoutMs. This is what lets the monitor thread (whose reads
// sit in these waits) die almost immediately on tab close, rather than after
// up to 30s, so the UI thread is never parked waiting on it.
static bool waitReadableCancellable(SshClient *client, int timeoutMs,
                                    std::atomic<bool> *cancelFlag)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cancelFlag && cancelFlag->load())
            return false;
        const int slice = int(qMin<qint64>(25, timeoutMs - t.elapsed()));
        if (client->waitReadable(slice))
            return true;
    }
    return false;
}

// Write all bytes to the channel's stdin, dropping the session lock while
// waiting on EAGAIN so the interactive shell channel is never starved.
// Returns false on error or when the cancel flag is raised mid-write.
static bool channelWriteAll(SshClient *client, LIBSSH2_CHANNEL *channel,
                            const QByteArray &data, std::atomic<bool> *cancelFlag,
                            int &errorCode)
{
    QRecursiveMutex &lock = client->sessionLock();
    qsizetype written = 0;
    while (written < data.size()) {
        if (cancelFlag && cancelFlag->load())
            return false;
        ssize_t n;
        {
            QMutexLocker locker(&lock);
            n = libssh2_channel_write(channel, data.constData() + written,
                                      size_t(data.size() - written));
        }
        if (n == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReadableCancellable(client, 50, cancelFlag) &&
                cancelFlag && cancelFlag->load())
                return false;
            continue;
        }
        if (n < 0) {
            errorCode = int(n);
            return false;
        }
        written += n;
    }
    return true;
}

ExecResult CommandExecutor::runCommand(SshClient *client, const QString &cmd, bool pty,
                                       int timeoutMs, const QByteArray &stdinData,
                                       std::atomic<bool> *cancelFlag)
{
    return runCommandInternal(client, cmd, pty, timeoutMs, stdinData, cancelFlag, nullptr);
}

// 对应Python: function/ssh_func.py::SshClient._exec（conn.exec_command + read）
ExecResult CommandExecutor::runCommandInternal(SshClient *client, const QString &cmd, bool pty,
                                               int timeoutMs, const QByteArray &stdinData,
                                               std::atomic<bool> *cancelFlag,
                                               const ChunkCallback &onChunk)
{
    ExecResult result;
    if (!client || !client->isTransportAlive()) {
        result.errorMessage = QStringLiteral("SSH 连接不可用，请重新连接后重试");
        return result;
    }
    LIBSSH2_SESSION *session = client->rawSession();
    QRecursiveMutex &lock = client->sessionLock();

    // 在所有 session 锁释放后统一上报传输失败。单独保存错误快照，避免返回值
    // 被移动后 guard 读到空字符串；普通命令失败不应使整个 SSH 会话断开。
    int transportCode = 0;
    QString transportMessage;
    const auto reportFailure = qScopeGuard([&]() {
        client->reportTransportError(transportCode, transportMessage);
    });
    auto recordError = [&](const QString &context, int code = 0) {
        fillExecError(session, result, context, code);
        transportCode = result.sshErrorCode;
        transportMessage = result.errorMessage;
    };

    QElapsedTimer clock;
    clock.start();
    auto timedOut = [&]() { return timeoutMs > 0 && clock.elapsed() >= timeoutMs; };
    auto isCancelled = [&]() { return cancelFlag && cancelFlag->load(); };

    // Force non-blocking so every call below can be retried on EAGAIN without
    // holding the session lock across a blocking wait; restore on exit.
    int origBlocking;
    {
        QMutexLocker locker(&lock);
        origBlocking = libssh2_session_get_blocking(session);
        libssh2_session_set_blocking(session, 0);
    }
    auto restoreBlocking = [&]() {
        QMutexLocker locker(&lock);
        if (client->rawSession())
            libssh2_session_set_blocking(client->rawSession(), origBlocking);
    };

    // --- open a dedicated exec channel (the shell channel stays untouched) ---
    LIBSSH2_CHANNEL *channel = nullptr;
    for (;;) {
        {
            QMutexLocker locker(&lock);
            channel = libssh2_channel_open_session(session);
            if (channel)
                break;
            if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
                recordError(QStringLiteral("Unable to open exec channel"));
                restoreBlocking();
                return result;
            }
        }
        if (timedOut() || isCancelled()) {
            result.timedOut = timedOut();
            result.cancelled = isCancelled();
            restoreBlocking();
            return result;
        }
        waitReadableCancellable(client, 50, cancelFlag);
    }

    auto finish = [&]() {
        // Close + free the channel; harvest the exit status when clean.
        int rc;
        for (;;) {
            {
                QMutexLocker locker(&lock);
                rc = libssh2_channel_close(channel);
            }
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            // 取消时不再继续等服务器应答：直接 free，本地释放不依赖对端确认。
            if (isCancelled())
                break;
            if (clock.elapsed() >= (timeoutMs > 0 ? timeoutMs + 5000 : 5000))
                break;
            waitReadableCancellable(client, 50, cancelFlag);
        }
        {
            QMutexLocker locker(&lock);
            if (rc == 0 && result.errorMessage.isEmpty() && !result.timedOut && !result.cancelled)
                result.exitCode = libssh2_channel_get_exit_status(channel);
            else if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN && !isCancelled()
                     && result.errorMessage.isEmpty())
                recordError(QStringLiteral("关闭命令通道失败"), rc);
            libssh2_channel_free(channel);
        }
        restoreBlocking();
    };

    // --- optional pty (对应Python: exec_command 的 get_pty=pty) ---
    if (pty) {
        int rc;
        for (;;) {
            {
                QMutexLocker locker(&lock);
                rc = libssh2_channel_request_pty(channel, "xterm");
            }
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            if (timedOut() || isCancelled())
                break;
            waitReadableCancellable(client, 50, cancelFlag);
        }
        if (rc != 0) {
            QMutexLocker locker(&lock);
            recordError(QStringLiteral("request_pty failed"), rc);
            locker.unlock();
            finish();
            return result;
        }
    }

    // --- start the command ---
    {
        const QByteArray cmdUtf8 = cmd.toUtf8();
        int rc;
        for (;;) {
            {
                QMutexLocker locker(&lock);
                rc = libssh2_channel_exec(channel, cmdUtf8.constData());
            }
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            if (timedOut() || isCancelled())
                break;
            waitReadableCancellable(client, 50, cancelFlag);
        }
        if (rc != 0) {
            QMutexLocker locker(&lock);
            recordError(QStringLiteral("exec failed"), rc);
            locker.unlock();
            finish();
            return result;
        }
    }

    // --- stdin (sudo password / script upload), then EOF ---
    // 对应Python: ssh_agent.py::_execute_single 的 stdin.write(password) + flush
    if (!stdinData.isEmpty()) {
        int writeError = 0;
        if (!channelWriteAll(client, channel, stdinData, cancelFlag, writeError)) {
            QMutexLocker locker(&lock);
            if (isCancelled())
                result.cancelled = true;
            else
                recordError(QStringLiteral("stdin write failed"), writeError);
            locker.unlock();
            finish();
            return result;
        }
        int rc;
        for (;;) {
            {
                QMutexLocker locker(&lock);
                rc = libssh2_channel_send_eof(channel);
            }
            if (rc != LIBSSH2_ERROR_EAGAIN)
                break;
            if (isCancelled())
                break;
            waitReadableCancellable(client, 50, cancelFlag);
        }
    }

    // --- read loop: stdout + stderr until EOF / timeout / cancel ---
    // 对应Python: ssh_agent.py::_stream_read_output（同时监控两个流，无数据时小睡）
    QByteArray outBuf;
    QByteArray errBuf;
    QByteArray pendingStdin; // callback-provided response (e.g. sudo password)
    char buf[4096];
    for (;;) {
        if (timedOut()) {
            result.timedOut = true;
            break;
        }
        if (isCancelled()) {
            result.cancelled = true;
            break;
        }

        bool got = false;
        bool eof = false;
        QByteArray outChunk;
        QByteArray errChunk;
        {
            QMutexLocker locker(&lock);
            ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
            if (n > 0) {
                outChunk = QByteArray(buf, int(n));
                got = true;
            } else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                recordError(QStringLiteral("读取命令输出失败"), int(n));
            }
            // 不让第二次读覆盖第一次错误；保留底层错误码供断线判定。
            if (result.errorMessage.isEmpty()) {
                ssize_t m = libssh2_channel_read_stderr(channel, buf, sizeof(buf));
                if (m > 0) {
                    errChunk = QByteArray(buf, int(m));
                    got = true;
                } else if (m < 0 && m != LIBSSH2_ERROR_EAGAIN) {
                    recordError(QStringLiteral("读取命令错误输出失败"), int(m));
                }
            }
            if (!got && libssh2_channel_eof(channel))
                eof = true;
        }

        if (!outChunk.isEmpty()) {
            outBuf += outChunk;
            if (onChunk)
                pendingStdin += onChunk(outChunk, false);
        }
        if (!errChunk.isEmpty()) {
            errBuf += errChunk;
            if (onChunk)
                pendingStdin += onChunk(errChunk, true);
        }
        if (!pendingStdin.isEmpty() && result.errorMessage.isEmpty()) {
            int writeError = 0;
            if (!channelWriteAll(client, channel, pendingStdin, cancelFlag, writeError)) {
                QMutexLocker locker(&lock);
                if (isCancelled()) result.cancelled = true;
                else recordError(QStringLiteral("stdin write failed"), writeError);
            }
            pendingStdin.clear();
        }
        if (eof || !result.errorMessage.isEmpty() || result.cancelled)
            break;
        if (!got) {
            // 切片等待 + 取消检查：stop() 置 cancelFlag 后下一轮（≤25ms）即返回，
            // 不会把监控线程钉满整个 30s 超时。
            waitReadableCancellable(client, 100, cancelFlag);
        }
    }

    result.stdoutText = QString::fromUtf8(outBuf);
    result.stderrText = QString::fromUtf8(errBuf);
    finish();
    return result;
}

// ==========================================================================
// Synchronous API
// ==========================================================================

// 对应Python: function/ssh_func.py::SshClient.exec
ExecResult CommandExecutor::exec(const QString &cmd, bool pty, int timeoutMs)
{
    m_cancelRequested.store(false);
    return runCommandInternal(m_client, cmd, pty, timeoutMs, QByteArray(),
                              &m_cancelRequested, nullptr);
}

// 对应Python: function/ssh_func.py::SshClient.sudo_exec / _sudo_exec
ExecResult CommandExecutor::sudoExec(const QString &cmd, bool pty, int timeoutMs)
{
    if (m_client && m_client->username() == QStringLiteral("root"))
        return exec(cmd, pty, timeoutMs);

    m_cancelRequested.store(false);
    const QString wrapped = QStringLiteral("sudo -S ") + cmd;
    QByteArray stdinData;
    if (m_client && !m_client->password().isEmpty())
        stdinData = (m_client->password() + QLatin1Char('\n')).toUtf8();
    return runCommandInternal(m_client, wrapped, pty, timeoutMs, stdinData,
                              &m_cancelRequested, nullptr);
}

// 对应Python: core/frp_manager.py::deploy_frps（上传 + exec 执行 + 清理）
ExecResult CommandExecutor::execScript(const QByteArray &scriptContent,
                                       const QStringList &args, int timeoutMs)
{
    m_cancelRequested.store(false);

    const QString remotePath = QStringLiteral("/tmp/cubeshell_script_%1_%2.sh")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QRandomGenerator::global()->generate());
    const QString quotedPath = shellQuote(remotePath);

    // Upload via `cat > file` on an exec channel (stdin + EOF terminates cat).
    ExecResult upload = runCommandInternal(
        m_client, QStringLiteral("cat > ") + quotedPath, false,
        kDefaultTimeoutMs, scriptContent, &m_cancelRequested, nullptr);
    if (!upload.ok() || upload.exitCode != 0) {
        if (upload.errorMessage.isEmpty())
            upload.errorMessage = QStringLiteral("script upload failed (exit %1)")
                                      .arg(upload.exitCode);
        return upload;
    }

    QStringList quotedArgs;
    for (const QString &a : args)
        quotedArgs << shellQuote(a);

    // chmod +x + execute + always clean up, propagating the script's rc.
    const QString runCmd = QStringLiteral("chmod +x %1 && %1%2%3; rc=$?; rm -f %1; exit $rc")
                               .arg(quotedPath,
                                    quotedArgs.isEmpty() ? QString() : QStringLiteral(" "),
                                    quotedArgs.join(QLatin1Char(' ')));
    return runCommandInternal(m_client, runCmd, false, timeoutMs,
                              QByteArray(), &m_cancelRequested, nullptr);
}

// ==========================================================================
// Streaming API
// ==========================================================================

// 对应Python: core/ai/ssh_agent.py::_CommandExecutorWorker.run + _stream_read_output
bool CommandExecutor::execStream(const QString &cmd, bool pty, int timeoutMs)
{
    if (m_streaming.load())
        return false;
    waitForFinished(); // reap a previously finished worker thread

    if (timeoutMs < 0)
        timeoutMs = isLongRunningCommand(cmd) ? kLongRunningTimeoutMs : kNormalStreamTimeoutMs;

    m_cancelRequested.store(false);
    m_streaming.store(true);
    m_streamThread = QThread::create([this, cmd, pty, timeoutMs]() {
        streamLoop(cmd, pty, timeoutMs);
    });
    m_streamThread->setObjectName(QStringLiteral("cubeshell-exec-stream"));
    m_streamThread->start();
    return true;
}

void CommandExecutor::streamLoop(const QString &cmd, bool pty, int timeoutMs)
{
    // sudo handling: rewrite to `sudo -S` and answer the password prompt.
    // 对应Python: ssh_agent.py::_execute_single 的 sudo -S 注入
    const QString username = m_client ? m_client->username() : QString();
    const bool needsSudo = needsSudoPassword(cmd, username);
    const QString actualCmd = needsSudo ? injectSudoStdinFlag(cmd) : cmd;

    QElapsedTimer emitTimer;
    emitTimer.start();
    qint64 lastEmitMs = -kEmitIntervalMs; // first chunk is emitted immediately
    QByteArray pending;
    bool passwordSent = false;

    // Throttled per-chunk emission (对应Python: _throttled_emit，防止 wget/curl
    // 的 \r 进度每秒几十次 emit 压垮 UI 事件队列)。\r 字节原样透传给消费方。
    ChunkCallback onChunk = [&](const QByteArray &chunk, bool isStderr) -> QByteArray {
        Q_UNUSED(isStderr);
        pending += chunk;
        if (emitTimer.elapsed() - lastEmitMs >= kEmitIntervalMs) {
            emit outputChunk(pending);
            pending.clear();
            lastEmitMs = emitTimer.elapsed();
        }
        if (needsSudo && !passwordSent
            && looksLikeSudoPasswordPrompt(QString::fromUtf8(chunk))) {
            passwordSent = true;
            return (m_client->password() + QLatin1Char('\n')).toUtf8();
        }
        return QByteArray();
    };

    const ExecResult r = runCommandInternal(m_client, actualCmd, pty, timeoutMs,
                                            QByteArray(), &m_cancelRequested, onChunk);
    if (!pending.isEmpty())
        emit outputChunk(pending);

    // 完成信号的接收方可能立即发起下一次刷新，先发布空闲状态。
    // execStream() 仍会 join 旧线程，不会让两个工作线程同时运行。
    m_streaming.store(false);
    if (!r.errorMessage.isEmpty())
        emit streamError(r.errorMessage);
    else if (r.timedOut)
        emit streamError(QStringLiteral("command timed out"));

    emit streamFinished(r.ok() ? r.exitCode : -1, r.stdoutText, r.stderrText);
}

} // namespace cubeshell
