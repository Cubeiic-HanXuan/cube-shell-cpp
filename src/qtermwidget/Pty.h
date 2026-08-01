#pragma once

// Pty.h — C++ port of qtermwidget/pty.py
//
// Starts a terminal process, sends/receives data to/from it, and manipulates
// properties of the pseudo-terminal used to communicate with the process.
//
// Original:
// Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
// Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include "KPtyProcess.h"

#include <QSize>
#include <QStringList>

namespace Konsole {

// 对应C++: class Pty: public KPtyProcess
class Pty : public KPtyProcess {
    Q_OBJECT
public:
    // 对应C++: explicit Pty(QObject* parent = nullptr)
    explicit Pty(QObject *parent = nullptr);
    // 对应C++: explicit Pty(int ptyMasterFd, QObject* parent = nullptr)
    explicit Pty(int ptyMasterFd, QObject *parent = nullptr);
    ~Pty() override;

    // Start the terminal process.
    // Returns 0 on success, non-zero otherwise.
    // 对应C++: int start(const QString& program, const QStringList& programArguments,
    //                    const QStringList& environment, int winid, bool addToUtmp)
    int start(const QString &program, const QStringList &programArguments, const QStringList &environment, int winid, bool addToUtmp);

    // 对应C++: void setWriteable(bool writeable)
    void setWriteable(bool writeable);

    // Enable/disable Xon/Xoff flow control.
    // 对应C++: void setFlowControlEnabled(bool enable)
    void setFlowControlEnabled(bool enable);
    // 对应C++: bool flowControlEnabled() const
    bool flowControlEnabled() const;

    // 对应C++: void setUtf8Mode(bool enable)
    void setUtf8Mode(bool enable);

    // 对应C++: void setErase(char erase)
    void setErase(char erase);
    // 对应C++: char erase() const
    char erase() const;

    // 对应C++: void setEmptyPTYProperties()
    void setEmptyPTYProperties();

    // Add "KEY=VALUE" pairs to the child environment; ensures TERM is set.
    // 对应C++: void addEnvironmentVariables(const QStringList& environment)
    void addEnvironmentVariables(const QStringList &environment);

    // Set the window size in characters. This is the hook the SSH bridge
    // intercepts.
    // 对应C++: void setWindowSize(int lines, int cols)
    void setWindowSize(int lines, int cols);
    // 对应C++: QSize windowSize() const
    QSize windowSize() const;

    // 对应C++: void setUseUtmp(bool value) (inherited)
    // 对应C++: int foregroundProcessGroup() const
    int foregroundProcessGroup() const;

    // Close the underlying pty master/slave pair.
    // 对应C++: void closePty()
    void closePty();

public Q_SLOTS:
    // 对应C++: void lockPty(bool lock)
    void lockPty(bool lock);
    // 对应C++: void sendData(const char* data, int length)
    void sendData(const char *data, int length);

Q_SIGNALS:
    // 对应C++: void receivedData(const char* buffer, int length)
    void receivedData(const char *buffer, int length);

    // Windows(ConPTY) 下进程退出通知;不复用 QProcess::finished,
    // 因为 QProcess::start() 从未被调用,其 state()/exitStatus() 无效。
    void processExited(int exitCode, QProcess::ExitStatus exitStatus);

private Q_SLOTS:
    // 对应C++: void dataReceived()
    void dataReceived();

private:
    void init();
    void applyTerminalSettings();

    int m_windowColumns = 0;
    int m_windowLines = 0;
    char m_eraseChar = 0;
    bool m_xonXoff = true;
    bool m_utf8 = true;
};

} // namespace Konsole
