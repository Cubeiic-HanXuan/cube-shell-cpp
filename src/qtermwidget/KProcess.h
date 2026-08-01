#pragma once

// KProcess.h — C++ port of qtermwidget/kprocess.py
//
// Child-process invocation, monitoring and control. A thin convenience layer
// over QProcess (the KDE KProcess, reimplemented here so we do not depend on
// KDE frameworks). Ported from the Python version.
//
// Copyright (C) 2007 Oswald Buddenhagen <ossi@kde.org>

#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

namespace Konsole {

// 对应C++: enum OutputChannelMode (mirrors QProcess::ProcessChannelMode)
enum OutputChannelMode {
    SeparateChannels   = QProcess::SeparateChannels,
    MergedChannels     = QProcess::MergedChannels,
    ForwardedChannels  = QProcess::ForwardedChannels,
    OnlyStdoutChannel  = QProcess::ForwardedErrorChannel,
    OnlyStderrChannel  = QProcess::ForwardedOutputChannel
};

class KProcess : public QProcess {
    Q_OBJECT
public:
    explicit KProcess(QObject *parent = nullptr)
        : QProcess(parent)
    {
        QProcess::setProcessChannelMode(QProcess::ForwardedChannels);
    }

    // Append an argument (or set the executable if not yet set).
    // 对应C++: KProcess &operator<<(const QString& arg) / (const QStringList&)
    KProcess &operator<<(const QString &arg)
    {
        if (m_prog.isEmpty())
            m_prog = arg;
        else
            m_args << arg;
        return *this;
    }
    KProcess &operator<<(const QStringList &args)
    {
        if (m_prog.isEmpty())
            setProgram(args);
        else
            m_args << args;
        return *this;
    }

    void setOutputChannelMode(OutputChannelMode mode)
    {
        QProcess::setProcessChannelMode(QProcess::ProcessChannelMode(mode));
    }
    OutputChannelMode outputChannelMode() const
    {
        return OutputChannelMode(QProcess::processChannelMode());
    }

    void setNextOpenMode(QIODevice::OpenMode mode) { m_openMode = mode; }

    void setEnv(const QString &name, const QString &value, bool overwrite = true);
    void unsetEnv(const QString &name);
    void clearEnvironment();

    void setProgram(const QString &exe, const QStringList &args = QStringList());
    void setProgram(const QStringList &argv);
    void clearProgram();

    // 对应C++: void setShellCommand(const QString &cmd)
    void setShellCommand(const QString &cmd);

    QStringList program() const
    {
        QStringList ret = m_args;
        ret.prepend(m_prog);
        return ret;
    }

    // 对应C++: void start()
    void start() { QProcess::start(m_prog, m_args, m_openMode); }

    // 对应C++: int execute(int msecs)
    int execute(int msecs = -1);

    static int execute(const QString &exe, const QStringList &args = QStringList(), int msecs = -1);
    static int execute(const QStringList &argv, int msecs = -1);

    int startDetached();
    static int startDetached(const QString &exe, const QStringList &args = QStringList());
    static int startDetached(const QStringList &argv);

private:
    using QProcess::setProcessChannelMode; // hidden — use setOutputChannelMode
    using QProcess::processChannelMode;

    QIODevice::OpenMode m_openMode = QIODevice::ReadWrite;
    QString m_prog;
    QStringList m_args;
};

} // namespace Konsole
