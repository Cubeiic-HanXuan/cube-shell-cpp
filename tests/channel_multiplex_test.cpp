// channel_multiplex_test.cpp — 主 session SFTP 通道多路复用（jms 一次性
// token / MFA 场景的并行传输路径）的确定性集成测试。
//
// 为什么单独一个测试而不是复用 uploader_test：uploader_test 的联网段走
// 密码认证 → 凭据可重放 → 连接池克隆独立连接，永远覆盖不到通道路径。
// 本测试用 SftpUploaderCore::setCloningEnabled(false) 强制走主 session
// 多通道复用，并用 openShell 把 session 切到非阻塞（与真实应用状态一致：
// 终端 shell 与多条 SFTP 通道并存于一条 SSH 连接上）。
//
// 覆盖点：
//  1) 禁克隆后 canParallelize() 仍为 true（通道多路复用可用），且传输槽 >1。
//  2) 单个大文件多流并行上传（≥8MB 触发拆流）：完成、进度单调、远端字节
//     与本地逐一相等（乱序分片写入的正确性）。
//  3) 批量多文件并行上传：全部完成、字节逐一相等。
//  4) 锁纪律回归：上传进行中 sessionLock 必须在有界时间内可获取
//    （旧实现持锁跨整个 4MB 分片 + EAGAIN 空等，jms/MFA 会话因此卡 UI）。
//
// 依赖活的 sshd（默认 127.0.0.1:2222，testuser/testpass123）；连不上 exit 2
// 记 SKIP。Env: CUBESSH_HOST/PORT/USER/PASS/REMOTE_BASE（与 uploader_test 一致）。

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

#include "ssh/SftpClient.h"
#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void spin(int msecs)
{
    QEventLoop loop;
    QTimer::singleShot(msecs, &loop, &QEventLoop::quit);
    loop.exec();
}

// 与 uploader_test 相同的可预测内容生成（pattern 仅依赖偏移）。
static bool writeTestFile(const QString &path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const int blockSize = 64 * 1024;
    QByteArray block(blockSize, Qt::Uninitialized);
    for (int i = 0; i < blockSize; ++i)
        block[i] = char('A' + (i % 26));
    qint64 written = 0;
    while (written < size) {
        const qint64 n = qMin<qint64>(blockSize, size - written);
        if (f.write(block.constData(), n) != n)
            return false;
        written += n;
    }
    f.close();
    return true;
}

static QByteArray sha256Of(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// 收集一次上传（单文件或批量）的信号簿记。
struct UploadWatch {
    QSet<QString> completed;
    QStringList failures;
    QHash<QString, qint64> lastDone; // fileId -> 最后报告的 done（查单调性）
    bool monotonic = true;

    void wire(SftpUploaderCore &uploader)
    {
        QObject::connect(&uploader, &SftpUploaderCore::progressChanged,
                         [this](const QString &fileId, qint64 done, qint64) {
                             if (done < lastDone.value(fileId))
                                 monotonic = false;
                             lastDone[fileId] = done;
                         });
        QObject::connect(&uploader, &SftpUploaderCore::uploadCompleted,
                         [this](const QString &fileId, const QString &) {
                             completed.insert(fileId);
                         });
        QObject::connect(&uploader, &SftpUploaderCore::uploadFailed,
                         [this](const QString &fileId, const QString &,
                                const QString &error) {
                             failures << fileId + ":" + error;
                         });
    }
};

// 校验远端文件大小与内容（内容经 SftpClient::readFile 读回比对哈希）。
static void checkRemote(SftpClient &sftp, const QString &remotePath, qint64 expectSize,
                        const QByteArray &expectHash)
{
    SshError err;
    SftpFileInfo info;
    CHECK(sftp.stat(remotePath, info, err));
    CHECK(info.size == expectSize);
    const QByteArray remote = sftp.readFile(remotePath, err);
    CHECK(remote.size() == expectSize);
    CHECK(sha256Of(remote) == expectHash);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    const QString base = qEnvironmentVariable(
        "CUBESSH_REMOTE_BASE", QStringLiteral("/home/testuser/sftp_dir"));

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "connect failed (channel multiplex tests SKIPPED):" << err.message;
        return 2;
    }
    qInfo() << "connected";

    // 打开 shell：session 转非阻塞（与真实应用状态一致 —— 终端与多条 SFTP
    // 通道并存于一条 SSH 连接上）。不读它：shell 输出仅一个 prompt，远填不满
    // 通道窗口；本测试也不往 shell 写（避免窗口背压干扰锁探针）。
    if (!client.openShell("xterm", 120, 40, err)) {
        qWarning() << "openShell failed (SKIPPED):" << err.message;
        return 2;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qWarning() << "cannot create temp dir";
        return 1;
    }

    SftpUploaderCore uploader(&client);
    uploader.setMetadataDir(tmp.filePath(QStringLiteral("meta")));
    // 强制走主 session 通道多路复用（密码认证默认会克隆独立连接，覆盖不到
    // 这条路径 —— 这正是本测试存在的意义）。
    uploader.setCloningEnabled(false);
    int maxConn = 8; // 足够每条流独占通道，先排除共享干扰
    if (qEnvironmentVariableIsSet("CUBESSH_MUX_MAXCONN"))
        maxConn = qEnvironmentVariableIntValue("CUBESSH_MUX_MAXCONN"); // 压共享路径用
    uploader.setMaxTransferConnections(maxConn);
    // maxConnections==1 是刻意的串行降级：不可并行、连接数就是 1，相关并行断言
    // 只在并行配置下才成立（maxconn=1 时跳过，只验证功能正确、不验证加速）。
    const bool parallelCfg = maxConn > 1;
    if (parallelCfg)
        CHECK(uploader.canParallelize()); // 禁克隆后通道复用仍应支持并行

    // ------------------------------------------------------------------
    // 场景 1：单个大文件多流并行（24MB = 6 分片 → 最多 4 条流）
    // ------------------------------------------------------------------
    const qint64 bigSize = 24 * 1024 * 1024;
    const QString bigLocal = tmp.filePath(QStringLiteral("big_24m.bin"));
    const QString bigRemote = base + QStringLiteral("/mux_big_24m.bin");
    CHECK(writeTestFile(bigLocal, bigSize));
    const QByteArray bigHash = sha256Of([&] {
        QFile f(bigLocal);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }());

    UploadWatch watch1;
    watch1.wire(uploader);

    // 锁纪律探针：上传进行中主线程反复 tryLock sessionLock，记录最长等待。
    // 旧纪律（持锁跨整片 + EAGAIN 锁内空等）下这里会轻易破秒 —— 即 jms/MFA
    // 会话卡 UI 的直接原因；新纪律（逐调用加锁）下应在毫秒级。
    QElapsedTimer probeTimer;
    QElapsedTimer uploadTimer;
    uploadTimer.start();
    qint64 maxLockWaitMs = 0;
    const QString bigId = QStringLiteral("mux_big");
    uploader.uploadFile(bigId, bigLocal, bigRemote);
    while (!watch1.completed.contains(bigId) && watch1.failures.isEmpty()
           && uploadTimer.elapsed() < 150000) {
        probeTimer.start();
        if (client.sessionLock().tryLock(2000)) {
            maxLockWaitMs = qMax(maxLockWaitMs, probeTimer.elapsed());
            client.sessionLock().unlock();
        } else {
            maxLockWaitMs = qMax(maxLockWaitMs, qint64(2000));
            break; // 2 秒拿不到锁：直接记失败级别的等待
        }
        spin(20);
    }
    CHECK(uploader.waitForFinished(180000));
    spin(300);

    qInfo() << "scenario1: maxLockWaitMs =" << maxLockWaitMs
            << " transferConns =" << uploader.activeTransferConnections()
            << " failures =" << watch1.failures;
    CHECK(watch1.failures.isEmpty());
    CHECK(watch1.completed.contains(bigId));
    CHECK(watch1.monotonic);
    if (parallelCfg)
        CHECK(uploader.activeTransferConnections() > 1); // 确实用上了多条通道
    CHECK(maxLockWaitMs < 2000);                     // 锁必须在有界时间内可得

    SftpClient sftp(&client);
    CHECK(sftp.open(err));
    checkRemote(sftp, bigRemote, bigSize, bigHash);

    // ------------------------------------------------------------------
    // 场景 2：批量多文件并行（5 x 3MB，单文件不拆流，多文件各租一条通道）
    // ------------------------------------------------------------------
    QHash<QString, QPair<QString, QString>> mappings;
    QHash<QString, QByteArray> hashes;
    for (int i = 0; i < 5; ++i) {
        const QString name = QStringLiteral("mux_batch_%1.bin").arg(i);
        const QString local = tmp.filePath(name);
        const qint64 size = 3 * 1024 * 1024 + i * 12345; // 大小不一
        CHECK(writeTestFile(local, size));
        QFile f(local);
        f.open(QIODevice::ReadOnly);
        hashes[name] = sha256Of(f.readAll());
        mappings.insert(name, {local, base + QStringLiteral("/") + name});
    }

    UploadWatch watch2;
    watch2.wire(uploader);
    uploader.batchUpload(mappings);
    CHECK(uploader.waitForFinished(180000));
    spin(300);

    qInfo() << "scenario2: completed =" << watch2.completed.size()
            << " failures =" << watch2.failures;
    CHECK(watch2.failures.isEmpty());
    CHECK(watch2.completed.size() == mappings.size());
    CHECK(watch2.monotonic);
    for (auto it = mappings.begin(); it != mappings.end(); ++it) {
        const qint64 size = QFileInfo(it.value().first).size();
        checkRemote(sftp, it.value().second, size, hashes.value(it.key()));
        sftp.remove(it.value().second, &err); // 顺手清理
    }
    sftp.remove(bigRemote, &err);

    qInfo() << (failures == 0 ? "CHANNEL MULTIPLEX ALL PASS" : "CHANNEL MULTIPLEX FAILURES")
            << failures;
    return failures == 0 ? 0 : 1;
}
