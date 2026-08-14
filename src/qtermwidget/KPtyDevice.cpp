// KPtyDevice.cpp — C++ port of qtermwidget/kpty_device.py (POSIX only).
// Windows ConPTY path is handled elsewhere; this module is the POSIX master.

#include "KPtyDevice.h"
#include "tools.h"

#ifndef Q_OS_WIN

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QSocketNotifier>

// 对应C++: #define PTY_BYTES_AVAILABLE ... (platform specific ioctl)
#if defined(Q_OS_FREEBSD) || defined(Q_OS_MAC)
// "the other end's output queue size"
#  define PTY_BYTES_AVAILABLE TIOCOUTQ
#elif defined(TIOCINQ)
// "our end's input queue size"
#  define PTY_BYTES_AVAILABLE TIOCINQ
#else
// the only generic one
#  define PTY_BYTES_AVAILABLE FIONREAD
#endif

// 对应C++: NO_INTR(x) macro
#define NO_INTR(x)                                                                                                     \
    ({                                                                                                                 \
        int result;                                                                                                    \
        while (((result = (x)) < 0) && (errno == EINTR))                                                               \
            ;                                                                                                          \
        result;                                                                                                        \
    })

namespace Konsole {

// ---------------------------------------------------------------------------
// KRingBuffer
// ---------------------------------------------------------------------------

void KRingBuffer::clear()
{
    buffers.clear();
    buffers.append(QByteArray(CHUNKSIZE, '\0'));
    head = 0;
    tail = 0;
    totalSize = 0;
}

int KRingBuffer::readSize() const
{
    return (buffers.size() == 1 ? tail : buffers.first().size()) - head;
}

const char *KRingBuffer::readPointer() const
{
    Q_ASSERT(totalSize > 0);
    return buffers.first().constData() + head;
}

void KRingBuffer::free(int bytes)
{
    totalSize -= bytes;
    Q_ASSERT(totalSize >= 0);

    forever {
        int nbs = readSize();

        if (bytes < nbs) {
            head += bytes;
            if (head == tail && buffers.size() == 1) {
                buffers.first().resize(CHUNKSIZE);
                buffers.first().fill('\0');
                head = tail = 0;
            }
            break;
        }

        bytes -= nbs;
        if (buffers.size() == 1) {
            buffers.first().resize(CHUNKSIZE);
            buffers.first().fill('\0');
            head = tail = 0;
            break;
        }

        buffers.removeFirst();
        head = 0;
    }
}

char *KRingBuffer::reserve(int bytes)
{
    totalSize += bytes;

    if (tail + bytes <= buffers.last().size()) {
        char *ptr = buffers.last().data() + tail;
        tail += bytes;
        return ptr;
    }

    buffers.last().resize(tail);
    buffers.append(QByteArray(qMax(CHUNKSIZE, bytes), '\0'));
    tail = bytes;
    return buffers.last().data();
}

void KRingBuffer::unreserve(int bytes)
{
    totalSize -= bytes;
    tail -= bytes;
}

void KRingBuffer::write(const char *data, int len)
{
    memcpy(reserve(len), data, len);
}

int KRingBuffer::indexAfter(char c, int maxLength) const
{
    int index = 0;
    int start = head;
    QVector<QByteArray>::ConstIterator it = buffers.constBegin();

    forever {
        if (!maxLength)
            return index;
        if (index == size())
            return -1;

        const QByteArray &buf = *it;
        ++it;

        int end = (it == buffers.constEnd() ? tail : buf.size());

        int length = qMin(end - start, maxLength);

        const char *p = static_cast<const char *>(memchr(buf.constData() + start, c, length));
        if (p)
            return index + (p - (buf.constData() + start)) + 1;

        index += length;
        maxLength -= length;
        start = 0;
    }
}

int KRingBuffer::read(char *data, int maxLength)
{
    int bytesToRead = qMin(size(), maxLength);
    int readSoFar = 0;
    while (readSoFar < bytesToRead) {
        const char *ptr = readPointer();
        int bs = qMin(bytesToRead - readSoFar, readSize());
        memcpy(data + readSoFar, ptr, bs);
        readSoFar += bs;
        free(bs);
    }
    return readSoFar;
}

int KRingBuffer::readLine(char *data, int maxLength)
{
    int lineLen = lineSize(qMin(maxLength, size()));
    if (lineLen == -1)
        lineLen = qMin(maxLength, size());
    return read(data, lineLen);
}

// ---------------------------------------------------------------------------
// KPtyDevicePrivate
// ---------------------------------------------------------------------------

// 对应C++: class KPtyDevicePrivate : public KPtyPrivate
class KPtyDevicePrivate {
public:
    explicit KPtyDevicePrivate(KPtyDevice *parent)
        : emittedReadyRead(false)
        , emittedBytesWritten(false)
        , readNotifier(nullptr)
        , writeNotifier(nullptr)
        , q_ptr(parent)
    {
    }

    // 对应C++: bool _k_canRead()
    bool _k_canRead();
    // 对应C++: bool _k_canWrite()
    bool _k_canWrite();
    // 对应C++: bool doWait(int msecs, bool reading)
    bool doWait(int msecs, bool reading);
    // 对应C++: void finishOpen(QIODevice::OpenMode mode)
    void finishOpen(QIODevice::OpenMode mode);

    bool emittedReadyRead;
    bool emittedBytesWritten;
    QSocketNotifier *readNotifier;
    QSocketNotifier *writeNotifier;
    KRingBuffer readBuffer;
    KRingBuffer writeBuffer;

private:
    KPtyDevice *q_ptr;
};

bool KPtyDevicePrivate::_k_canRead()
{
    KPtyDevice *q = q_ptr;
    int readBytes = 0;

    int available = 0;
    bool sizeKnown = true;
    if (::ioctl(q->masterFd(), PTY_BYTES_AVAILABLE, &available) == -1) {
        // 鸿蒙 OHOS 沙箱用 seccomp 过滤 pty master 上的 FIONREAD → EPERM
        // （也可能是不支持该 ioctl 的平台 → ENOTTY/ENOSYS）。此时 readNotifier
        // 既然已触发，fd 就是可读的——不能因探测不到字节数就放弃读（否则终端空白）。
        // 回退为按固定块大小直接 read()：pty 的 read 有数据即返回、不会等满缓冲区，
        // 后续 read() 返回 0 仍走下面的 EOF 分支，逻辑闭环。
        if (errno == EPERM || errno == EACCES || errno == ENOTTY || errno == ENOSYS) {
            sizeKnown = false;
            available = 4096;
        } else {
            qCWarning(qtermwidgetLogger) << "KPtyDevice: ioctl(PTY_BYTES_AVAILABLE) failed:" << ::strerror(errno);
            return false;
        }
    }

#ifdef Q_OS_SOLARIS
    // A 0-byte read on Solaris returns the 0-byte STREAMS message.
    if (sizeKnown && !available) {
        char c;
        if (!NO_INTR(::read(q->masterFd(), &c, 0))) {
            if (readNotifier)
                readNotifier->setEnabled(false);
            Q_EMIT q->readEof();
            return false;
        }
        return true;
    }
#else
    if (sizeKnown && !available) {
        if (readNotifier)
            readNotifier->setEnabled(false);
        Q_EMIT q->readEof();
        return false;
    }
#endif

    char *ptr = readBuffer.reserve(available);
    if (!ptr) {
        qCWarning(qtermwidgetLogger) << "KPtyDevice: failed to reserve read buffer";
        return false;
    }

    readBytes = NO_INTR(::read(q->masterFd(), ptr, available));
    if (readBytes < 0) {
        readBuffer.unreserve(available);
        if (errno == EBADF) {
            if (readNotifier)
                readNotifier->setEnabled(false);
            return false;
        }
        q->setErrorString(QStringLiteral("Error reading from PTY"));
        return false;
    }
    Q_ASSERT(readBytes <= available);
    readBuffer.unreserve(available - readBytes); // *should* be a no-op

    if (!readBytes) {
        if (readNotifier)
            readNotifier->setEnabled(false);
        Q_EMIT q->readEof();
        return false;
    }

    if (!emittedReadyRead) {
        emittedReadyRead = true;
        Q_EMIT q->readyRead();
        emittedReadyRead = false;
    }
    return true;
}

bool KPtyDevicePrivate::_k_canWrite()
{
    KPtyDevice *q = q_ptr;

    if (writeNotifier)
        writeNotifier->setEnabled(false);
    if (writeBuffer.isEmpty())
        return false;

    int wroteBytes = NO_INTR(::write(q->masterFd(), writeBuffer.readPointer(), writeBuffer.readSize()));
    if (wroteBytes < 0) {
        q->setErrorString(QStringLiteral("Error writing to PTY"));
        return false;
    }
    writeBuffer.free(wroteBytes);

    if (!emittedBytesWritten) {
        emittedBytesWritten = true;
        Q_EMIT q->bytesWritten(wroteBytes);
        emittedBytesWritten = false;
    }

    if (!writeBuffer.isEmpty() && writeNotifier)
        writeNotifier->setEnabled(true);

    return true;
}

bool KPtyDevicePrivate::doWait(int msecs, bool reading)
{
    KPtyDevice *q = q_ptr;

#ifndef Q_OS_LINUX
    struct timeval etv;
    if (msecs >= 0) {
        struct timeval tv;
        ::gettimeofday(&tv, nullptr);
        etv.tv_sec = tv.tv_sec + msecs / 1000;
        etv.tv_usec = tv.tv_usec + (msecs % 1000) * 1000;
        if (etv.tv_usec >= 1000000) {
            etv.tv_sec += 1;
            etv.tv_usec -= 1000000;
        }
    }
#endif

    while ((reading && readNotifier && readNotifier->isEnabled()) || (!reading && !writeBuffer.isEmpty())) {
        fd_set rfds;
        fd_set wfds;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        if (readNotifier && readNotifier->isEnabled())
            FD_SET(q->masterFd(), &rfds);
        if (!writeBuffer.isEmpty())
            FD_SET(q->masterFd(), &wfds);

        struct timeval tv;
        struct timeval *tvp = nullptr;
        if (msecs >= 0) {
#ifdef Q_OS_LINUX
            tv.tv_sec = msecs / 1000;
            tv.tv_usec = (msecs % 1000) * 1000;
#else
            struct timeval ctv;
            ::gettimeofday(&ctv, nullptr);
            tv.tv_sec = etv.tv_sec - ctv.tv_sec;
            tv.tv_usec = etv.tv_usec - ctv.tv_usec;
            if (tv.tv_usec < 0) {
                tv.tv_sec -= 1;
                tv.tv_usec += 1000000;
            }
            if (tv.tv_sec < 0) {
                q->setErrorString(QStringLiteral("PTY operation timed out"));
                return false;
            }
#endif
            tvp = &tv;
        }

        int retval = ::select(q->masterFd() + 1, &rfds, &wfds, nullptr, tvp);
        switch (retval) {
        case -1:
            if (errno == EINTR)
                continue;
            return false;
        case 0:
            q->setErrorString(QStringLiteral("PTY operation timed out"));
            return false;
        default:
            if (FD_ISSET(q->masterFd(), &rfds)) {
                bool canRead = _k_canRead();
                if (reading && canRead)
                    return true;
            }
            if (FD_ISSET(q->masterFd(), &wfds)) {
                bool canWrite = _k_canWrite();
                if (!reading)
                    return canWrite;
            }
        }
    }
    return false;
}

void KPtyDevicePrivate::finishOpen(QIODevice::OpenMode mode)
{
    KPtyDevice *q = q_ptr;

    q->QIODevice::open(mode);
    ::fcntl(q->masterFd(), F_SETFL, O_NONBLOCK);
    readBuffer.clear();

    if (QCoreApplication::instance()) {
        readNotifier = new QSocketNotifier(q->masterFd(), QSocketNotifier::Read, q);
        QObject::connect(readNotifier, &QSocketNotifier::activated, q, [this](int) { _k_canRead(); });
        writeNotifier = new QSocketNotifier(q->masterFd(), QSocketNotifier::Write, q);
        QObject::connect(writeNotifier, &QSocketNotifier::activated, q, [this](int) { _k_canWrite(); });
        readNotifier->setEnabled(true);
    }
}

// ---------------------------------------------------------------------------
// KPtyDevice
// ---------------------------------------------------------------------------

KPtyDevice::KPtyDevice(QObject *parent)
    : QIODevice(parent)
    , m_devPriv(new KPtyDevicePrivate(this))
{
}

KPtyDevice::~KPtyDevice()
{
    close();
    delete m_devPriv;
}

bool KPtyDevice::open(OpenMode mode)
{
    if (masterFd() >= 0)
        return true;

    if (!KPty::open()) {
        setErrorString(QStringLiteral("Error opening PTY"));
        return false;
    }

    m_devPriv->finishOpen(mode);
    return true;
}

bool KPtyDevice::open(int fd, OpenMode mode)
{
    if (masterFd() >= 0)
        return true;

    if (!KPty::open(fd)) {
        setErrorString(QStringLiteral("Error opening PTY"));
        return false;
    }

    m_devPriv->finishOpen(mode);
    return true;
}

void KPtyDevice::close()
{
    if (masterFd() < 0)
        return;

    delete m_devPriv->readNotifier;
    m_devPriv->readNotifier = nullptr;
    delete m_devPriv->writeNotifier;
    m_devPriv->writeNotifier = nullptr;

    QIODevice::close();
    KPty::close();
}

void KPtyDevice::setSuspended(bool suspended)
{
    if (m_devPriv->readNotifier)
        m_devPriv->readNotifier->setEnabled(!suspended);
}

bool KPtyDevice::isSuspended() const
{
    return m_devPriv->readNotifier && !m_devPriv->readNotifier->isEnabled();
}

bool KPtyDevice::canReadLine() const
{
    return QIODevice::canReadLine() || m_devPriv->readBuffer.canReadLine();
}

bool KPtyDevice::atEnd() const
{
    return QIODevice::atEnd() && m_devPriv->readBuffer.isEmpty();
}

qint64 KPtyDevice::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + m_devPriv->readBuffer.size();
}

qint64 KPtyDevice::bytesToWrite() const
{
    return m_devPriv->writeBuffer.size();
}

bool KPtyDevice::waitForBytesWritten(int msecs)
{
    return m_devPriv->doWait(msecs, false);
}

bool KPtyDevice::waitForReadyRead(int msecs)
{
    return m_devPriv->doWait(msecs, true);
}

qint64 KPtyDevice::readData(char *data, qint64 maxlen)
{
    qint64 maxLen = qMin<qint64>(maxlen, INT_MAX);
    return m_devPriv->readBuffer.read(data, int(maxLen));
}

qint64 KPtyDevice::readLineData(char *data, qint64 maxlen)
{
    qint64 maxLen = qMin<qint64>(maxlen, INT_MAX);
    return m_devPriv->readBuffer.readLine(data, int(maxLen));
}

qint64 KPtyDevice::writeData(const char *data, qint64 len)
{
    Q_ASSERT(len <= INT_MAX);

    m_devPriv->writeBuffer.write(data, int(len));

    if (m_devPriv->writeNotifier)
        m_devPriv->writeNotifier->setEnabled(true);

    return len;
}

} // namespace Konsole

#else // Q_OS_WIN

// TODO(win32): KPtyDevice over ConPTY. The POSIX path above is the main one;
// Windows session layer drives ConPTY directly.
namespace Konsole {
KPtyDevice::KPtyDevice(QObject *parent) : QIODevice(parent), m_devPriv(nullptr) {}
KPtyDevice::~KPtyDevice() {}
bool KPtyDevice::open(OpenMode) { return false; }
bool KPtyDevice::open(int, OpenMode) { return false; }
void KPtyDevice::close() {}
void KPtyDevice::setSuspended(bool) {}
bool KPtyDevice::isSuspended() const { return false; }
bool KPtyDevice::canReadLine() const { return false; }
bool KPtyDevice::atEnd() const { return true; }
qint64 KPtyDevice::bytesAvailable() const { return 0; }
qint64 KPtyDevice::bytesToWrite() const { return 0; }
bool KPtyDevice::waitForBytesWritten(int) { return false; }
bool KPtyDevice::waitForReadyRead(int) { return false; }
qint64 KPtyDevice::readData(char *, qint64) { return 0; }
qint64 KPtyDevice::readLineData(char *, qint64) { return 0; }
qint64 KPtyDevice::writeData(const char *, qint64) { return 0; }
} // namespace Konsole

#endif // Q_OS_WIN
