// Config store unit test: host:port parse/format (IPv4/IPv6/bare) and the
// add -> edit -> delete -> save JSON -> reload round-trip.

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#include "config/DeviceConfigStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testParseFormat()
{
    // IPv4
    HostPort a = parseHostPort(QStringLiteral("192.168.1.10:2222"));
    CHECK(a.host == QStringLiteral("192.168.1.10") && a.port == 2222);
    // bare host -> default
    HostPort b = parseHostPort(QStringLiteral("example.com"));
    CHECK(b.host == QStringLiteral("example.com") && b.port == 22);
    // [IPv6]:port
    HostPort c = parseHostPort(QStringLiteral("[fd00::1]:2200"));
    CHECK(c.host == QStringLiteral("fd00::1") && c.port == 2200);
    // bare IPv6 -> default port, no split
    HostPort d = parseHostPort(QStringLiteral("fd00::1"));
    CHECK(d.host == QStringLiteral("fd00::1") && d.port == 22);

    // format round-trips
    CHECK(formatHostPort(QStringLiteral("192.168.1.10"), 22) == QStringLiteral("192.168.1.10:22"));
    CHECK(formatHostPort(QStringLiteral("fd00::1"), 3389) == QStringLiteral("[fd00::1]:3389"));
}

static void testCrudRoundTrip()
{
    DeviceConfigStore store;
    CHECK(store.load(QStringLiteral("/tmp/cubeshell_test/config_p4.dat")));
    const int base = store.count();
    CHECK(base == 3);

    // The pickle stores host as "host:port".
    const DeviceEntry *web = store.find(QStringLiteral("web服务器"));
    CHECK(web != nullptr);
    CHECK(web->hostPort().host == QStringLiteral("192.168.1.10"));

    // ADD
    DeviceEntry added;
    added.name = QStringLiteral("new-box");
    added.username = QStringLiteral("deploy");
    added.password = QStringLiteral("pw");
    added.host = formatHostPort(QStringLiteral("10.0.0.5"), 2222);
    added.port = 2222;
    store.addDevice(added);
    CHECK(store.count() == base + 1);
    CHECK(store.find(QStringLiteral("new-box"))->hostPort().port == 2222);

    // EDIT (rename + change key auth)
    DeviceEntry edited = *store.find(QStringLiteral("new-box"));
    store.removeDevice(QStringLiteral("new-box"));
    edited.name = QStringLiteral("renamed-box");
    edited.password.clear();
    edited.keyType = QStringLiteral("Ed25519Key");
    edited.keyFile = QStringLiteral("/home/deploy/.ssh/id_ed25519");
    store.addDevice(edited);
    CHECK(store.find(QStringLiteral("new-box")) == nullptr);
    const DeviceEntry *rb = store.find(QStringLiteral("renamed-box"));
    CHECK(rb && rb->usesKey());

    // DELETE
    CHECK(store.removeDevice(QStringLiteral("renamed-box")));
    CHECK(store.count() == base);

    // SAVE + RELOAD JSON
    const QString jsonPath = QDir::temp().filePath(QStringLiteral("cubeshell_crud.json"));
    CHECK(store.saveJson(jsonPath));
    DeviceConfigStore reloaded;
    CHECK(reloaded.loadJson(jsonPath));
    CHECK(reloaded.count() == store.count());
    const DeviceEntry *rw = reloaded.find(QStringLiteral("web服务器"));
    CHECK(rw && rw->username == QStringLiteral("root"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testParseFormat();
    testCrudRoundTrip();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
