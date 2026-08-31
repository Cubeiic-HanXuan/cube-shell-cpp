// ssh_key_store_test.cpp — SshKeyGenerator + SshKeyStore 的单元测试。
//
// 覆盖：密钥对生成（Ed25519/RSA/ECDSA）的公钥行/blob/指纹形态、注释净化、
// 索引 CRUD、0600 私钥权限、id 稳定性。公钥/指纹与系统 ssh-keygen 的逐字节
// 一致性在 docs 验证流程里另做（需起真 sshd），这里只断言自洽形态。

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QTemporaryDir>

#include "ssh/SshKeyGenerator.h"
#include "ssh/SshKeyStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 注释净化：剥离会破坏远端单引号包裹与折行的字符。
static void testSanitizeComment()
{
    CHECK(SshKeyGenerator::sanitizeComment(QStringLiteral("hello world"))
          == QStringLiteral("hello world"));
    CHECK(SshKeyGenerator::sanitizeComment(QStringLiteral("a'b\\c$d`e"))
          == QStringLiteral("a b c d e"));
    CHECK(SshKeyGenerator::sanitizeComment(QStringLiteral("  x\ny\r "))
          == QStringLiteral("x y"));
}

// 公钥 blob 形态：base64 往返 + 首字段即类型名。
static void checkBlobRoundTrip(const SshKeyGenResult &r, const QString &expectType)
{
    CHECK(r.type == expectType);
    CHECK(!r.publicKeyBlob.isEmpty());
    CHECK(SshKeyGenerator::typeNameFromBlob(r.publicKeyBlob) == expectType);
    // 公钥行 = "type base64 [comment]"，第二段 base64 解码须回到原始 blob。
    const QStringList parts = r.publicKeyLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    CHECK(parts.size() >= 2);
    CHECK(parts.at(0) == expectType);
    CHECK(QByteArray::fromBase64(parts.at(1).toLatin1()) == r.publicKeyBlob);
}

// 指纹形态：SHA256: 前缀 + 43 个无填充 base64 字符（32 字节摘要）。
static void checkFingerprint(const QString &fp)
{
    CHECK(fp.startsWith(QStringLiteral("SHA256:")));
    const QString b64 = fp.mid(7);
    CHECK(b64.size() == 43);
    CHECK(!b64.contains(QLatin1Char('=')));
}

static void testGenerate()
{
    QString err;

    SshKeyGenParams ed;
    ed.type = QStringLiteral("ssh-ed25519");
    ed.comment = QStringLiteral("test key");
    SshKeyGenResult rEd;
    CHECK(SshKeyGenerator::generate(ed, &rEd, &err));
    if (!rEd.privateKeyPem.isEmpty()) {
        checkBlobRoundTrip(rEd, QStringLiteral("ssh-ed25519"));
        CHECK(rEd.bits == 0);
        // 无口令 Ed25519 用 OpenSSH 原生格式（系统 ssh 可读）。
        CHECK(rEd.privateKeyPem.contains("BEGIN OPENSSH PRIVATE KEY"));
        CHECK(rEd.publicKeyLine.endsWith(QLatin1String("test key")));
        checkFingerprint(rEd.fingerprint);
    }

    SshKeyGenParams rsa;
    rsa.type = QStringLiteral("ssh-rsa");
    rsa.bits = 2048;
    SshKeyGenResult rRsa;
    CHECK(SshKeyGenerator::generate(rsa, &rRsa, &err));
    if (!rRsa.privateKeyPem.isEmpty()) {
        checkBlobRoundTrip(rRsa, QStringLiteral("ssh-rsa"));
        CHECK(rRsa.bits == 2048);
        checkFingerprint(rRsa.fingerprint);
    }

    SshKeyGenParams ec;
    ec.type = QStringLiteral("ecdsa-sha2-nistp384");
    SshKeyGenResult rEc;
    CHECK(SshKeyGenerator::generate(ec, &rEc, &err));
    if (!rEc.privateKeyPem.isEmpty()) {
        checkBlobRoundTrip(rEc, QStringLiteral("ecdsa-sha2-nistp384"));
        CHECK(rEc.bits == 384);
        checkFingerprint(rEc.fingerprint);
    }

    // 带口令的私钥应为加密 PEM（不是裸 PKCS#8）。
    SshKeyGenParams enc;
    enc.type = QStringLiteral("ssh-ed25519");
    enc.passphrase = QStringLiteral("s3cret");
    SshKeyGenResult rEnc;
    CHECK(SshKeyGenerator::generate(enc, &rEnc, &err));
    if (!rEnc.privateKeyPem.isEmpty())
        CHECK(rEnc.privateKeyPem.contains("BEGIN ENCRYPTED PRIVATE KEY"));

    // 两次生成同类型应得不同密钥（随机性）。
    SshKeyGenResult rEd2;
    CHECK(SshKeyGenerator::generate(ed, &rEd2, &err));
    CHECK(rEd.publicKeyBlob != rEd2.publicKeyBlob);
}

// store CRUD 往返 + 0600 权限 + id 稳定 + 删除连文件一起清。
static void testStore()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    SshKeyStore store(dir.filePath(QStringLiteral("ssh_keys.json")));

    CHECK(store.load().isEmpty());   // 缺失文件 = 空表
    CHECK(!store.keyDir().isEmpty());

    SshKeyGenParams p;
    p.type = QStringLiteral("ssh-ed25519");
    p.comment = QStringLiteral("deploy me");
    SshKeyEntry k;
    QString err;
    CHECK(store.createKey(QStringLiteral("我的密钥"), p, &k, &err));
    if (k.id.isEmpty())
        return;

    CHECK(QFile::exists(k.privateKeyPath));
    CHECK(QFile::exists(k.publicKeyPath));
    // 私钥须为 0600（仅属主读写）。
    const QFile::Permissions perms = QFile(k.privateKeyPath).permissions();
    CHECK(!(perms & (QFile::ReadGroup | QFile::WriteGroup |
                     QFile::ReadOther | QFile::WriteOther)));

    QList<SshKeyEntry> all = store.load();
    CHECK(all.size() == 1);
    CHECK(all[0].id == k.id);
    CHECK(all[0].name == QStringLiteral("我的密钥"));
    CHECK(all[0].type == QStringLiteral("ssh-ed25519"));
    CHECK(all[0].comment == QStringLiteral("deploy me"));
    CHECK(all[0].fingerprint.startsWith(QStringLiteral("SHA256:")));
    QFile pubFile(k.publicKeyPath);
    CHECK(pubFile.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(pubFile.readAll()).trimmed() == all[0].publicKeyLine);

    // upsert 同 id 覆盖不新增，id 不变。
    SshKeyEntry edited = all[0];
    edited.name = QStringLiteral("改名后");
    CHECK(store.upsert(edited, &err));
    all = store.load();
    CHECK(all.size() == 1);
    CHECK(all[0].id == edited.id);
    CHECK(all[0].name == QStringLiteral("改名后"));

    // 删除：索引条目与两份密钥文件一并清掉；再删幂等。
    CHECK(store.remove(k.id, &err));
    CHECK(store.load().isEmpty());
    CHECK(!QFile::exists(k.privateKeyPath));
    CHECK(!QFile::exists(k.publicKeyPath));
    CHECK(store.remove(k.id, &err));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 手工验证模式：--dump <dir> 把三种密钥各生成一份到 <dir>（经 SshKeyStore
    // 落盘），打印指纹与公钥行，便于与系统 ssh-keygen -y/-lf 逐字节对比。
    const QStringList args = app.arguments();
    const int dumpIdx = args.indexOf(QStringLiteral("--dump"));
    if (dumpIdx >= 0 && dumpIdx + 1 < args.size()) {
        const QString dirPath = args.at(dumpIdx + 1);
        SshKeyStore store(dirPath + QStringLiteral("/ssh_keys.json"));
        const struct { const char *name; const char *type; int bits; } specs[] = {
            { "ed25519", "ssh-ed25519", 0 },
            { "rsa2048", "ssh-rsa", 2048 },
            { "ecdsa256", "ecdsa-sha2-nistp256", 0 },
        };
        for (const auto &s : specs) {
            SshKeyGenParams p;
            p.type = QString::fromLatin1(s.type);
            p.bits = s.bits;
            p.comment = QStringLiteral("dump");
            SshKeyEntry k;
            QString err;
            if (!store.createKey(QString::fromLatin1(s.name), p, &k, &err)) {
                qWarning() << "dump failed for" << s.type << ":" << err;
                return 1;
            }
            printf("%s\n  priv: %s\n  pub:  %s\n  fp:   %s\n  line: %s\n",
                   s.type,
                   qPrintable(k.privateKeyPath), qPrintable(k.publicKeyPath),
                   qPrintable(k.fingerprint), qPrintable(k.publicKeyLine));
        }
        return 0;
    }

    testSanitizeComment();
    testGenerate();
    testStore();

    if (failures) {
        qWarning() << "ssh_key_store_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "ssh_key_store_test: all passed";
    return 0;
}
