// KPtyProcess.cpp — C++ port of qtermwidget/kptyprocess.py (POSIX only).
// Windows ConPTY path is handled by the Windows session layer (TODO(win32)).

#include "KPtyProcess.h"
#include "tools.h"

#ifndef Q_OS_WIN

#include <csignal>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace Konsole {

KPtyProcess::KPtyProcess(QObject *parent)
    : KProcess(parent)
{
    init(-1);
}

KPtyProcess::KPtyProcess(int ptyMasterFd, QObject *parent)
    : KProcess(parent)
{
    init(ptyMasterFd);
}

void KPtyProcess::init(int ptyMasterFd)
{
    m_pty = new KPtyDevice(this);

    if (ptyMasterFd == -1) {
        // 对应C++: d->pty->open()
        if (!m_pty->open()) {
            qCWarning(qtermwidgetLogger) << "KPtyProcess: failed to open KPtyDevice";
        }
    } else {
        // 对应C++: d->pty->open(ptyMasterFd)
        m_pty->open(ptyMasterFd);
    }

    m_ptyChannels = PtyChannelFlag::NoChannels;
    m_addUtmp = false;

    // 对应C++: setChildProcessModifier([d]() { ... setCTty + dup2 ... })
    setChildProcessModifier([this]() {
        setupChildProcessImpl();
    });
}

KPtyProcess::~KPtyProcess()
{
    // m_pty is a child of this; deleted by QObject.
}

void KPtyProcess::setChildProcessModifier(const std::function<void()> &modifier)
{
    m_childProcessModifier = modifier;
    QProcess::setChildProcessModifier(modifier);
}

std::function<void()> KPtyProcess::childProcessModifier() const
{
    return m_childProcessModifier;
}

void KPtyProcess::setupChildProcessImpl()
{
    // 对应C++: d->pty->setCTty();
    m_pty->setCTty();

    // 对应C++: if (d->ptyChannels & StdinChannel) dup2(d->pty->slaveFd(), 0); ...
    if (m_ptyChannels & PtyChannelFlag::StdinChannel)
        ::dup2(m_pty->slaveFd(), 0);
    if (m_ptyChannels & PtyChannelFlag::StdoutChannel)
        ::dup2(m_pty->slaveFd(), 1);
    if (m_ptyChannels & PtyChannelFlag::StderrChannel)
        ::dup2(m_pty->slaveFd(), 2);

    // Reset all signal handlers so the terminal app responds to Ctrl+C etc.
    // (matches Pty behaviour; QProcess does not do this for us)
    struct sigaction action;
    sigset_t sigset;
    sigemptyset(&sigset);
    action.sa_handler = SIG_DFL;
    action.sa_mask = sigset;
    action.sa_flags = 0;
    for (int signo = 1; signo < NSIG; ++signo)
        sigaction(signo, &action, nullptr);
}

void KPtyProcess::setPtyChannels(PtyChannels channels)
{
    m_ptyChannels = channels;
}

PtyChannels KPtyProcess::ptyChannels() const
{
    return m_ptyChannels;
}

KPtyDevice *KPtyProcess::pty() const
{
    return m_pty;
}

void KPtyProcess::start()
{
    // 对应C++: KProcess::start()
    KProcess::start();
}

bool KPtyProcess::waitForStarted(int msecs)
{
    // The child process was launched by QProcess via the child modifier.
    // QProcess::waitForStarted works because KPtyDevice doesn't interfere
    // with the fork/exec notification pipe.
    return QProcess::waitForStarted(msecs);
}

void KPtyProcess::setUseUtmp(bool value)
{
    m_addUtmp = value;
}

bool KPtyProcess::isUseUtmp() const
{
    return m_addUtmp;
}

void KPtyProcess::setWriteable(bool writeable)
{
    // 对应C++: struct stat sbuf; stat(...); chmod(...)
    const QByteArray tty = m_pty->ttyName();
    if (tty.isEmpty())
        return;
    struct stat sbuf;
    if (::stat(tty.constData(), &sbuf) != 0)
        return;
    if (writeable)
        ::chmod(tty.constData(), sbuf.st_mode | S_IWGRP);
    else
        ::chmod(tty.constData(), sbuf.st_mode & ~(S_IWGRP | S_IWOTH));
}

} // namespace Konsole

#else // Q_OS_WIN

// TODO(win32): KPtyProcess over ConPTY. See the Windows session layer.
namespace Konsole {
KPtyProcess::KPtyProcess(QObject *parent) : KProcess(parent), m_pty(nullptr), m_ptyChannels(PtyChannelFlag::NoChannels), m_addUtmp(false) {}
KPtyProcess::KPtyProcess(int, QObject *parent) : KPtyProcess(parent) {}
KPtyProcess::~KPtyProcess() {}
void KPtyProcess::init(int) {}
void KPtyProcess::setChildProcessModifier(const std::function<void()> &) {}
std::function<void()> KPtyProcess::childProcessModifier() const { return {}; }
void KPtyProcess::setupChildProcessImpl() {}
void KPtyProcess::setPtyChannels(PtyChannels channels) { m_ptyChannels = channels; }
PtyChannels KPtyProcess::ptyChannels() const { return m_ptyChannels; }
KPtyDevice *KPtyProcess::pty() const { return m_pty; }
void KPtyProcess::start() {}
bool KPtyProcess::waitForStarted(int) { return false; }
void KPtyProcess::setUseUtmp(bool value) { m_addUtmp = value; }
bool KPtyProcess::isUseUtmp() const { return m_addUtmp; }
void KPtyProcess::setWriteable(bool) {}
} // namespace Konsole

#endif // Q_OS_WIN
