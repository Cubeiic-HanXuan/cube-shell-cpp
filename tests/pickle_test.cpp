// Test: PickleReader parses real config.dat pickles (protocol 2 & 4) and the
// DeviceConfigStore round-trips to JSON. Exits 0 on success.

#include <QCoreApplication>
#include <QFile>
#include <QDebug>

#include "config/PickleReader.h"
#include "config/DeviceConfigStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "at line" << __LINE__; ++failures; } } while (0)

static void testFile(const QString &path, int expectedCount)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { qWarning() << "cannot open" << path; ++failures; return; }
    const QByteArray data = f.readAll();

    QHash<QString, QStringList> out;
    QString err;
    const bool ok = PickleReader::parseDeviceConfig(data, out, &err);
    qInfo() << path << "ok:" << ok << "devices:" << out.size() << (ok ? "" : err);
    CHECK(ok);
    CHECK(out.size() == expectedCount);

    if (expectedCount > 0) {
        const QStringList web = out.value(QStringLiteral("web服务器"));
        CHECK(web.size() == 5);
        CHECK(web.value(0) == QStringLiteral("root"));
        CHECK(web.value(2) == QStringLiteral("192.168.1.10"));
        CHECK(web.value(3) == QStringLiteral("RSAKey"));
        CHECK(out.contains(QStringLiteral("中文设备名_很长很长的名字用于测试短unicode边界")));
        const QStringList db = out.value(QStringLiteral("db"));
        CHECK(db.size() == 3);
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testFile(QStringLiteral("/tmp/cubeshell_test/config_p2.dat"), 3);
    testFile(QStringLiteral("/tmp/cubeshell_test/config_p4.dat"), 3);
    testFile(QStringLiteral("/tmp/cubeshell_test/config_empty.dat"), 0);

    // Round-trip: load pickle -> save JSON -> load JSON.
    DeviceConfigStore store;
    QString err;
    CHECK(store.load(QStringLiteral("/tmp/cubeshell_test/config_p4.dat"), &err));
    CHECK(store.count() == 3);
    CHECK(store.saveJson(QStringLiteral("/tmp/cubeshell_test/devices.json"), &err));

    DeviceConfigStore store2;
    CHECK(store2.loadJson(QStringLiteral("/tmp/cubeshell_test/devices.json"), &err));
    CHECK(store2.count() == 3);
    const DeviceEntry *web = store2.find(QStringLiteral("web服务器"));
    CHECK(web && web->username == QStringLiteral("root") && web->keyType == QStringLiteral("RSAKey"));

    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
