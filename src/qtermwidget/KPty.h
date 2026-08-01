#pragma once

// KPty.h — C++ port of qtermwidget/kpty.py
//
// Encapsulates a pseudo-terminal master/slave pair (POSIX). This is the
// Qt-only replacement for KDE's KPty. Ported from the Python version, which
// was converted from kpty.cpp / kpty.h.
//
// Original: Copyright (C) 1997-2002 by the Konsole / KDE authors.

#include <QByteArray>
#include <QString>

struct termios; // fwd

namespace Konsole {

class KPtyPrivate;

class KPty {
public:
    KPty();
    virtual ~KPty();

    KPty(const KPty &) = delete;
    KPty &operator=(const KPty &) = delete;

    // Creates a master/slave pty pair.
    // 对应C++: bool open()
    bool open();
    // Creates a pty from an existing master fd.
    // 对应C++: bool open(int fd)
    bool open(int fd);

    // Close the pty master/slave pair.
    // 对应C++: void close()
    void close();

    // Close only the slave.
    // 对应C++: void closeSlave()
    void closeSlave();

    // Open the slave side (after closeSlave).
    // 对应C++: bool openSlave()
    bool openSlave();

    // Make the slave the controlling terminal of the calling process.
    // 对应C++: void setCTty()
    void setCTty();

    // 对应C++: bool tcSetAttr(struct ::termios *ttmode) / tcGetAttr
    bool tcSetAttr(const struct termios *ttmode);
    bool tcGetAttr(struct termios *ttmode) const;

    // Set the terminal size (lines x columns). Emitted to the child via ioctl.
    // 对应C++: bool setWinSize(int lines, int columns)
    bool setWinSize(int lines, int columns);

    // 对应C++: bool setEcho(bool echo)
    bool setEcho(bool echo);

    // The name of the slave pty device (e.g. /dev/pts/1, /dev/ttys003).
    // 对应C++: const char *ttyName() const
    QByteArray ttyName() const;

    int masterFd() const;
    int slaveFd() const;

    bool isOpen() const;

    // Query current window size. Returns false on error.
    bool getWinSize(int *lines, int *columns) const;

protected:
    KPtyPrivate *const d_ptr;
    explicit KPty(KPtyPrivate *d);
    Q_DECLARE_PRIVATE(KPty)

private:
    bool chownpty(bool grant);
};

} // namespace Konsole
