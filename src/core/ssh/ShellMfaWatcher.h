#pragma once

// ShellMfaWatcher.h — C++ port of core/shell_mfa_watcher.py
//
// Detects MFA/OTP prompts in the shell output stream (e.g. a target server's
// PAM google_authenticator challenge after SSH auth). Non-blocking: feed() only
// detects and returns the prompt text; the caller forwards it to the UI thread
// via a Qt signal so the read loop never blocks on UI.

#include <QByteArray>
#include <QRegularExpression>
#include <QString>

#include <QElapsedTimer>

namespace cubeshell {

// Strip ANSI escape sequences from terminal output.
QByteArray stripAnsi(const QByteArray &data);

// True if the buffer (after ANSI stripping) looks like an MFA/OTP prompt.
bool looksLikeMfaPrompt(const QByteArray &buffer);

// 对应C++: class ShellMfaWatcher
class ShellMfaWatcher {
public:
    ShellMfaWatcher();

    // Feed a chunk of terminal output. Returns the detected prompt line, or an
    // empty string if none. Non-blocking; safe to call from the read loop.
    QString feed(const QByteArray &data);

private:
    static QString extractPromptLine(const QByteArray &buffer);

    QByteArray m_buffer;
    QElapsedTimer m_triggerTimer;
    bool m_timerStarted = false;

    static constexpr qint64 COOLDOWN_MS = 5000;
    static constexpr int BUFFER_SIZE = 4096;
};

} // namespace cubeshell
