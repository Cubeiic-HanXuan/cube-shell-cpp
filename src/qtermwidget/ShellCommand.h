#pragma once

// ShellCommand.h — C++ port of qtermwidget/shell_command.py
//
// Parses and extracts shell command information. Ported from the Python
// PySide6 version, which was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright (C) 2007 by Robert Knight <robertknight@gmail.com>
//   Rewritten for QT4 by e_k <e_k at users.sourceforge.net>, Copyright (C)2008

#include <QString>
#include <QStringList>

namespace Konsole {

/**
 * A class to parse and extract information about shell commands.
 *
 * ShellCommand can be used to:
 *
 * - Take a command-line (e.g. "/bin/sh -c /path/to/my/script") and split it
 *   into its component parts (e.g. the command "/bin/sh" and the arguments
 *   "-c", "/path/to/my/script")
 * - Take a command and a list of arguments and combine them to form a
 *   complete command-line
 * - Determine whether the binary specified by a command exists in the user's
 *   PATH
 * - Determine whether a command-line specifies the execution of another
 *   command as the root user using su/sudo etc.
 *
 * 对应C++: class ShellCommand
 */
class ShellCommand {
public:
    /**
     * Constructs a ShellCommand from a command line.
     *
     * @param fullCommand The command line to parse.
     *
     * 对应C++: ShellCommand(const QString & fullCommand)
     */
    explicit ShellCommand(const QString &fullCommand);

    /**
     * Constructs a ShellCommand with the specified @p command and @p arguments.
     *
     * 对应C++: ShellCommand(const QString & command, const QStringList & arguments)
     */
    ShellCommand(const QString &command, const QStringList &arguments);

    /** Returns the full command line. */
    // 对应C++: QString fullCommand() const
    QString fullCommand() const;

    /** Returns the command. */
    // 对应C++: QString command() const
    QString command() const;

    /** Returns the arguments for the command. */
    // 对应C++: QStringList arguments() const
    QStringList arguments() const;

    /**
     * Returns true if this is a root command.
     *
     * Note: in upstream C++ this method is unimplemented (Q_ASSERT(0)); the
     * Python version provided a complete implementation which is preserved here.
     *
     * 对应C++: bool isRootCommand() const
     */
    bool isRootCommand() const;

    /**
     * Returns true if the program specified by @p command() exists.
     *
     * Note: in upstream C++ this method is unimplemented (Q_ASSERT(0)); the
     * Python version provided a complete implementation which is preserved here.
     *
     * 对应C++: bool isAvailable() const
     */
    bool isAvailable() const;

    /**
     * Expands environment variables in @p text.
     *
     * 对应C++: static QString expand(const QString & text)
     */
    static QString expand(const QString &text);

    /**
     * Expands environment variables in each item of @p items.
     *
     * 对应C++: static QStringList expand(const QStringList & items)
     */
    static QStringList expand(const QStringList &items);

    // Value semantics: equality compares the parsed argument list.
    friend bool operator==(const ShellCommand &a, const ShellCommand &b)
    {
        return a._arguments == b._arguments;
    }
    friend bool operator!=(const ShellCommand &a, const ShellCommand &b)
    {
        return !(a == b);
    }

private:
    // 对应C++: static bool expandEnv(QString & text)
    // Expands environment variables in @p text. Escaped '$' characters are
    // ignored. Returns the expanded text.
    static QString expandEnv(const QString &text);

    QStringList _arguments;
};

} // namespace Konsole
