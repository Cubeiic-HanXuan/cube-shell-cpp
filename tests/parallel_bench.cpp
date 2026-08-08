// parallel_bench.cpp — 并行 SFTP 上传/下载的吞吐对比（需要活的 sshd）。
//
// 同一个文件分别用 1 条连接（串行，等价于改造前的行为）和 N 条连接（并行）
// 各传一遍，打印耗时与吞吐。
//
// 重要：本地回环 RTT 近乎 0，而 SFTP 单流吞吐 ≈ 分片 / RTT —— 并行的收益正比
// 于 RTT，所以本机跑出来的提速会远小于真实跨网链路。用 CUBESSH_DELAY_MS 可
// 在测量里模拟额外延迟（仅影响本进程的观测，不改协议行为）。
//
// Env: CUBESSH_HOST/PORT/USER/PASS, CUBESSH_REMOTE_BASE, CUBESSH_BENCH_MB。

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

// 内容必须与位置相关：并行分片是各流 seek 到自己的偏移写的，如果填充全同
// 字节，分片写错位置也照样校验通过 —— 那种"校验"什么都证明不了。
// 用偏移驱动的确定性 LCG 填充，任何错位/重叠/空洞都会改变 MD5。
static bool writeTestFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const qint64 blockSize = 1024 * 1024;
    QByteArray block(blockSize, Qt::Uninitialized);
    qint64 written = 0;
    while (written < size) {
        const qint64 n = qMin<qint64>(blockSize, size - written);
        quint64 s = quint64(written) * 6364136223846793005ULL + 1442695040888963407ULL;
        for (qint64 i = 0; i < n; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            block[int(i)] = char(quint8(s >> 33));
        }
        f.write(block.constData(), n);
        written += n;
    }
    return true;
}

// 本地文件 MD5，用于和远端 md5sum 对比，验证并行写出的字节完全一致。
static QString localMd5(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!hash.addData(&f))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

// 跑一次上传并返回耗时毫秒；connections 为传输连接数上限。
static qint64 runUpload(SshClient *client, const QString &localPath,
                        const QString &remotePath, int connections,
                        const QString &metaDir)
{
    SftpUploaderCore uploader(client);
    uploader.setMetadataDir(metaDir);
    uploader.setMaxTransferConnections(connections);

    // 预热：把 SSH 握手成本挪出计时窗口。不这么做的话测出来的是"握手 +
    // 传输"，高延迟链路上握手（约 300ms/条）会淹没传输本身的差异 ——
    // 这也正是产品代码里 prewarmConnections() 存在的理由。
    uploader.prewarmConnections();
    for (int i = 0; i < 100 && uploader.activeTransferConnections() < connections; ++i) {
        QEventLoop warm;
        QTimer::singleShot(50, &warm, &QEventLoop::quit);
        warm.exec();
    }
    qInfo().noquote() << QStringLiteral("  预热完成，就绪连接: %1")
                             .arg(uploader.activeTransferConnections());

    bool done = false;
    QString failure;
    QObject::connect(&uploader, &SftpUploaderCore::uploadCompleted,
                     [&done](const QString &, const QString &) { done = true; });
    QObject::connect(&uploader, &SftpUploaderCore::uploadFailed,
                     [&done, &failure](const QString &, const QString &, const QString &e) {
                         failure = e;
                         done = true;
                     });

    QElapsedTimer timer;
    timer.start();
    uploader.uploadFile(QStringLiteral("bench"), localPath, remotePath);

    // 等完成（同时抽事件循环，让 QueuedConnection 的信号落地）。
    while (!done && timer.elapsed() < 600000) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const qint64 elapsed = timer.elapsed();
    uploader.waitForFinished(10000);

    if (!failure.isEmpty()) {
        qWarning() << "  upload FAILED:" << failure;
        return -1;
    }
    if (!done) {
        qWarning() << "  upload TIMED OUT";
        return -1;
    }
    qInfo().noquote() << QStringLiteral("  实际并行连接数: %1")
                             .arg(uploader.activeTransferConnections());
    return elapsed;
}

// 多文件并发上传：改造前所有文件的写入都排在同一把 sessionLock 上严格串行，
// 这是并行收益最直接的场景。返回总耗时。
static qint64 runMultiFileUpload(SshClient *client, const QStringList &localPaths,
                                 const QString &base, int connections,
                                 const QString &metaDir)
{
    SftpUploaderCore uploader(client);
    uploader.setMetadataDir(metaDir);
    uploader.setMaxTransferConnections(connections);
    uploader.prewarmConnections();
    for (int i = 0; i < 100 && uploader.activeTransferConnections() < connections; ++i) {
        QEventLoop warm;
        QTimer::singleShot(50, &warm, &QEventLoop::quit);
        warm.exec();
    }

    int remaining = localPaths.size();
    QString failure;
    QObject::connect(&uploader, &SftpUploaderCore::uploadCompleted,
                     [&remaining](const QString &, const QString &) { --remaining; });
    QObject::connect(&uploader, &SftpUploaderCore::uploadFailed,
                     [&remaining, &failure](const QString &, const QString &, const QString &e) {
                         failure = e;
                         --remaining;
                     });

    QElapsedTimer timer;
    timer.start();
    for (const QString &local : localPaths) {
        const QString remote = base + QStringLiteral("/mf_%1_%2.bin")
                                          .arg(connections).arg(QFileInfo(local).fileName());
        uploader.uploadFile(remote, local, remote);
    }
    while (remaining > 0 && timer.elapsed() < 600000) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const qint64 elapsed = timer.elapsed();
    uploader.waitForFinished(10000);
    if (!failure.isEmpty()) {
        qWarning() << "  multi-file upload FAILED:" << failure;
        return -1;
    }
    return elapsed;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    const QString base = qEnvironmentVariable("CUBESSH_REMOTE_BASE", "/home/testuser/sftp_dir");
    const qint64 sizeMb = qEnvironmentVariable("CUBESSH_BENCH_MB", "64").toLongLong();

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "connect failed:" << err.message;
        return 2;
    }
    qInfo() << "connected;" << "可克隆连接(认证可重放):" << client.isAuthReplayable();

    QTemporaryDir tmp;
    const QString localPath = QDir(tmp.path()).filePath(QStringLiteral("bench.bin"));
    const qint64 total = sizeMb * 1024 * 1024;
    if (!writeTestFile(localPath, total)) {
        qWarning() << "cannot create local test file";
        return 3;
    }
    qInfo().noquote() << QStringLiteral("测试文件: %1 MB").arg(sizeMb);
    qInfo().noquote() << QStringLiteral("本地 MD5: %1  <- 与远端 md5sum bench_*.bin 逐一比对")
                             .arg(localMd5(localPath));

    struct Result { int conns; qint64 ms; };
    QVector<Result> results;

    for (int conns : {1, 2, 4}) {
        const QString remotePath = base + QStringLiteral("/bench_%1.bin").arg(conns);
        const QString metaDir = QDir(tmp.path()).filePath(QStringLiteral("meta_%1").arg(conns));
        qInfo().noquote() << QStringLiteral("\n=== %1 条传输连接 ===").arg(conns);
        const qint64 ms = runUpload(&client, localPath, remotePath, conns, metaDir);
        if (ms < 0)
            return 1;
        const double mbps = double(sizeMb) / (double(ms) / 1000.0);
        qInfo().noquote() << QStringLiteral("  耗时 %1 ms  吞吐 %2 MB/s")
                                 .arg(ms).arg(mbps, 0, 'f', 1);
        results.append({conns, ms});
    }

    qInfo().noquote() << QStringLiteral("\n=== 汇总（%1 MB 单文件）===").arg(sizeMb);
    const qint64 baseline = results.first().ms;
    for (const Result &r : results) {
        qInfo().noquote() << QStringLiteral("  %1 连接: %2 ms  相对串行 %3x")
                                 .arg(r.conns).arg(r.ms)
                                 .arg(double(baseline) / double(r.ms), 0, 'f', 2);
    }

    // --- 多文件并发场景 ---
    const int fileCount = 6;
    const qint64 eachMb = qMax<qint64>(1, sizeMb / 4);
    QStringList multi;
    for (int i = 0; i < fileCount; ++i) {
        const QString p = QDir(tmp.path()).filePath(QStringLiteral("mf_%1.bin").arg(i));
        if (!writeTestFile(p, eachMb * 1024 * 1024))
            return 3;
        multi.append(p);
    }
    qInfo().noquote() << QStringLiteral("\n=== 多文件并发：%1 个 × %2 MB ===")
                             .arg(fileCount).arg(eachMb);

    QVector<Result> multiResults;
    for (int conns : {1, 4}) {
        const QString metaDir = QDir(tmp.path()).filePath(QStringLiteral("mfmeta_%1").arg(conns));
        const qint64 ms = runMultiFileUpload(&client, multi, base, conns, metaDir);
        if (ms < 0)
            return 1;
        const double mbps = double(eachMb * fileCount) / (double(ms) / 1000.0);
        qInfo().noquote() << QStringLiteral("  %1 连接: %2 ms  吞吐 %3 MB/s")
                                 .arg(conns).arg(ms).arg(mbps, 0, 'f', 1);
        multiResults.append({conns, ms});
    }
    qInfo().noquote() << QStringLiteral("  多文件提速: %1x")
                             .arg(double(multiResults.first().ms) / double(multiResults.last().ms),
                                  0, 'f', 2);
    return 0;
}
