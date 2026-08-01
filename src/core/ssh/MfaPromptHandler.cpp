// MfaPromptHandler.cpp — MFA multi-stage dialog state machine. See header.

#include "MfaPromptHandler.h"

#include <QLoggingCategory>
#include <QMutexLocker>

Q_DECLARE_LOGGING_CATEGORY(mfaLog)
Q_LOGGING_CATEGORY(mfaLog, "cubeshell.ssh.mfa")

namespace cubeshell {

MfaPromptHandler::MfaPromptHandler(QObject *parent)
    : QObject(parent)
{
}

MfaPromptHandler::State MfaPromptHandler::state() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

int MfaPromptHandler::attempt() const
{
    QMutexLocker locker(&m_mutex);
    return m_attempt;
}

void MfaPromptHandler::setMaxAttempts(int maxAttempts)
{
    QMutexLocker locker(&m_mutex);
    m_maxAttempts = qMax(1, maxAttempts);
}

void MfaPromptHandler::setSuccessGraceMs(int ms)
{
    QMutexLocker locker(&m_mutex);
    m_successGraceMs = qMax(0, ms);
}

void MfaPromptHandler::reset()
{
    QMutexLocker locker(&m_mutex);
    m_state = State::Idle;
    m_attempt = 0;
    m_lastPrompt.clear();
    m_sinceSubmit.invalidate();
}

// Compute the state transition under the lock; signals fire afterwards so a
// direct-connected slot can never re-enter the mutex (死锁防护).
MfaPromptHandler::Actions MfaPromptHandler::handlePromptLocked(const QString &prompt)
{
    Actions a;
    switch (m_state) {
    case State::Idle:
        // First prompt of a session. 对应Python: _on_shell_mfa_prompt 首次弹窗
        m_attempt = 1;
        m_state = State::WaitingForCode;
        m_lastPrompt = prompt;
        a.emitPrompt = true;
        a.prompt = prompt;
        a.attempt = m_attempt;
        break;
    case State::WaitingForCode:
        // Dialog already open — the server re-printed the prompt; ignore.
        // (watcher 冷却期通常已挡住这种重复。)
        break;
    case State::WaitingForResult:
        // A new prompt after we sent a code ⇒ the code was rejected. Retry
        // until the attempt budget runs out.
        if (m_attempt >= m_maxAttempts) {
            a.emitFailed = true;
            a.failedPrompt = prompt;
            m_state = State::Idle;
            m_attempt = 0;
            m_sinceSubmit.invalidate();
        } else {
            ++m_attempt;
            m_state = State::WaitingForCode;
            m_lastPrompt = prompt;
            a.emitPrompt = true;
            a.prompt = prompt;
            a.attempt = m_attempt;
        }
        break;
    }
    return a;
}

void MfaPromptHandler::applyActions(const Actions &a)
{
    if (a.emitPrompt) {
        qCDebug(mfaLog) << "MFA prompt (attempt" << a.attempt << "):" << a.prompt;
        emit mfaPromptRequired(a.prompt, a.attempt);
    }
    if (a.emitSucceeded) {
        qCDebug(mfaLog) << "MFA accepted";
        emit mfaSucceeded();
    }
    if (a.emitFailed) {
        qCWarning(mfaLog) << "MFA failed after max attempts:" << a.failedPrompt;
        emit mfaFailed(a.failedPrompt);
    }
}

// 对应Python: core/paramiko_bridge.py::_read_loop 的 watcher.feed + emit
void MfaPromptHandler::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    // The watcher owns ANSI stripping, accumulation and the cooldown window.
    const QString prompt = m_watcher.feed(data);

    Actions a;
    {
        QMutexLocker locker(&m_mutex);
        if (!prompt.isEmpty()) {
            a = handlePromptLocked(prompt);
        } else if (m_state == State::WaitingForResult
                   && m_sinceSubmit.isValid()
                   && m_sinceSubmit.elapsed() >= m_successGraceMs
                   && !QString::fromUtf8(data).trimmed().isEmpty()) {
            // Normal output kept flowing after the submit ⇒ code accepted.
            a.emitSucceeded = true;
            m_state = State::Idle;
            m_attempt = 0;
            m_sinceSubmit.invalidate();
        }
    }
    applyActions(a);
}

void MfaPromptHandler::onPromptDetected(const QString &prompt)
{
    if (prompt.isEmpty())
        return;
    Actions a;
    {
        QMutexLocker locker(&m_mutex);
        a = handlePromptLocked(prompt);
    }
    applyActions(a);
}

// 对应Python: cube-shell.py::_on_shell_mfa_prompt 中 bridge.send_input(code)
void MfaPromptHandler::submitCode(const QString &code)
{
    bool send = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_state == State::WaitingForCode && !code.isEmpty()) {
            m_state = State::WaitingForResult;
            m_sinceSubmit.start();
            send = true;
        }
    }
    if (send)
        emit codeReady(code);
}

// 对应Python: 取消对话框不影响会话（用户可直接在终端键入验证码）
void MfaPromptHandler::cancelPrompt()
{
    QMutexLocker locker(&m_mutex);
    if (m_state == State::WaitingForCode) {
        m_state = State::Idle;
        m_sinceSubmit.invalidate();
    }
}

} // namespace cubeshell
