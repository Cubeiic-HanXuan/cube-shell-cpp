// SFTP integration test (headless): connects to a live sshd, then exercises
// SftpClient listdir / stat / readFile / writeFile / mkdir / rename / remove.
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS.

#include <QCoreApplication>
#include <QDebug>

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

    qInfo() << (failures == 0 ? "SFTP ALL PASS" : "SFTP FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
