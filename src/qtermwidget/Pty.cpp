// Pty.cpp — C++ port of qtermwidget/pty.py (POSIX only; Windows ConPTY is
// handled by the Windows session layer, stubbed here).

#include "Pty.h"
#include "tools.h"

#ifndef Q_OS_WIN

#include <csignal>
#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

namespace Konsole {

Pty::Pty(QObject *parent)
    : KPtyProcess(parent)
{
    init();
}

Pty::Pty(int ptyMasterFd, QObject *parent)
    : KPtyProcess(ptyMasterFd, parent)
{
    init();
}

void Pty::init()
{
    // 对应C++: connect(pty(), SIGNAL(readyRead()), this, SLOT(dataReceived()));
    connect(pty(), &KPtyDevice::readyRead, this, &Pty::dataReceived);

    // 对应C++: setPtyChannels(KPtyProcess::AllChannels)
    setPtyChannels(PtyChannelFlag::AllChannels);
}

Pty::~Pty() = default;

int Pty::start(const QString &program, const QStringList &programArguments, const QStringList &environment, int winid, bool addToUtmp)
{
    clearProgram();

    // For historical reasons, the first argument is the command to run.
    Q_ASSERT(programArguments.count() >= 1);
    if (programArguments.isEmpty())
        return -1;

    setProgram(program, programArguments.mid(1));

    addEnvironmentVariables(environment);

    setEnv(QStringLiteral("WINDOWID"), QString::number(winid));
    // 显式告知子进程终端支持 24-bit 真彩色（Node supports-color、bat、delta 等
    // 工具据此启用全保真渲染），与 Python 版 kptyprocess.py 对齐。
    setEnv(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"), true);

    // 对应C++: setUseUtmp(addToUtmp)
    setUseUtmp(addToUtmp);

    applyTerminalSettings();

    // 对应C++: pty()->setWinSize(_windowLines, _windowColumns)
    pty()->setWinSize(m_windowLines, m_windowColumns);

    KProcess::start();
    if (!waitForStarted())
        return -1;

    return 0;
}

void Pty::setWriteable(bool writeable)
{
    struct stat sbuf;
    if (::stat(pty()->ttyName().constData(), &sbuf) != 0)
        return;
    if (writeable)
        ::chmod(pty()->ttyName().constData(), sbuf.st_mode | S_IWGRP);
    else
        ::chmod(pty()->ttyName().constData(), sbuf.st_mode & ~(S_IWGRP | S_IWOTH));
}

void Pty::setFlowControlEnabled(bool enable)
{
    m_xonXoff = enable;

    if (pty()->masterFd() >= 0) {
        struct termios ttmode;
        pty()->tcGetAttr(&ttmode);
        if (!enable)
            ttmode.c_iflag &= ~(IXOFF | IXON);
        else
            ttmode.c_iflag |= (IXOFF | IXON);
        if (!pty()->tcSetAttr(&ttmode))
            qCWarning(qtermwidgetLogger) << "Unable to set terminal attributes.";
    }
}

bool Pty::flowControlEnabled() const
{
    if (pty()->masterFd() >= 0) {
        struct termios ttmode;
        pty()->tcGetAttr(&ttmode);
        return ((ttmode.c_iflag & IXOFF) && (ttmode.c_iflag & IXON));
    }
    qCWarning(qtermwidgetLogger) << "Unable to get flow control status, terminal not connected.";
    return false;
}

void Pty::setUtf8Mode(bool enable)
{
    m_utf8 = enable;

#ifdef IUTF8 // XXX not a reasonable place to check this.
    if (pty()->masterFd() >= 0) {
        struct termios ttmode;
        pty()->tcGetAttr(&ttmode);
        if (!enable)
            ttmode.c_iflag &= ~IUTF8;
        else
            ttmode.c_iflag |= IUTF8;
        if (!pty()->tcSetAttr(&ttmode))
            qCWarning(qtermwidgetLogger) << "Unable to set terminal attributes.";
    }
#endif
}

void Pty::setErase(char eraseChar)
{
    m_eraseChar = eraseChar;

    if (pty()->masterFd() >= 0) {
        struct termios ttmode;
        pty()->tcGetAttr(&ttmode);
        ttmode.c_cc[VERASE] = eraseChar;
        if (!pty()->tcSetAttr(&ttmode))
            qCWarning(qtermwidgetLogger) << "Unable to set terminal attributes.";
    }
}

char Pty::erase() const
{
    if (pty()->masterFd() >= 0) {
        struct termios ttyAttributes;
        pty()->tcGetAttr(&ttyAttributes);
        return ttyAttributes.c_cc[VERASE];
    }

    return m_eraseChar;
}

void Pty::setEmptyPTYProperties()
{
    applyTerminalSettings();
}

void Pty::addEnvironmentVariables(const QStringList &environment)
{
    bool termEnvVarAdded = false;
    bool langAdded = false;
    bool lcAllAdded = false;
    for (const QString &pair : environment) {
        int pos = pair.indexOf(QLatin1Char('='));
        if (pos >= 0) {
            const QString variable = pair.left(pos);
            const QString value = pair.mid(pos + 1);
            setEnv(variable, value);
            if (variable == QLatin1String("TERM"))
                termEnvVarAdded = true;
            else if (variable == QLatin1String("LANG"))
                langAdded = true;
            else if (variable == QLatin1String("LC_ALL"))
                lcAllAdded = true;
        }
    }

    if (!termEnvVarAdded)
        setEnv(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));

    // 对应 Python kptyprocess.py: env_dict['LANG'] = env_dict.get('LANG', 'en_US.UTF-8')
    // macOS GUI 应用从 Finder/Dock 启动时 launchd 环境不含 LANG，导致 bash/readline
    // 无法识别 UTF-8 多字节字符，把中文每个字节当 meta 字符回显（<008e> 之类乱码）。
    if (!langAdded)
        setEnv(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"), false);
    if (!lcAllAdded)
        setEnv(QStringLiteral("LC_ALL"), QStringLiteral("en_US.UTF-8"), false);
}

void Pty::applyTerminalSettings()
{
    if (pty()->masterFd() < 0)
        return;

    struct termios ttmode;
    pty()->tcGetAttr(&ttmode);

    if (!m_xonXoff)
        ttmode.c_iflag &= ~(IXOFF | IXON);
    else
        ttmode.c_iflag |= (IXOFF | IXON);

    // 对应 Python kptyprocess.py::_configure_ssh_tty_attrs:
    // 关闭 ISTRIP，避免 UTF-8 输入的高位被裁剪（中文多字节 >= 0x80 会被破坏）。
    ttmode.c_iflag &= ~ISTRIP;

#ifdef IUTF8
    if (!m_utf8)
        ttmode.c_iflag &= ~IUTF8;
    else
        ttmode.c_iflag |= IUTF8;
#endif

    // 对应 Python kptyprocess.py: attrs[2] |= (termios.CS8 | termios.CREAD | termios.CLOCAL)
    // 确保 8-bit 字符模式，这是 UTF-8 正常传输的必要条件。
    ttmode.c_cflag |= (CS8 | CREAD | CLOCAL);

    if (m_eraseChar != 0)
        ttmode.c_cc[VERASE] = m_eraseChar;

    if (!pty()->tcSetAttr(&ttmode))
        qCWarning(qtermwidgetLogger) << "Unable to set terminal attributes.";
}

void Pty::setWindowSize(int lines, int cols)
{
    m_windowColumns = cols;
    m_windowLines = lines;

    if (pty()->masterFd() >= 0)
        pty()->setWinSize(lines, cols);
}

QSize Pty::windowSize() const
{
    return QSize(m_windowColumns, m_windowLines);
}

int Pty::foregroundProcessGroup() const
{
    const int master_fd = pty()->masterFd();

    if (master_fd >= 0) {
        int pid = tcgetpgrp(master_fd);
        if (pid != -1)
            return pid;
    }

    return 0;
}

void Pty::closePty()
{
    pty()->close();
}

void Pty::lockPty(bool lock)
{
    // TODO: Support for locking the Pty
    // if (lock)
    //     suspend();
    // else
    //     resume();
    Q_UNUSED(lock);
}

void Pty::sendData(const char *data, int length)
{
    if (!length)
        return;

    if (!pty()->write(data, length))
        qCWarning(qtermwidgetLogger) << "Pty::doSendJobs - Could not send input data to terminal process.";
}

void Pty::dataReceived()
{
    QByteArray data = pty()->readAll();
    if (data.isEmpty())
        return;
    Q_EMIT receivedData(data.constData(), data.size());
}

} // namespace Konsole

#else // Q_OS_WIN

// TODO(win32): Pty over ConPTY. The Windows session layer drives ConPTY and
// forwards receivedData / sendData; this stub keeps the interface present.
namespace Konsole {
Pty::Pty(QObject *parent) : KPtyProcess(parent) {}
Pty::Pty(int ptyMasterFd, QObject *parent) : KPtyProcess(ptyMasterFd, parent) {}
Pty::~Pty() {}
void Pty::init() {}
int Pty::start(const QString &, const QStringList &, const QStringList &, int, bool) { return -1; }
void Pty::setWriteable(bool) {}
void Pty::setFlowControlEnabled(bool enable) { m_xonXoff = enable; }
bool Pty::flowControlEnabled() const { return false; }
void Pty::setUtf8Mode(bool enable) { m_utf8 = enable; }
void Pty::setErase(char eraseChar) { m_eraseChar = eraseChar; }
char Pty::erase() const { return m_eraseChar; }
void Pty::setEmptyPTYProperties() {}
void Pty::addEnvironmentVariables(const QStringList &) {}
void Pty::applyTerminalSettings() {}
void Pty::setWindowSize(int lines, int cols) { m_windowLines = lines; m_windowColumns = cols; }
QSize Pty::windowSize() const { return QSize(m_windowColumns, m_windowLines); }
int Pty::foregroundProcessGroup() const { return 0; }
void Pty::closePty() {}
void Pty::lockPty(bool) {}
void Pty::sendData(const char *, int) {}
void Pty::dataReceived() {}
} // namespace Konsole

#endif // Q_OS_WIN
