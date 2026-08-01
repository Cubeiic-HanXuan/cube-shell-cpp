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
static void fillExecError(LIBSSH2_SESSION *session, ExecResult &result, const QString &context)
{
    char *msg = nullptr;
    int len = 0;
    const int code = session ? libssh2_session_last_error(session, &msg, &len, 0) : 0;
    result.errorMessage = context;
    if (msg && len > 0)
        result.errorMessage += QStringLiteral(": ") + QString::fromLatin1(msg, len);
    result.errorMessage += QStringLiteral(" (code %1)").arg(code);
}

// Write all bytes to the channel's stdin, dropping the session lock while
// waiting on EAGAIN so the interactive shell channel is never starved.
static bool channelWriteAll(SshClient *client, LIBSSH2_CHANNEL *channel,
                            const QByteArray &data)
{
    QRecursiveMutex &lock = client->sessionLock();
    qsizetype written = 0;
    while (written < data.size()) {
        ssize_t n;
        {
            QMutexLocker locker(&lock);
            n = libssh2_channel_write(channel, data.constData() + written,
                                      size_t(data.size() - written));
        }
        if (n == LIBSSH2_ERROR_EAGAIN) {
            client->waitReadable(50);
            continue;
        }
        if (n < 0)
            return false;
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
    if (!client || !client->isConnected()) {
        result.errorMessage = QStringLiteral("Not connected");
        return result;
    }
    LIBSSH2_SESSION *session = client->rawSession();
    QRecursiveMutex &lock = client->sessionLock();

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
                fillExecError(session, result, QStringLiteral("Unable to open exec channel"));
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
        client->waitReadable(50);
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
            if (clock.elapsed() >= (timeoutMs > 0 ? timeoutMs + 5000 : 5000))
                break;
            client->waitReadable(50);
        }
        {
            QMutexLocker locker(&lock);
            if (rc == 0 && result.errorMessage.isEmpty() && !result.timedOut && !result.cancelled)
                result.exitCode = libssh2_channel_get_exit_status(channel);
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
            client->waitReadable(50);
        }
        if (rc != 0) {
            QMutexLocker locker(&lock);
            fillExecError(session, result, QStringLiteral("request_pty failed"));
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
            client->waitReadable(50);
        }
        if (rc != 0) {
            QMutexLocker locker(&lock);
            fillExecError(session, result, QStringLiteral("exec failed"));
            locker.unlock();
            finish();
            return result;
        }
    }

    // --- stdin (sudo password / script upload), then EOF ---
    // 对应Python: ssh_agent.py::_execute_single 的 stdin.write(password) + flush
    if (!stdinData.isEmpty()) {
        if (!channelWriteAll(client, channel, stdinData)) {
            QMutexLocker locker(&lock);
            fillExecError(session, result, QStringLiteral("stdin write failed"));
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
            client->waitReadable(50);
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
            }
            ssize_t m = libssh2_channel_read_stderr(channel, buf, sizeof(buf));
            if (m > 0) {
                errChunk = QByteArray(buf, int(m));
                got = true;
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
        if (!pendingStdin.isEmpty()) {
            channelWriteAll(client, channel, pendingStdin);
            pendingStdin.clear();
        }
        if (eof)
            break;
        if (!got)
            client->waitReadable(100); // 对应Python: time.sleep(0.1)
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

    if (!r.errorMessage.isEmpty())
        emit streamError(r.errorMessage);
    else if (r.timedOut)
        emit streamError(QStringLiteral("command timed out"));

    emit streamFinished(r.ok() ? r.exitCode : -1, r.stdoutText, r.stderrText);
    m_streaming.store(false);
}

} // namespace cubeshell
