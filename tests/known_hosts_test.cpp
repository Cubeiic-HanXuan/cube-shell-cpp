// known_hosts_test.cpp — unit tests for KnownHostsStore.

#include <QCoreApplication>
#include <QDebug>
#include <QTemporaryFile>

#include "ssh/KnownHostsStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testLoadEmpty()
{
    KnownHostsStore store;
    QString err;
    CHECK(store.load(&err));
    CHECK(store.count() == 0);
}

static void testParseAndCheck()
{
    const QByteArray pubKey = QByteArray(32, 'A').toBase64();
    const QString line = QStringLiteral("test.example.com ssh-ed25519 %1 comment").arg(QString::fromLatin1(pubKey));

    QTemporaryFile tmp;
    CHECK(tmp.open());
    tmp.write(line.toUtf8() + '\n');
    tmp.flush();

    KnownHostsStore store(tmp.fileName());
    QString err;
    CHECK(store.load(&err));
    CHECK(store.count() == 1);

    const QByteArray serverKey = QByteArray(32, 'A');
    const QByteArray fp = KnownHostsStore::fingerprintSha256(serverKey);
    CHECK(store.check(QStringLiteral("test.example.com"), 22, fp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Match);

    const QByteArray otherKey = QByteArray(32, 'B');
    const QByteArray otherFp = KnownHostsStore::fingerprintSha256(otherKey);
    CHECK(store.check(QStringLiteral("test.example.com"), 22, otherFp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Mismatch);

    CHECK(store.check(QStringLiteral("other.example.com"), 22, fp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::NotFound);
}

static void testPortFormat()
{
    const QByteArray pubKey = QByteArray(32, 'X').toBase64();
    const QString line = QStringLiteral("[myhost]:2222 ssh-ed25519 %1").arg(QString::fromLatin1(pubKey));

    QTemporaryFile tmp;
    CHECK(tmp.open());
    tmp.write(line.toUtf8() + '\n');
    tmp.flush();

    KnownHostsStore store(tmp.fileName());
    CHECK(store.load());

    const QByteArray serverKey = QByteArray(32, 'X');
    const QByteArray fp = KnownHostsStore::fingerprintSha256(serverKey);
    CHECK(store.check(QStringLiteral("myhost"), 2222, fp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Match);
    CHECK(store.check(QStringLiteral("myhost"), 22, fp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::NotFound);
}

static void testAcceptAndSave()
{
    QTemporaryFile tmp;
    CHECK(tmp.open());
    tmp.close();

    KnownHostsStore store(tmp.fileName());
    CHECK(store.load());

    const QByteArray serverKey = QByteArray(32, 'Z');
    const QByteArray fp = KnownHostsStore::fingerprintSha256(serverKey);
    QString err;
    CHECK(store.accept(QStringLiteral("newhost"), 22, serverKey,
                       QStringLiteral("ssh-ed25519"), &err));

    CHECK(store.check(QStringLiteral("newhost"), 22, fp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Match);

    KnownHostsStore store2(tmp.fileName());
    CHECK(store2.load());
    CHECK(store2.check(QStringLiteral("newhost"), 22, fp,
                       QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Match);
}

static void testReplaceExisting()
{
    QTemporaryFile tmp;
    CHECK(tmp.open());

    const QByteArray oldKey = QByteArray(32, 'O').toBase64();
    tmp.write(QByteArray("oldhost ssh-ed25519 ") + oldKey + '\n');
    tmp.flush();

    KnownHostsStore store(tmp.fileName());
    CHECK(store.load());

    const QByteArray newKey = QByteArray(32, 'N');
    const QByteArray newFp = KnownHostsStore::fingerprintSha256(newKey);
    CHECK(store.accept(QStringLiteral("oldhost"), 22, newKey,
                       QStringLiteral("ssh-ed25519")));

    CHECK(store.check(QStringLiteral("oldhost"), 22, newFp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Match);

    const QByteArray oldFp = KnownHostsStore::fingerprintSha256(QByteArray(32, 'O'));
    CHECK(store.check(QStringLiteral("oldhost"), 22, oldFp,
                      QStringLiteral("ssh-ed25519"))
          == KnownHostsStore::CheckResult::Mismatch);
}

static void testFingerprintDisplay()
{
    const QByteArray fp(32, '\xab');
    const QString display = KnownHostsStore::fingerprintDisplayString(fp);
    CHECK(display.startsWith(QStringLiteral("SHA256:")));
    CHECK(display.size() == 7 + QString::fromLatin1(fp.toBase64()).size());
}

static void testKeyTypeFromLibssh2()
{
    CHECK(KnownHostsStore::keyTypeFromLibssh2(1) == QStringLiteral("ssh-rsa"));
    CHECK(KnownHostsStore::keyTypeFromLibssh2(6) == QStringLiteral("ssh-ed25519"));
    CHECK(KnownHostsStore::keyTypeFromLibssh2(99) == QStringLiteral("unknown"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testLoadEmpty();
    testParseAndCheck();
    testPortFormat();
    testAcceptAndSave();
    testReplaceExisting();
    testFingerprintDisplay();
    testKeyTypeFromLibssh2();

    if (failures) {
        qWarning() << "known_hosts_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "known_hosts_test: all passed";
    return 0;
}
