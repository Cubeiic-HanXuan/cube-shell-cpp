// ShellMfaWatcher.cpp — C++ port of core/shell_mfa_watcher.py. See header.

#include "ShellMfaWatcher.h"

namespace cubeshell {

// 对应C++: _ANSI_RE
static const QRegularExpression &ansiRe()
{
    static const QRegularExpression re(
        QStringLiteral("\x1b\\[[0-9;]*[a-zA-Z]|\x1b\\][^\x07]*\x07|\x1b\\][^\x1b]*\x1b\\\\|\x1b\\[\\?[0-9]+[hl]"));
    return re;
}

// 对应C++: _PROMPT_PATTERNS (joined with '|')
static QString promptAlternation()
{
    // NOTE: build the list as QString directly — routing the Chinese pattern
    // through toUtf8().constData() into a static const char* array leaves a
    // dangling pointer (the temporary QByteArray dies immediately), which
    // corrupts the alternation and makes the regex match arbitrary output.
    static const QStringList patterns = {
        QStringLiteral(R"(verification\s+code(?:\s*\([^)]+\))?\s*[:?])"),
        QStringLiteral(R"(verify\s+code\s*[:?])"),
        QStringLiteral(R"(auth(?:entication|enticator)?\s+code\s*[:?])"),
        QStringLiteral(R"(enter\s+(?:your\s+)?(?:verification|auth(?:entication|enticator)?|otp|totp|6[-\s]?digit|one[-\s]?time)[^:]{0,40}\s*[:?])"),
        QStringLiteral(R"(otp\s*[:?])"),
        QStringLiteral(R"(totp\s*[:?])"),
        QStringLiteral(R"(one[-\s]?time\s+(?:password|code|token)\s*[:?])"),
        QStringLiteral(R"(6[-\s]?digit\s+code\s*[:?])"),
        QStringLiteral(R"(google\s+auth(?:enticator)?[^:]{0,30}\s*[:?])"),
        // Chinese prompts (动态口令/验证码/...).
        QStringLiteral("(?:请(?:输入)?\\s*)?(?:动态口令|动态密码|二次验证|两步验证|身份验证器|验证码|令牌)\\s*[:?：]"),
        QStringLiteral(R"(\[?mfa[^:：\r\n]{0,30}[:?：])"),
        QStringLiteral(R"(passcode\s*[:?])"),
        QStringLiteral(R"(challenge\s*[:?])"),
        QStringLiteral(R"(\btoken\s*[:?])"),
        QStringLiteral(R"(\bcode\s*[:?]\s*$)"),
    };
    return patterns.join(QLatin1Char('|'));
}

// Anchored prompt matcher (prefix-anchored).
// 对应C++: _PROMPT_RE
static const QRegularExpression &promptRe()
{
    static const QRegularExpression re(
        QStringLiteral("(?:^|\r\n|\n|\x1b\\[\\?25h|\x1b\\[K|\x1b\\[0m|\\$\\s|>\\s|#\\s)(?:")
        + promptAlternation() + QStringLiteral(")"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Line-level matcher (no anchor), for extracting the prompt line.
// 对应C++: _PROMPT_LINE_RE
static const QRegularExpression &promptLineRe()
{
    static const QRegularExpression re(
        QStringLiteral("(?:") + promptAlternation() + QStringLiteral(")"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QByteArray stripAnsi(const QByteArray &data)
{
    QString out = QString::fromUtf8(data);
    out.remove(ansiRe());
    return out.toUtf8();
}

bool looksLikeMfaPrompt(const QByteArray &buffer)
{
    if (buffer.isEmpty())
        return false;
    const QByteArray text = stripAnsi(buffer);
    return promptRe().match(QString::fromUtf8(text)).hasMatch();
}

ShellMfaWatcher::ShellMfaWatcher() = default;

QString ShellMfaWatcher::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return QString();

    m_buffer += data;
    if (m_buffer.size() > BUFFER_SIZE)
        m_buffer = m_buffer.right(BUFFER_SIZE);

    if (m_timerStarted && m_triggerTimer.elapsed() < COOLDOWN_MS)
        return QString();

    if (!looksLikeMfaPrompt(m_buffer))
        return QString();

    const QString promptText = extractPromptLine(m_buffer);
    m_buffer.clear();
    m_triggerTimer.start();
    m_timerStarted = true;
    return promptText;
}

QString ShellMfaWatcher::extractPromptLine(const QByteArray &buffer)
{
    const QString text = QString::fromUtf8(stripAnsi(buffer));
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

    // Newest prompt usually wins — scan from the bottom up.
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty() && promptLineRe().match(line).hasMatch())
            return line;
    }
    // Fallback: last non-empty line.
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty())
            return line;
    }
    return text.trimmed();
}

} // namespace cubeshell
