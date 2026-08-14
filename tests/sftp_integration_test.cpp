// SFTP integration test (headless): connects to a live sshd, then exercises
// SftpClient listdir / stat / readFile / writeFile / mkdir / rename / remove.
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS.

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include "ssh/SshClient.h"
#include "ssh/SftpClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "connect failed:" << err.message;
        return 2;
    }
    qInfo() << "connected";

    // NOTE: no shell channel is opened here, so the session is still in blocking
    // mode from connectToHost — ideal for synchronous SFTP ops.
    SftpClient sftp(&client);
    if (!sftp.open(err)) {
        qWarning() << "sftp open failed:" << err.message;
        return 3;
    }
    qInfo() << "sftp open";

    const QString base = QStringLiteral("/home/testuser");

    // listdir
    const QStringList names = sftp.listdir(base, err);
    qInfo() << "listdir:" << names;
    CHECK(names.contains(QStringLiteral("sftp_dir")));

    // stat a directory + a file
    SftpFileInfo info;
    CHECK(sftp.stat(base + QStringLiteral("/sftp_dir"), info, err));
    CHECK(info.isDirectory());
    CHECK(sftp.stat(base + QStringLiteral("/sftp_dir/hello.txt"), info, err));
    CHECK(info.isRegular());

    // readFile
    const QByteArray content = sftp.readFile(base + QStringLiteral("/sftp_dir/hello.txt"), err);
    qInfo() << "readFile:" << content;
    CHECK(content.contains("hello cubeshell"));

    // writeFile + read back
    CHECK(sftp.writeFile(base + QStringLiteral("/sftp_dir/written.txt"), "cubeshell wrote this\n", err));
    CHECK(sftp.readFile(base + QStringLiteral("/sftp_dir/written.txt"), err).contains("cubeshell wrote this"));

    // mkdir + rename + remove
    CHECK(sftp.mkdir(base + QStringLiteral("/sftp_dir/newdir"), 0755, &err));
    CHECK(sftp.exists(base + QStringLiteral("/sftp_dir/newdir")));
    CHECK(sftp.rename(base + QStringLiteral("/sftp_dir/written.txt"),
                      base + QStringLiteral("/sftp_dir/renamed.txt"), &err));
    CHECK(sftp.exists(base + QStringLiteral("/sftp_dir/renamed.txt")));
    CHECK(sftp.remove(base + QStringLiteral("/sftp_dir/renamed.txt"), &err));
    CHECK(!sftp.exists(base + QStringLiteral("/sftp_dir/renamed.txt")));
    CHECK(sftp.rmdir(base + QStringLiteral("/sftp_dir/newdir"), &err));

    // recursive listing attr on nested dir
    const SftpFileInfoList sub = sftp.listdirAttr(base + QStringLiteral("/sftp_dir/subdir"), err);
    bool foundNested = false;
    for (const SftpFileInfo &e : sub)
        if (e.filename == QStringLiteral("nested.txt")) foundNested = true;
    CHECK(foundNested);

    // --- 异步下载：串行多文件（模拟 UI 批量队列），逐字节校验 -----------------
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QStringList dlNames = {QStringLiteral("dl_a.txt"), QStringLiteral("dl_b.txt"),
                                 QStringLiteral("dl_c.txt")};
    for (const QString &name : dlNames) {
        const QByteArray payload = ("payload-" + name + "\n").toUtf8().repeated(100);
        CHECK(sftp.writeFile(base + QStringLiteral("/sftp_dir/") + name, payload, err));
        bool ok = false;
        QString msg;
        QEventLoop loop;
        QObject::connect(&sftp, &SftpClient::transferFinished, &loop,
                         [&loop, &ok, &msg](const QString &, bool success, const QString &m) {
            ok = success;
            msg = m;
            loop.quit();
        });
        sftp.download(base + QStringLiteral("/sftp_dir/") + name,
                      tmp.path() + QLatin1Char('/') + name);
        loop.exec();
        if (!ok)
            qWarning() << "download failed:" << name << msg;
        CHECK(ok);
        QFile f(tmp.path() + QLatin1Char('/') + name);
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == payload);
    }

    // --- 取消-重启回归：钉住 "read failed at offset 0" 竞态 ------------------
    // 下载中收到首个进度立刻 cancelTransfer()：A 必须以 "cancelled"（或竞态完成）
    // 收尾，绝不能冒出 "read failed at offset 0" 的假失败；随后重启的下载必须成功。
    {
        const QByteArray big = QByteArray(1024 * 1024, 'x').repeated(32); // 32MB
        CHECK(sftp.writeFile(base + QStringLiteral("/sftp_dir/dl_big.bin"), big, err));

        bool finishOk = false, progressed = false;
        QString finishMsg;
        QEventLoop loop;
        QObject::connect(&sftp, &SftpClient::transferProgress, &loop,
                         [&sftp, &progressed](const QString &, qint64, qint64) {
            if (!progressed) {
                progressed = true;
                sftp.cancelTransfer();
            }
        });
        QObject::connect(&sftp, &SftpClient::transferFinished, &loop,
                         [&loop, &finishOk, &finishMsg](const QString &, bool success,
                                                        const QString &m) {
            finishOk = success;
            finishMsg = m;
            loop.quit();
        });
        sftp.download(base + QStringLiteral("/sftp_dir/dl_big.bin"),
                      tmp.path() + QStringLiteral("/dl_big.bin"));
        loop.exec();
        qInfo() << "cancel case: progressed" << progressed << "ok" << finishOk
                << "msg" << finishMsg;
        // 核心断言：取消哨兵不得以读错误的形式冒出（本次修的竞态）。
        CHECK(!finishMsg.contains(QStringLiteral("read failed at offset")));

        // 立刻重启另一个下载。download() 入口只在无在传 worker 时才复位 cancel，
        // 这里等一拍让 A 的 worker 收尾（真实用户操作间隔远大于此）。
        QEventLoop waitLoop;
        QTimer::singleShot(500, &waitLoop, &QEventLoop::quit);
        waitLoop.exec();

        bool ok2 = false;
        QString msg2;
        QEventLoop loop2;
        QObject::connect(&sftp, &SftpClient::transferFinished, &loop2,
                         [&loop2, &ok2, &msg2](const QString &, bool success,
                                               const QString &m) {
            ok2 = success;
            msg2 = m;
            loop2.quit();
        });
        sftp.download(base + QStringLiteral("/sftp_dir/dl_a.txt"),
                      tmp.path() + QStringLiteral("/dl_restart.txt"));
        loop2.exec();
        if (!ok2)
            qWarning() << "restart download failed:" << msg2;
        CHECK(ok2);

        // 续传正确性：取消留下的 .part + 位图必须续传出完整内容。
        // 钉住的是这条真实故障：预分配让残件尺寸等于远端大小，重下直接
        // "already downloaded" 跳过，用户拿到一个中间是空洞的全尺寸坏文件。
        // （若竞态下首次已下完，则走 "already downloaded" 分支，内容校验同样过。）
        bool ok3 = false;
        QString msg3;
        QEventLoop loop3;
        QObject::connect(&sftp, &SftpClient::transferFinished, &loop3,
                         [&loop3, &ok3, &msg3](const QString &, bool success,
                                               const QString &m) {
            ok3 = success;
            msg3 = m;
            loop3.quit();
        });
        sftp.download(base + QStringLiteral("/sftp_dir/dl_big.bin"),
                      tmp.path() + QStringLiteral("/dl_big.bin"));
        loop3.exec();
        if (!ok3)
            qWarning() << "resume download failed:" << msg3;
        CHECK(ok3);
        QFile bigFile(tmp.path() + QStringLiteral("/dl_big.bin"));
        CHECK(bigFile.open(QIODevice::ReadOnly) && bigFile.readAll() == big);
        // 完工后临时文件与位图必须已清掉
        CHECK(!QFile::exists(tmp.path() + QStringLiteral("/dl_big.bin.part")));
        CHECK(!QFile::exists(tmp.path() + QStringLiteral("/dl_big.bin.part.meta")));

        // 现场清理（best-effort）
        for (const QString &name : dlNames)
            sftp.remove(base + QStringLiteral("/sftp_dir/") + name, &err);
        sftp.remove(base + QStringLiteral("/sftp_dir/dl_big.bin"), &err);
    }

    // --- 析构安全回归：下载进行中销毁 SftpClient 不得崩溃/冻住 ---------------
    // 钉住关标签页路径：~SftpClient 的 cancelTransfer + join 必须在 worker 仍
    // 持有裸 this 时安全收敛（超时则走连接池泄漏兜底，绝不能 UAF）。
    {
        const QByteArray big2 = QByteArray(1024 * 1024, 'y').repeated(64); // 64MB
        CHECK(sftp.writeFile(base + QStringLiteral("/sftp_dir/dl_teardown.bin"), big2, err));

        auto *sftp2 = new SftpClient(&client); // 无父对象，手动销毁
        SshError err2;
        CHECK(sftp2->open(err2));
        QEventLoop loop;
        QObject::connect(sftp2, &SftpClient::transferProgress, &loop,
                         [&loop, &sftp2](const QString &, qint64, qint64) {
            // 首个进度到达 = 传输已在跑：直接销毁（与关标签页同款路径）。
            // sender 的信号投递是 queued，此处 emission 已返回，delete 安全；
            // 进程不崩、不冻住即通过。
            delete sftp2;
            sftp2 = nullptr;
            loop.quit();
        });
        sftp2->download(base + QStringLiteral("/sftp_dir/dl_teardown.bin"),
                        tmp.path() + QStringLiteral("/dl_teardown.bin"));
        // localhost 上 64MB 可能秒完、竞态下进度信号先到 finished：兜底退出。
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        delete sftp2; // 竞态下已完成、没走进度回调时在这里销毁
        sftp.remove(base + QStringLiteral("/sftp_dir/dl_teardown.bin"), &err);
        qInfo() << "teardown-during-download survived";
    }

    qInfo() << (failures == 0 ? "SFTP ALL PASS" : "SFTP FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
