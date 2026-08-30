#pragma once

// session_recorder.h — 会话原始流录制器。
//
// 把「未经任何协议处理」的原始字节落盘，用于审计追溯（"凌晨三点在生产敲了什么"）。
// 相对 TcpClient/SerialClient 内嵌的 m_logFile 直写，这里额外提供：
//   - 行首时间戳（审计定位）；
//   - 按大小轮转（避免单个日志文件无限增长）；
//   - 自动按 tag+时间命名（无需每次另存为）。
//
// 线程安全：SSH 的读循环跑在专用工作线程（SshBridge::readLoop），而
// Tcp/Serial 在 GUI 线程 readyRead。所有公有方法内部持同一把 QMutex，
// 任意线程都可直接调用 writeRaw()/stop()。

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QString>

namespace cubeshell {

class SessionRecorder {
public:
    struct Options {
        bool addTimestamps = false;  // 行首加 [HH:MM:SS.zzz]
        qint64 maxBytes = 0;         // >0 启用按大小轮转
        int backupCount = 5;         // 轮转时保留的历史卷数
    };

    SessionRecorder() = default;
    ~SessionRecorder();

    // 以 path 为目标开始录制。目录不存在先建；Append 打开（同目标多次连接累积）。
    // 已在录制时先 stop()。返回 false 并填 errOut 表示失败。
    bool start(const QString &path, const Options &opt, QString *errOut = nullptr);
    void stop();
    bool isActive() const;
    QString filePath() const;

    // 写一段原始字节。线程安全。崩溃也要留下已收到的数据（每批 flush）。
    void writeRaw(const QByteArray &data);

    // 自动命名：<dir>/<tag>-<yyyyMMdd-HHmmss>.log。
    static QString autoFileName(const QString &tag, const QString &dir);
    // 净化设备名/主机名为可入文件名的串（路径分隔、IPv6 冒号、空白等 → '_'）。
    static QString sanitizeTag(const QString &tag);

private:
    // 调用方须已持 m_mutex。size 超限时把当前卷改名 .1/.2… 并以原路径重开新卷。
    void rotateIfNeededLocked();

    mutable QMutex m_mutex;
    QFile m_file;
    Options m_opt;
    bool m_active = false;
    // 时间戳模式：下一字节是否处于行首（决定要不要先插时间戳）。
    bool m_atLineStart = true;
};

} // namespace cubeshell
