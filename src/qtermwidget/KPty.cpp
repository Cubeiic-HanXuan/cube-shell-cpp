// KPty.cpp — C++ port of qtermwidget/kpty.py (POSIX only; Windows uses ConPTY
// elsewhere and is stubbed here).

#include "KPty.h"
#include "tools.h"

#ifndef Q_OS_WIN

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#if defined(Q_OS_LINUX)
#  include <pty.h>          // openpty, forkpty
#elif defined(Q_OS_MAC) || defined(Q_OS_FREEBSD) || defined(Q_OS_BSD4)
#  include <util.h>         // openpty
#elif defined(Q_OS_UNIX)
#  include <stropts.h>
#endif

namespace Konsole {

class KPtyPrivate {
public:
    explicit KPtyPrivate(KPty *parent)
        : masterFd(-1), slaveFd(-1), ownMaster(true), q_ptr(parent) {}

    int masterFd;
    int slaveFd;
    bool ownMaster;
    QByteArray ttyName;

private:
    KPty *q_ptr;
    Q_DECLARE_PUBLIC(KPty)
};

KPty::KPty()
    : d_ptr(new KPtyPrivate(this))
{
}

KPty::KPty(KPtyPrivate *d)
    : d_ptr(d)
{
}

KPty::~KPty()
{
    close();
    delete d_ptr;
}

bool KPty::open()
{
    Q_D(KPty);

    if (d->masterFd >= 0)
        return true;

    d->ownMaster = true;

    int master = -1, slave = -1;
    char name[64] = {0};
    if (::openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        qCWarning(qtermwidgetLogger) << "KPty::open: openpty() failed:" << ::strerror(errno);
        d->masterFd = -1;
        d->slaveFd = -1;
        return false;
    }

    d->masterFd = master;
    d->slaveFd = slave;
    d->ttyName = name;

    ::fcntl(d->masterFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(d->slaveFd, F_SETFD, FD_CLOEXEC);
    return true;
}

bool KPty::open(int fd)
{
    Q_D(KPty);

    if (d->masterFd >= 0) {
        qCWarning(qtermwidgetLogger) << "KPty::open: already open";
        return false;
    }

    d->ownMaster = false;

    // Resolve the slave name from the master fd.
    const char *name = ::ptsname(fd);
    if (!name) {
        qCWarning(qtermwidgetLogger) << "KPty::open(fd): ptsname() failed";
        return false;
    }
    d->ttyName = name;
    d->masterFd = fd;
    return true;
}

void KPty::close()
{
    Q_D(KPty);

    if (d->masterFd < 0)
        return;

    closeSlave();

    // don't bother resetting unix98 pty: it will go away when closed
    if (d->ownMaster)
        ::close(d->masterFd);
    d->masterFd = -1;
}

void KPty::closeSlave()
{
    Q_D(KPty);
    if (d->slaveFd < 0)
        return;
    ::close(d->slaveFd);
    d->slaveFd = -1;
}

bool KPty::openSlave()
{
    Q_D(KPty);
    if (d->slaveFd >= 0)
        return true;
    if (d->masterFd < 0) {
        qCWarning(qtermwidgetLogger) << "KPty::openSlave: pty not open";
        return false;
    }
    d->slaveFd = ::open(d->ttyName.constData(), O_RDWR | O_NOCTTY);
    if (d->slaveFd < 0) {
        qCWarning(qtermwidgetLogger) << "KPty::openSlave: can't open" << d->ttyName;
        return false;
    }
    ::fcntl(d->slaveFd, F_SETFD, FD_CLOEXEC);
    return true;
}

void KPty::setCTty()
{
    Q_D(KPty);

    // Job control: become session leader, drop old ctty.
    ::setsid();

#ifdef TIOCSCTTY
    ::ioctl(d->slaveFd, TIOCSCTTY, 0);
#else
    // __svr4__ hack: the first tty opened after setsid() becomes the ctty.
    int fd = ::open(d->ttyName.constData(), O_WRONLY);
    if (fd >= 0)
        ::close(fd);
#endif

    // Make our process group the foreground group on the pty.
    int pgrp = ::getpid();
#if defined(_POSIX_JOB_CONTROL) && defined(TCSETPGRP)
    ::tcsetpgrp(d->slaveFd, pgrp);
#elif defined(TIOCSPGRP)
    ::ioctl(d->slaveFd, TIOCSPGRP, &pgrp);
#endif
}

bool KPty::tcSetAttr(const struct termios *ttmode)
{
    Q_D(KPty);
    if (d->masterFd < 0)
        return false;
    return ::tcsetattr(d->masterFd, TCSANOW, ttmode) == 0;
}

bool KPty::tcGetAttr(struct termios *ttmode) const
{
    Q_D(const KPty);
    if (d->masterFd < 0)
        return false;
    return ::tcgetattr(d->masterFd, ttmode) == 0;
}

bool KPty::setWinSize(int lines, int columns)
{
    Q_D(KPty);
    if (d->masterFd < 0)
        return false;

    struct winsize win;
    win.ws_row = static_cast<unsigned short>(lines);
    win.ws_col = static_cast<unsigned short>(columns);
    win.ws_xpixel = 0;
    win.ws_ypixel = 0;
    return ::ioctl(d->masterFd, TIOCSWINSZ, &win) == 0;
}

bool KPty::setEcho(bool echo)
{
    struct termios ttmode;
    if (!tcGetAttr(&ttmode))
        return false;
    if (echo)
        ttmode.c_lflag |= ECHO;
    else
        ttmode.c_lflag &= ~ECHO;
    return tcSetAttr(&ttmode);
}

bool KPty::getWinSize(int *lines, int *columns) const
{
    Q_D(const KPty);
    if (d->masterFd < 0)
        return false;
    struct winsize win;
    if (::ioctl(d->masterFd, TIOCGWINSZ, &win) != 0)
        return false;
    *lines = win.ws_row;
    *columns = win.ws_col;
    return true;
}

QByteArray KPty::ttyName() const
{
    Q_D(const KPty);
    return d->ttyName;
}

int KPty::masterFd() const
{
    Q_D(const KPty);
    return d->masterFd;
}

int KPty::slaveFd() const
{
    Q_D(const KPty);
    return d->slaveFd;
}

bool KPty::isOpen() const
{
    Q_D(const KPty);
    return d->masterFd >= 0;
}

bool KPty::chownpty(bool)
{
    // Ownership/permissions of unix98 ptys are handled by the kernel (devpts);
    // no setuid helper needed. Kept for API parity with upstream.
    return true;
}

} // namespace Konsole

#else // Q_OS_WIN — ConPTY path lives in the Windows session layer; stub here.

namespace Konsole {

class KPtyPrivate {
public:
    explicit KPtyPrivate(KPty *) {}
    int masterFd = -1;
    int slaveFd = -1;
};

KPty::KPty() : d_ptr(new KPtyPrivate(this)) {}
KPty::KPty(KPtyPrivate *d) : d_ptr(d) {}
KPty::~KPty() { delete d_ptr; }
bool KPty::open() { return false; }
bool KPty::open(int) { return false; }
void KPty::close() {}
void KPty::closeSlave() {}
bool KPty::openSlave() { return false; }
void KPty::setCTty() {}
bool KPty::tcSetAttr(const struct termios *) { return false; }
bool KPty::tcGetAttr(struct termios *) const { return false; }
bool KPty::setWinSize(int, int) { return false; }
bool KPty::setEcho(bool) { return false; }
bool KPty::getWinSize(int *, int *) const { return false; }
QByteArray KPty::ttyName() const { return {}; }
int KPty::masterFd() const { return -1; }
int KPty::slaveFd() const { return -1; }
bool KPty::isOpen() const { return false; }
bool KPty::chownpty(bool) { return false; }

} // namespace Konsole

#endif // Q_OS_WIN
