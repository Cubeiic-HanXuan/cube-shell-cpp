// upload_teardown_test.cpp — 上传进行中销毁上传器不能卡住调用线程。
//
// 复现的是用户报的两个现象：上传时关闭标签页程序卡死、退出程序卡在程序坞。
// 两者都是同一类故障：~SftpUploaderCore 跑在 UI 线程上，如果它无限等工作线程
// 或无限等 sessionLock，UI 线程就被钉死，窗口冻结、进程不退出。
//
// 场景 2 是关键：终端读循环/远程监控线程持有 sessionLock 并阻塞在网络 IO 上，
// 这是关闭标签页时的常态（SshClient.cpp 里 kCloseLockTimeoutMs 那段注释记录了
// 同款故障的 gdb 现场）。析构路径绝不能无条件抢这把锁。
//
// 需要活的 sshd：CUBESSH_HOST/PORT/USER/PASS, CUBESSH_REMOTE_BASE。

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>

#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int g_failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok) {
        qInfo().noquote() << "  PASS:" << what;
    } else {
        qWarning().noquote() << "  FAIL:" << what;
        ++g_failures;
    }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool writeTestFile(const QString &path, qint64 sizeMb)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QByteArray block(1024 * 1024, 'z');
    for (qint64 i = 0; i < sizeMb; ++i)
        f.write(block);
    return true;
}

// 析构必须在此时限内返回。工作线程一旦看到取消标志就该在当前分片边界退出，
// 正常情况远小于此值；超过就说明有无界等待。
static constexpr qint64 kMaxTeardownMs = 8000;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    const QString base = qEnvironmentVariable("CUBESSH_REMOTE_BASE", "/home/testuser/sftp_dir");

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);
    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "SKIP: connect failed:" << err.message;
        return 0;
    }

    QTemporaryDir tmp;
    const QString localPath = QDir(tmp.path()).filePath(QStringLiteral("teardown.bin"));
    if (!writeTestFile(localPath, 160)) {
        qWarning() << "cannot create local test file";
        return 3;
    }

    // --- 场景 1：多连接并行传输进行中直接析构（关闭标签页） ---
    qInfo().noquote() << "\n=== 场景1: 并行传输中析构上传器 ===";
    {
        auto *up = new SftpUploaderCore(&client);
        up->setMetadataDir(QDir(tmp.path()).filePath(QStringLiteral("m1")));
        up->setMaxTransferConnections(4);
        up->prewarmConnections();
        for (int i = 0; i < 40 && up->activeTransferConnections() < 4; ++i)
            spin(50);

        std::atomic<bool> started{false};
        QObject::connect(up, &SftpUploaderCore::progressChanged,
                         [&started](const QString &, qint64, qint64) { started = true; });
        up->uploadFile(QStringLiteral("t1"), localPath,
                       base + QStringLiteral("/teardown1.bin"));
        for (int i = 0; i < 100 && !started; ++i)
            spin(50);
        check(started, QStringLiteral("传输已真正开始（否则本场景没测到东西）"));

        QElapsedTimer t;
        t.start();
        delete up;
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QStringLiteral("  析构耗时 %1 ms").arg(ms);
        check(ms < kMaxTeardownMs,
              QStringLiteral("析构在 %1ms 内返回（实测 %2ms）").arg(kMaxTeardownMs).arg(ms));
    }

    // --- 场景 2：降级到主连接 + 另一线程占着 sessionLock 时析构 ---
    // maxTransferConnections=1 强制走主连接，让池在主 session 上开出 SFTP 通道；
    // 同时另起一个线程长时间持有 sessionLock，模拟终端读循环阻塞在网络 IO 上。
    qInfo().noquote() << "\n=== 场景2: 降级到主连接 + sessionLock 被他人持有 ===";
    {
        auto *up = new SftpUploaderCore(&client);
        up->setMetadataDir(QDir(tmp.path()).filePath(QStringLiteral("m2")));
        up->setMaxTransferConnections(1); // 强制降级：使用主连接
        std::atomic<bool> started{false};
        QObject::connect(up, &SftpUploaderCore::progressChanged,
                         [&started](const QString &, qint64, qint64) { started = true; });
        up->uploadFile(QStringLiteral("t2"), localPath,
                       base + QStringLiteral("/teardown2.bin"));
        for (int i = 0; i < 100 && !started; ++i)
            spin(50);
        check(started, QStringLiteral("降级路径传输已开始"));

        // 抢占 sessionLock 并保持 12 秒（> kMaxTeardownMs），模拟终端读循环。
        std::atomic<bool> holding{false};
        QThread *hog = QThread::create([&client, &holding] {
            QMutexLocker<QRecursiveMutex> guard(&client.sessionLock());
            holding = true;
            QThread::msleep(12000);
        });
        hog->start();
        for (int i = 0; i < 100 && !holding; ++i)
            spin(20);
        check(holding, QStringLiteral("模拟线程已持有 sessionLock"));

        QElapsedTimer t;
        t.start();
        delete up;
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QStringLiteral("  析构耗时 %1 ms").arg(ms);
        check(ms < kMaxTeardownMs,
              QStringLiteral("sessionLock 被占时析构仍在 %1ms 内返回（实测 %2ms）")
                  .arg(kMaxTeardownMs).arg(ms));

        hog->wait();
        delete hog;
    }

    qInfo().noquote() << (g_failures == 0 ? "\nTEARDOWN ALL PASS"
                                         : QStringLiteral("\nTEARDOWN FAILURES %1").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
