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

        // 现场清理（best-effort）
        for (const QString &name : dlNames)
            sftp.remove(base + QStringLiteral("/sftp_dir/") + name, &err);
        sftp.remove(base + QStringLiteral("/sftp_dir/dl_big.bin"), &err);
    }

    qInfo() << (failures == 0 ? "SFTP ALL PASS" : "SFTP FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
