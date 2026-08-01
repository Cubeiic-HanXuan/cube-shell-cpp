#pragma once

// KPtyProcess.h — C++ port of qtermwidget/kptyprocess.py
//
// Extends KProcess with support for running a child process inside a
// pseudo-terminal (PTY). Qt-only replacement for KDE's KPtyProcess.
//
// Original:
// Copyright (C) 2007 Oswald Buddenhagen <ossi@kde.org>

#include "KProcess.h"
#include "KPtyDevice.h"

#include <functional>

namespace Konsole {

// 对应C++: enum PtyChannelFlag
// Which stdio channels of the child are connected to the pty.
enum PtyChannelFlag {
    NoChannels = 0,        // don't connect the pty to any channel
    StdinChannel = 1,      // connect pty to stdin
    StdoutChannel = 2,     // connect pty to stdout
    StderrChannel = 4,     // connect pty to stderr
    AllOutputChannels = 6, // connect pty to all output channels
    AllChannels = 7        // connect pty to all channels
};
Q_DECLARE_FLAGS(PtyChannels, PtyChannelFlag)

class KPtyProcessPrivate;

// 对应C++: class KPtyProcess : public KProcess
class KPtyProcess : public KProcess {
    Q_OBJECT
public:
    // 对应C++: explicit KPtyProcess(QObject *parent = nullptr)
    explicit KPtyProcess(QObject *parent = nullptr);
    // 对应C++: KPtyProcess(int ptyMasterFd, QObject *parent = nullptr)
    explicit KPtyProcess(int ptyMasterFd, QObject *parent = nullptr);
    ~KPtyProcess() override;

    // 对应C++: void setPtyChannels(PtyChannels channels)
    void setPtyChannels(PtyChannels channels);
    // 对应C++: PtyChannels ptyChannels() const
    PtyChannels ptyChannels() const;

    // 对应C++: KPtyDevice *pty() const
    KPtyDevice *pty() const;

    // Start the configured program inside the pty.
    // 对应C++: void start()
    void start();

    // 对应C++: bool waitForStarted(int msecs = 30000)
    bool waitForStarted(int msecs = 30000);

    // 对应C++: void setUseUtmp(bool value)
    void setUseUtmp(bool value);
    // 对应C++: bool isUseUtmp() const
    bool isUseUtmp() const;

    // Set the child process modifier (chainable; used by Pty).
    // 对应C++: setChildProcessModifier(...)
    void setChildProcessModifier(const std::function<void()> &modifier);
    std::function<void()> childProcessModifier() const;

    // 对应C++: void setWriteable(bool writeable)
    void setWriteable(bool writeable);

protected:
    // Performs the setCTty + dup2(stdio) work in the child process. Invoked
    // via QProcess::setChildProcessModifier. (Not the Qt6 QProcess virtual,
    // which is intentionally un-overridable; hence the distinct name.)
    // 对应C++: setChildProcessModifier([d]() { ... })
    void setupChildProcessImpl();

private:
    void init(int ptyMasterFd);

    KPtyDevice *m_pty;
    PtyChannels m_ptyChannels;
    bool m_addUtmp;
    std::function<void()> m_childProcessModifier;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(PtyChannels)

} // namespace Konsole
