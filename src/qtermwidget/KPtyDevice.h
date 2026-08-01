#pragma once

// KPtyDevice.h — C++ port of qtermwidget/kpty_device.py
//
// Wraps KPty as a QIODevice so it can be used with Qt stream classes,
// providing async I/O, signal emission and buffering. This is the Qt-only
// replacement for KDE's KPtyDevice.
//
// Original:
// Copyright (C) 2007 Oswald Buddenhagen <ossi@kde.org>
// Copyright (C) 2010 KDE e.V. <kde-ev-board@kde.org>

#include "KPty.h"

#include <climits>

#include <QIODevice>
#include <QVector>
#include <QByteArray>

namespace Konsole {

// 对应C++: class KRingBuffer
// Chunked ring buffer used internally for read/write buffering.
class KRingBuffer {
public:
    KRingBuffer() { clear(); }

    // 对应C++: void clear()
    void clear();

    // 对应C++: inline bool isEmpty() const
    bool isEmpty() const { return buffers.size() == 1 && tail == 0; }

    // 对应C++: inline int size() const
    int size() const { return totalSize; }

    // 对应C++: inline int readSize() const
    int readSize() const;

    // 对应C++: inline const char *readPointer() const
    const char *readPointer() const;

    // 对应C++: void free(int bytes)
    void free(int bytes);

    // 对应C++: char *reserve(int bytes)
    char *reserve(int bytes);

    // 对应C++: inline void unreserve(int bytes)
    void unreserve(int bytes);

    // 对应C++: inline void write(const char *data, int len)
    void write(const char *data, int len);

    // 对应C++: int indexAfter(char c, int maxLength = KMAXINT) const
    int indexAfter(char c, int maxLength = INT_MAX) const;

    // 对应C++: inline int lineSize(int maxLength = KMAXINT) const
    int lineSize(int maxLength = INT_MAX) const { return indexAfter('\n', maxLength); }

    // 对应C++: inline bool canReadLine() const
    bool canReadLine() const { return lineSize() != -1; }

    // 对应C++: int read(char *data, int maxLength)
    int read(char *data, int maxLength);

    // 对应C++: int readLine(char *data, int maxLength)
    int readLine(char *data, int maxLength);

private:
    static constexpr int CHUNKSIZE = 4096;

    QVector<QByteArray> buffers;
    int head = 0;
    int tail = 0;
    int totalSize = 0;
};

class KPtyDevicePrivate;

// 对应C++: class KPtyDevice : public QIODevice, public KPty
class KPtyDevice : public QIODevice, public KPty {
    Q_OBJECT
public:
    // 对应C++: KPtyDevice::KPtyDevice(QObject *parent)
    explicit KPtyDevice(QObject *parent = nullptr);
    ~KPtyDevice() override;

    // 对应C++: bool open(OpenMode mode = ReadWrite | Unbuffered) override
    bool open(OpenMode mode = ReadWrite | Unbuffered) override;
    // 对应C++: bool open(int fd, OpenMode mode = ReadWrite | Unbuffered)
    bool open(int fd, OpenMode mode = ReadWrite | Unbuffered);

    // 对应C++: void close() override
    void close() override;

    // 对应C++: void setSuspended(bool suspended)
    void setSuspended(bool suspended);
    // 对应C++: bool isSuspended() const
    bool isSuspended() const;

    // 对应C++: bool isSequential() const override
    bool isSequential() const override { return true; }
    // 对应C++: bool canReadLine() const override
    bool canReadLine() const override;
    // 对应C++: bool atEnd() const override
    bool atEnd() const override;
    // 对应C++: qint64 bytesAvailable() const override
    qint64 bytesAvailable() const override;
    // 对应C++: qint64 bytesToWrite() const override
    qint64 bytesToWrite() const override;
    // 对应C++: bool waitForBytesWritten(int msecs) override
    bool waitForBytesWritten(int msecs = -1) override;
    // 对应C++: bool waitForReadyRead(int msecs) override
    bool waitForReadyRead(int msecs = -1) override;

Q_SIGNALS:
    // 对应C++: void readEof()
    void readEof();

protected:
    // 对应C++: qint64 readData(char *data, qint64 maxlen) override
    qint64 readData(char *data, qint64 maxlen) override;
    // 对应C++: qint64 readLineData(char *data, qint64 maxlen) override
    qint64 readLineData(char *data, qint64 maxlen) override;
    // 对应C++: qint64 writeData(const char *data, qint64 len) override
    qint64 writeData(const char *data, qint64 len) override;

private:
    friend class KPtyDevicePrivate;
    // Device-specific private state (ring buffers + socket notifiers).
    // Owned by this object; distinct from KPty's own d_ptr.
    KPtyDevicePrivate *m_devPriv;
};

} // namespace Konsole
