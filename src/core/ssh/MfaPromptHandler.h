#pragma once

// MfaPromptHandler.h — multi-stage MFA/OTP dialog state machine.
//
// C++ port of the shell-stream MFA flow spread across the Python side:
//   - prompt detection      对应Python: core/shell_mfa_watcher.py::ShellMfaWatcher
//     (reuses the already-ported cubeshell::ShellMfaWatcher — no re-implementation)
//   - prompt → dialog → code 回填 对应Python: cube-shell.py::_on_shell_mfa_prompt
//   - auth-phase prompt loop 对应Python: cube-shell.py::_get_mfa_code_from_user
//
// State machine: Idle → WaitingForCode (prompt detected, mfaPromptRequired
// emitted) → WaitingForResult (code submitted, codeReady emitted) → either
//   * a new prompt arrives   → attempt++ and back to WaitingForCode (retry),
//     until maxAttempts is exhausted → mfaFailed → Idle;
//   * normal output arrives after a grace period → mfaSucceeded → Idle.
//
// Threading: feed()/onPromptDetected() are called from the SSH read-loop
// thread; submitCode()/cancelPrompt() from the UI thread. Internal state is
// mutex-guarded and signals are emitted outside the lock. The UI must connect
// mfaPromptRequired with Qt::QueuedConnection (信号产生于读线程)，codeReady 同理
// （接收方通常是 SshBridge::sendInput）。

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>

#include "ShellMfaWatcher.h"

namespace cubeshell {

class MfaPromptHandler : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,             // no MFA interaction in progress
        WaitingForCode,   // prompt shown to the user, waiting for submitCode()
        WaitingForResult, // code sent, watching the stream for accept/reject
    };
    Q_ENUM(State)

    // 对应Python: ssh_func.py::SshClient.max_reconnect_attempts 同款上限语义
    static constexpr int kDefaultMaxAttempts = 3;
    // Output arriving this long after a submit with no new prompt ⇒ success.
    static constexpr int kDefaultSuccessGraceMs = 1500;

    explicit MfaPromptHandler(QObject *parent = nullptr);

    State state() const;
    int attempt() const;

    void setMaxAttempts(int maxAttempts);
    void setSuccessGraceMs(int ms); // test hook / tuning

    // Feed raw terminal output from the read-loop thread. Runs the internal
    // ShellMfaWatcher (ANSI stripping + cooldown) and drives the state machine.
    // 对应Python: core/paramiko_bridge.py::_read_loop 里的 watcher.feed(data)
    void feed(const QByteArray &data);

    // Reset to Idle (e.g. when the channel closes).
    void reset();

public slots:
    // Direct prompt entry point — connect SshBridge::shellMfaPromptDetected
    // here (Qt::QueuedConnection) when the bridge already runs its own watcher.
    // 对应Python: cube-shell.py 里 bridge.shellMfaPromptDetected → _on_shell_mfa_prompt
    void onPromptDetected(const QString &prompt);

    // UI thread: the user entered a code in the dialog.
    // 对应Python: cube-shell.py::_on_shell_mfa_prompt 的 bridge.send_input(code)
    void submitCode(const QString &code);

    // UI thread: the user dismissed the dialog. The session is unaffected —
    // the user may type the code directly in the terminal (watcher cooldown
    // prevents an immediate re-prompt). 对应Python: 取消对话框不影响会话
    void cancelPrompt();

signals:
    // Ask the UI to show the code dialog. Emitted from the read-loop thread —
    // connect with Qt::QueuedConnection. attempt starts at 1.
    void mfaPromptRequired(const QString &prompt, int attempt);

    // The code to write back to the channel; connect to SshBridge::sendInput
    // (bridge appends the carriage return itself).
    void codeReady(const QString &code);

    // Heuristic success: normal output kept flowing after the last submit.
    void mfaSucceeded();

    // maxAttempts prompts were rejected in a row.
    void mfaFailed(const QString &lastPrompt);

private:
    // Returns the signal-emission plan computed under the lock.
    struct Actions {
        bool emitPrompt = false;
        QString prompt;
        int attempt = 0;
        bool emitSucceeded = false;
        bool emitFailed = false;
        QString failedPrompt;
    };
    Actions handlePromptLocked(const QString &prompt);
    void applyActions(const Actions &a);

    mutable QMutex m_mutex;
    ShellMfaWatcher m_watcher;
    State m_state = State::Idle;
    int m_attempt = 0;
    int m_maxAttempts = kDefaultMaxAttempts;
    int m_successGraceMs = kDefaultSuccessGraceMs;
    QString m_lastPrompt;
    QElapsedTimer m_sinceSubmit;
};

} // namespace cubeshell
