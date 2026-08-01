// ShellCommand.cpp — C++ port of qtermwidget/shell_command.py
//
// Original copyright:
//   Copyright (C) 2007 by Robert Knight <robertknight@gmail.com>
//   Rewritten for QT4 by e_k <e_k at users.sourceforge.net>, Copyright (C)2008

#include "ShellCommand.h"

#include <QFileInfo>
#include <QStandardPaths>

namespace Konsole {

// 对应C++: ShellCommand::ShellCommand(const QString & fullCommand)
ShellCommand::ShellCommand(const QString &fullCommand)
{
    bool inQuotes = false;
    QString builder;

    const qsizetype count = fullCommand.size();
    for (qsizetype i = 0; i < count; ++i) {
        const QChar ch = fullCommand.at(i);
        const bool isLastChar = (i == count - 1);
        const bool isQuote = (ch == QLatin1Char('\'') || ch == QLatin1Char('"'));

        if (!isLastChar && isQuote) {
            inQuotes = !inQuotes;
        } else {
            if ((!ch.isSpace() || inQuotes) && !isQuote)
                builder.append(ch);

            if ((ch.isSpace() && !inQuotes) || isLastChar) {
                // Upstream behaviour: always append builder, even if empty.
                _arguments.append(builder);
                builder.clear();
            }
        }
    }
}

// 对应C++: ShellCommand::ShellCommand(const QString & command, const QStringList & arguments)
//     : _arguments(arguments)
// {
//     if ( !_arguments.isEmpty() )
//         _arguments[0] = command;
// }
ShellCommand::ShellCommand(const QString &command, const QStringList &arguments)
    : _arguments(arguments)
{
    if (!_arguments.isEmpty())
        _arguments[0] = command;
}

// 对应C++: QString ShellCommand::fullCommand() const
QString ShellCommand::fullCommand() const
{
    return _arguments.join(QLatin1Char(' '));
}

// 对应C++: QString ShellCommand::command() const
QString ShellCommand::command() const
{
    if (!_arguments.isEmpty())
        return _arguments.first();
    return QString();
}

// 对应C++: QStringList ShellCommand::arguments() const
QStringList ShellCommand::arguments() const
{
    return _arguments;
}

// 对应C++: bool ShellCommand::isRootCommand() const
//
// Upstream C++ leaves this unimplemented (Q_ASSERT(0)); the Python version
// provided a complete implementation which is preserved here.
bool ShellCommand::isRootCommand() const
{
    if (_arguments.isEmpty())
        return false;

    const QString cmd = command().toLower();

    // Privilege-escalation commands (sudo, su and modern equivalents).
    static const QStringList rootCommands = {
        QStringLiteral("sudo"), QStringLiteral("su"),
        QStringLiteral("pkexec"), QStringLiteral("gksu"),
        QStringLiteral("kdesu"), QStringLiteral("kdesudo"),
        QStringLiteral("doas"), QStringLiteral("run0")
    };

    for (const QString &rootCmd : rootCommands) {
        if (cmd.endsWith(rootCmd) || cmd == rootCmd)
            return true;
    }

    return false;
}

// 对应C++: bool ShellCommand::isAvailable() const
//
// Upstream C++ leaves this unimplemented (Q_ASSERT(0)); the Python version
// provided a complete implementation which is preserved here.
bool ShellCommand::isAvailable() const
{
    if (_arguments.isEmpty())
        return false;

    const QString cmd = command();
    if (cmd.isEmpty())
        return false;

    const QFileInfo info(cmd);
    // Absolute (or relative-to-cwd) path: check it directly.
    if (info.isAbsolute() || cmd.contains(QLatin1Char('/')))
        return info.exists() && info.isFile() && info.isExecutable();

    // Otherwise search the PATH.
    return !QStandardPaths::findExecutable(cmd).isEmpty();
}

// 对应C++: QString ShellCommand::expand(const QString & text)
QString ShellCommand::expand(const QString &text)
{
    return expandEnv(text);
}

// 对应C++: QStringList ShellCommand::expand(const QStringList & items)
QStringList ShellCommand::expand(const QStringList &items)
{
    QStringList result;
    result.reserve(items.size());
    for (const QString &item : items)
        result.append(expandEnv(item));
    return result;
}

// 对应C++: static bool expandEnv(QString & text)
//
// Expands environment variables in @p text. Escaped '$' characters are
// ignored. The C++ upstream returned a bool indicating whether anything was
// expanded; the Python version returned the expanded text directly, which is
// what we keep here.
QString ShellCommand::expandEnv(const QString &text)
{
    if (text.isEmpty())
        return text;

    QString result = text;
    qsizetype pos = 0;

    while (true) {
        // Find the next '$'.
        // 对应C++: pos = text.indexOf(QLatin1Char('$'), pos)
        pos = result.indexOf(QLatin1Char('$'), pos);
        if (pos == -1)
            break;

        // Skip escaped '$'.
        // 对应C++: if ( pos > 0 && text.at(pos-1) == QLatin1Char('\\') )
        if (pos > 0 && result.at(pos - 1) == QLatin1Char('\\')) {
            ++pos;
            continue;
        }

        // Find the end of the variable: the nearest of ' ' or '/'.
        // 对应C++: pos2 = text.indexOf( QLatin1Char(' '), pos+1 );
        const qsizetype pos2Space = result.indexOf(QLatin1Char(' '), pos + 1);
        const qsizetype pos2Slash = result.indexOf(QLatin1Char('/'), pos + 1);

        qsizetype pos2;
        if (pos2Space == -1 && pos2Slash == -1)
            pos2 = result.size();
        else if (pos2Space == -1)
            pos2 = pos2Slash;
        else if (pos2Slash == -1)
            pos2 = pos2Space;
        else
            pos2 = qMin(pos2Space, pos2Slash);

        const qsizetype varLen = pos2 - pos;
        // 对应C++: QString key = text.mid( pos+1, len-1);
        const QString key = result.mid(pos + 1, varLen - 1);

        // 对应C++: QString::fromLocal8Bit( qgetenv(key.toLocal8Bit().constData()) )
        const QByteArray value = qgetenv(key.toLocal8Bit().constData());

        if (!value.isEmpty()) {
            // 对应C++: text.replace( pos, len, value );
            const QString valueStr = QString::fromLocal8Bit(value);
            result.replace(pos, varLen, valueStr);
            // 对应C++: pos = pos + value.length();
            pos = pos + valueStr.size();
        } else {
            // Variable not set: skip past it without replacing.
            pos = pos2;
        }
    }

    return result;
}

} // namespace Konsole
