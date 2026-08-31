// ssh_key_copyid_test.cpp — SSH 密钥管理功能的端到端集成测试（headless，需真实 sshd）。
//
// 对默认 127.0.0.1:2401 的 docker sshd（tests/docker/ssh-enhance，testuser/testpass123）
// 跑完整闭环：
//   1) SshKeyStore 生成一把 Ed25519 + 一把 RSA 密钥（落临时目录）
//   2) SshCopyIdWorker 用**密码**连接并部署公钥（幂等：连发两次，第二次应报已存在）
//   3) 用**生成的私钥**经 SshClient(libssh2) 连回去跑 whoami —— 同时证明
//      libssh2 能读我们写出的私钥格式、且 copy-id 把公钥装对了
//
// 环境变量覆盖：CUBESSH_HOST / CUBESSH_PORT / CUBESSH_USER / CUBESSH_PASS。
// 不是单元测试——没有可达 sshd 时会整体失败，故默认不随 ctest 跑（见 CMakeLists 注释）。

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QThread>
#include <QTemporaryDir>

#include "config/DeviceConfigStore.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SshClient.h"
#include "ssh/SshCopyIdWorker.h"
#include "ssh/SshKeyStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static DeviceEntry makePasswordDevice()
{
    DeviceEntry dev;
    dev.name = QStringLiteral("copyid-target");
    dev.host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    dev.port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2401").toUShort());
    dev.username = qEnvironmentVariable("CUBESSH_USER", "testuser");
    dev.password = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    dev.credentialKind = SshCredentialKind::Password;
    return dev;
}

// 用 SshCopyIdWorker 部署一次，阻塞等结果。返回 (ok, alreadyPresent)。
static bool deployOnce(const DeviceEntry &dev, const QString &pubLine, bool *alreadyPresent)
{
    SshCopyIdWorker worker(dev, pubLine);
    bool ok = false;
    *alreadyPresent = false;
    QEventLoop loop;
    QObject::connect(&worker, &SshCopyIdWorker::finishedSignal,
                     [&](bool s, const QString &msg, bool present) {
                         ok = s;
                         *alreadyPresent = present;
                         qInfo() << "  deploy:" << (s ? "OK" : "FAIL") << msg
                                 << (present ? "(已存在)" : "");
                         loop.quit();
                     });
    worker.start();
    loop.exec();
    worker.wait(15000);
    return ok;
}

// 用私钥文件经 libssh2 连接并跑 whoami；返回远端用户名（空 = 失败）。
static QString whoamiWithKey(const DeviceEntry &base, const QString &keyType,
                             const QString &keyFile)
{
    DeviceEntry dev = base;
    dev.credentialKind = SshCredentialKind::PrivateKeyFile;
    dev.keyType = keyType;
    dev.keyFile = keyFile;
    dev.password.clear();

    // SshClient 线程不友好且阻塞，放 worker 线程跑。
    QString user;
    bool ok = false;
    QThread *t = QThread::create([&]() {
        SshClient ssh;
        const HostPort hp = dev.hostPort();
        ssh.setHost(hp.host, hp.port);
        ssh.setUsername(dev.username);
        ssh.setCredentialKind(dev.credentialKind);
        ssh.setPrivateKey(dev.keyType, dev.keyFile);
        SshError err;
        if (!ssh.connectToHost(nullptr, err)) {
            qWarning() << "  key-auth connect failed:" << err.message;
            return;
        }
        CommandExecutor exec(&ssh);
        const ExecResult r = exec.exec(QStringLiteral("whoami"), false);
        if (r.ok() && r.exitCode == 0) {
            user = r.stdoutText.trimmed();
            ok = true;
        } else {
            qWarning() << "  whoami failed:" << r.stderrText << r.errorMessage;
        }
        ssh.disconnectFromHost();
    });
    t->start();
    t->wait(20000);
    t->deleteLater();
    return ok ? user : QString();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const DeviceEntry dev = makePasswordDevice();
    qInfo() << "target:" << dev.username << "@" << dev.host << ":" << dev.port;

    QTemporaryDir dir;
    CHECK(dir.isValid());
    SshKeyStore store(dir.filePath(QStringLiteral("ssh_keys.json")));

    // 生成两种格式各一把：Ed25519 走 OpenSSH 格式，RSA 走 PKCS#8 PEM。
    struct { const char *name; const char *type; int bits; const char *keyType; } specs[] = {
        { "e2e-ed25519", "ssh-ed25519", 0, "Ed25519Key" },
        { "e2e-rsa", "ssh-rsa", 2048, "RSAKey" },
    };

    for (const auto &s : specs) {
        qInfo() << "== " << s.type << " ==";
        SshKeyGenParams p;
        p.type = QString::fromLatin1(s.type);
        p.bits = s.bits;
        p.comment = QStringLiteral("e2e copyid");
        SshKeyEntry key;
        QString err;
        CHECK(store.createKey(QString::fromLatin1(s.name), p, &key, &err));
        if (key.id.isEmpty())
            continue;

        // 第一次部署应新增；第二次应命中去重（已存在）。
        bool present = false;
        CHECK(deployOnce(dev, key.publicKeyLine, &present));
        CHECK(!present);
        CHECK(deployOnce(dev, key.publicKeyLine, &present));
        CHECK(present);   // 幂等：重复部署报已存在

        // 用生成的私钥连回去——证明 libssh2 读得出我们的格式、copy-id 装对了。
        const QString remote = whoamiWithKey(dev, QString::fromLatin1(s.keyType),
                                             key.privateKeyPath);
        CHECK(remote == dev.username);
        qInfo() << "  key-auth whoami ->" << remote;
    }

    if (failures) {
        qWarning() << "ssh_key_copyid_test:" << failures << "failure(s)";
        return 1;
    }
    qInfo() << "ssh_key_copyid_test: ALL PASSED";
    return 0;
}
