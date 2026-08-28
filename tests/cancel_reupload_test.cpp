// cancel_reupload_test.cpp — 取消上传后立即重传不能卡死调用线程。
//
// 复现用户报的现象：上传大文件时点取消，紧接着再次上传，UI 直接卡死。
// UI 卡死的机理是主线程（UI）在 refreshUploadProgress 里调
// activeTransferConnections() 要抢连接池 m_lock，而某个工作线程把 m_lock
// 无限占住（取消打断 socket 后连接池清理/重建路径上的无界等待）。
// 本测试在主线程模拟这个动作：取消 → 立即重传 → 主线程反复抢 m_lock。
//
// 需要活的 sshd：CUBESSH_HOST/PORT/USER/PASS, CUBESSH_REMOTE_BASE。
// CUBESSH_JUMP=1 时走跳板链 b1 → target（CUBESSH_B1_PORT 默认 2311）。
//
// 已知：跳板链多流并行传输存在一个**既有**的间歇性死锁（共享连接的阻塞写 +
// 中继背压互相等待），与取消功能无关（去掉取消打断也复现）。它偶发时表现为
// 某一轮重传停在原地、最终超时 FAIL —— 那是这个既有问题，不是本测试要防的
// 「UI 线程被 m_lock 钉死」。区分方法：UI 冻结时主线程连 "…等待完成" 都打不出来。

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
#include "config/GlobalState.h"
#include "net/ProxyConfig.h"

using namespace cubeshell;

// 与 proxy_integration_test 同构的跳板名录：b1 由本机直拨，target 经 b1 跳到。
static void publishJumpCatalog()
{
    DeviceEntry b1;
    b1.id       = QStringLiteral("b1");
    b1.name     = QStringLiteral("跳板一号");
    b1.host     = QStringLiteral("127.0.0.1");
    b1.port     = quint16(qEnvironmentVariable("CUBESSH_B1_PORT", "2311").toUShort());
    b1.username = QStringLiteral("testuser");
    b1.password = QStringLiteral("testpass123");
    b1.protocol = QStringLiteral("ssh");
    GlobalState::instance().setJumpHostCatalog({b1});
}

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

// 重传必须在此时限内完成。给足余量是为了覆盖跳板中继的低吞吐——本测试测的是
// 「取消+重传不死锁」，不是吞吐。真正的卡死是永久 hang，由外层 timeout 兜底判定。
static constexpr qint64 kMaxReuploadMs = 60000;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2301").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    const QString base = qEnvironmentVariable("CUBESSH_REMOTE_BASE", "/home/testuser/sftp_dir");

    SshClient client;
    client.setUsername(user);
    client.setPassword(pass);
    // CUBESSH_JUMP=1 时走跳板链 b1 → target：内网名 "target" 交给 b1 解析。
    if (qEnvironmentVariableIsSet("CUBESSH_JUMP")) {
        publishJumpCatalog();
        ProxyConfig jump;
        jump.type   = ProxyType::JumpHost;
        jump.hopIds = {QStringLiteral("b1")};
        client.setHost(QStringLiteral("target"), 22);
        client.setProxyConfig(jump, {});
        client.setConnectTimeoutMs(15000);
        qInfo().noquote() << "  走跳板链 b1 → target";
    } else {
        client.setHost(host, port);
    }
    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "SKIP: connect failed:" << err.message;
        return 0;
    }

    QTemporaryDir tmp;
    const QString localPath = QDir(tmp.path()).filePath(QStringLiteral("cancel.bin"));
    if (!writeTestFile(localPath, 48)) {
        qWarning() << "cannot create local test file";
        return 3;
    }

    auto *up = new SftpUploaderCore(&client);
    up->setMetadataDir(QDir(tmp.path()).filePath(QStringLiteral("m")));
    up->setMaxTransferConnections(4);
    up->prewarmConnections();
    for (int i = 0; i < 40 && up->activeTransferConnections() < 4; ++i)
        spin(50);

    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
    std::atomic<qint64> lastDone{0};
    std::atomic<qint64> lastTotal{0};
    QObject::connect(up, &SftpUploaderCore::progressChanged,
                     [&started, &lastDone, &lastTotal](const QString &, qint64 d, qint64 t) {
                         started = true;
                         lastDone = d;
                         lastTotal = t;
                     });
    QObject::connect(up, &SftpUploaderCore::uploadCompleted,
                     [&done](const QString &, const QString &) { done = true; });
    QObject::connect(up, &SftpUploaderCore::uploadFailed,
                     [&cancelled](const QString &, const QString &, const QString &e) {
                         qInfo().noquote() << "  (uploadFailed)" << e;
                         if (e.contains(QStringLiteral("已取消")))
                             cancelled = true;
                     });

    // 真实用户流程：上传 → 取消 →【等取消收敛（收到"已取消"）】→ 再重传。
    // 取消收敛这一步直接验证问题1（取消不能卡在"正在上传"）；主线程全程反复抢
    // m_lock（activeTransferConnections，即 refreshUploadProgress 在 UI 干的事），
    // 连接池一旦被工作线程无限占住 m_lock，主线程卡死（外层 timeout 判定 UI 冻结）。
    for (int round = 0; round < 6; ++round) {
        qInfo().noquote() << QStringLiteral("\n=== 第 %1 轮 ===").arg(round + 1);
        started = false;
        done = false;
        cancelled = false;

        up->uploadFile(QStringLiteral("f"), localPath, base + QStringLiteral("/cancel.bin"));
        for (int i = 0; i < 100 && !started; ++i)
            spin(20);
        check(started, QStringLiteral("第 %1 轮上传已开始").arg(round + 1));

        // 取消，并等它收敛（旧工作线程发出"已取消"）。这一步卡住 = 问题1 复发。
        up->cancelUpload(QStringLiteral("f"));
        QElapsedTimer ct;
        ct.start();
        while (!cancelled && ct.elapsed() < kMaxReuploadMs) {
            (void)up->activeTransferConnections(); // 主线程抢 m_lock，侦测 UI 冻结
            spin(20);
        }
        check(cancelled, QStringLiteral("第 %1 轮取消在 %2ms 内收敛（不卡在正在上传）")
                             .arg(round + 1).arg(ct.elapsed()));
        if (!cancelled)
            break;

        // 取消成功后重传。
        qInfo().noquote() << "  重传…";
        up->uploadFile(QStringLiteral("f"), localPath, base + QStringLiteral("/cancel.bin"));

        QElapsedTimer t;
        t.start();
        qint64 lastLog = 0;
        while (!done && t.elapsed() < kMaxReuploadMs) {
            const int conns = up->activeTransferConnections();
            if (t.elapsed() - lastLog > 3000) {
                qInfo().noquote() << QStringLiteral("  …等待完成 %1ms (连接数 %2, 进度 %3/%4)")
                                         .arg(t.elapsed()).arg(conns)
                                         .arg(lastDone.load()).arg(lastTotal.load());
                lastLog = t.elapsed();
            }
            spin(20);
        }
        qInfo().noquote() << QStringLiteral("  第 %1 轮重传等待 %2 ms, done=%3")
                                 .arg(round + 1).arg(t.elapsed()).arg(done.load());
        check(done, QStringLiteral("第 %1 轮取消后重传在 %2ms 内完成（未卡死）")
                        .arg(round + 1).arg(kMaxReuploadMs));
        if (!done)
            break;
    }

    delete up;
    qInfo().noquote() << (g_failures == 0 ? "\nCANCEL-REUPLOAD ALL PASS"
                                          : QStringLiteral("\nCANCEL-REUPLOAD FAILURES %1").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
