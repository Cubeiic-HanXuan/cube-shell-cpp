// secrets_migration_test.cpp — 明文密码 → 钥匙串迁移的正确性测试。
//
// 这是整个改动里唯一可能把用户密码弄丢的代码，所以测的不是「能迁移」，
// 而是「迁移失败时一条密码都不会少」。跑的是真实钥匙串后端
//（macOS 上就是 login Keychain，一个聚合条目装下全部测试密码）。
//
// 覆盖：
//   (a) 迁移成功后 JSON 无 password 键、密码能从钥匙串读回
//   (b) 备份文件权限 0600
//   (c) 翻闸门前（迁移窗口期）保存仍写明文 —— m_inlinePasswords 闸门
//   (d) needsMigration 的判定：旧格式 true、新格式 false
//   (e) resolved() 不污染 find() 的不变量（find 拿到的条目恒无密码）
//   (f) 全部 5 个协议分支的 id 都非空（device() 的提前 return 回归）

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include "config/DeviceConfigStore.h"
#include "config/SecretMigration.h"
#include "config/Secrets.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 造一份旧格式 devices.json：无 id、带明文 password。
static QString writeOldFormatJson(const QString &dir)
{
    const QString path = dir + QStringLiteral("/devices.json");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(QByteArrayLiteral(
        R"([{"name":"web","username":"root","password":"livepw1","host":"10.0.0.1:22","port":22,"protocol":"ssh"},
            {"name":"db","username":"dba","password":"livepw2","host":"10.0.0.2:22","port":22,"protocol":"ssh"},
            {"name":"keyonly","username":"op","password":"","host":"10.0.0.3:22","port":22,"protocol":"ssh","keyType":"RSAKey","keyFile":"/k"}])"));
    f.close();
    return path;
}

static void testMigrationHappyPath()
{
    if (!Secrets::isAvailable()) {
        qInfo() << "SKIP: 本平台无钥匙串后端";
        return;
    }
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = writeOldFormatJson(dir.path());
    CHECK(!jsonPath.isEmpty());

    DeviceConfigStore store;
    QString err;
    CHECK(store.loadJson(jsonPath, &err));

    // (d) 旧格式 → 需要迁移
    CHECK(store.needsMigration());
    CHECK(store.inlinePasswords());

    const SecretMigration::Result r = SecretMigration::run(store, jsonPath);
    if (r.status != SecretMigration::Result::Migrated)
        qWarning() << "migration status:" << r.status << "err:" << r.error;
    CHECK(r.status == SecretMigration::Result::Migrated);
    CHECK(r.migrated == 2);          // keyonly 的 password 为空，不计入

    // (a) JSON 里不再有任何 password 键
    QFile f(jsonPath);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    CHECK(!raw.contains("\"password\":"));  // 按「键」匹配：credentialKind 的值也是 "password"
    CHECK(raw.contains("\"id\""));

    // 闸门已翻
    CHECK(!store.inlinePasswords());
    CHECK(!store.needsMigration());

    // (a) 密码能从钥匙串读回（resolved 走真实钥匙串）
    CHECK(store.resolved(QStringLiteral("web")).password == QStringLiteral("livepw1"));
    CHECK(store.resolved(QStringLiteral("db")).password == QStringLiteral("livepw2"));
    CHECK(store.resolved(QStringLiteral("keyonly")).password.isEmpty());

    // (e) find() 的不变量：永不带密码
    CHECK(store.find(QStringLiteral("web"))->password.isEmpty());
    CHECK(!store.find(QStringLiteral("web"))->id.isEmpty());

    // (b) 备份存在且权限 0600
    const QString backup = jsonPath + QStringLiteral(".plain.bak");
    CHECK(QFileInfo::exists(backup));
    const auto perms = QFile::permissions(backup);
    CHECK(perms & QFileDevice::ReadOwner);
    CHECK(!(perms & QFileDevice::ReadGroup));
    CHECK(!(perms & QFileDevice::ReadOther));

    // 幂等：再跑一次应当 NotNeeded
    const SecretMigration::Result r2 = SecretMigration::run(store, jsonPath);
    CHECK(r2.status == SecretMigration::Result::NotNeeded);
}

static void testMigrationGateKeepsPlaintext()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = writeOldFormatJson(dir.path());
    CHECK(!jsonPath.isEmpty());

    DeviceConfigStore store;
    QString err;
    CHECK(store.loadJson(jsonPath, &err));
    CHECK(store.needsMigration());

    // (c) 不跑迁移、直接保存 —— 模拟「迁移窗口期内触发了别的保存路径」。
    // 此时必须仍写明文，否则密码还没进钥匙串就被删了。
    CHECK(store.saveJson(jsonPath, &err));
    QFile f(jsonPath);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    CHECK(raw.contains("\"password\":"));  // 明文必须还在（按「键」匹配，避开 credentialKind 的值）
    CHECK(raw.contains("\"id\""));          // id 已补齐
}

static void testFreshFormatNotMigrated()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.path() + QStringLiteral("/devices.json");

    // 全新安装的 store（不经过任何 load），inlinePasswords 默认 false。
    DeviceConfigStore store;
    CHECK(!store.needsMigration());
    DeviceEntry e;
    e.name = QStringLiteral("new");
    e.username = QStringLiteral("u");
    e.password = QStringLiteral("pw");
    e.host = QStringLiteral("1.2.3.4:22");
    store.addDevice(e);
    CHECK(!store.needsMigration());
    QString err;
    CHECK(store.saveJson(jsonPath, &err));

    QFile f(jsonPath);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    CHECK(!raw.contains("\"password\":"));  // 按「键」匹配：credentialKind 的值也是 "password"
}

static void testAllProtocolIdsNonEmpty()
{
    // (f) device() 顶部赋 id 的回归：5 个协议分支的条目都要有 id，
    // 否则密码存不进钥匙串。这里直接构造各协议的 DeviceEntry，
    // 走 addDevice 的 id 兜底（对话框侧的分支逻辑在 UI 层，单测够不着）。
    DeviceConfigStore store;
    const QStringList protos = {QStringLiteral("ssh"), QStringLiteral("rdp"),
                                QStringLiteral("telnet"), QStringLiteral("tcp"),
                                QStringLiteral("serial")};
    for (const QString &p : protos) {
        DeviceEntry e;
        e.name = p;
        e.protocol = p;
        e.password = QStringLiteral("pw");
        store.addDevice(e);   // id 为空时 addDevice 现分配
    }
    for (const QString &p : protos) {
        const DeviceEntry *e = store.find(p);
        CHECK(e != nullptr);
        if (e)
            CHECK(!e->id.isEmpty());
    }
    // 且彼此不相同
    QSet<QString> ids;
    for (const DeviceEntry &e : store.devices())
        ids.insert(e.id);
    CHECK(ids.size() == protos.size());
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testMigrationGateKeepsPlaintext();
    testFreshFormatNotMigrated();
    testAllProtocolIdsNonEmpty();
    testMigrationHappyPath();   // 最后跑：会真的写钥匙串
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
