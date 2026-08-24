// Config module unit test: GroupManager CRUD + JSON round-trip (Python
// groups.json compatible), FrequentlyUsedCommands load/save/filter,
// ConfigUtil JSON/TOML loading against the real conf/ samples, and
// GlobalState path resolution. Secrets is exercised at compile/link level
// only (no real Keychain writes).

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "config/ConfigUtil.h"
#include "config/FrequentlyUsedCommands.h"
#include "config/GlobalState.h"
#include "config/GroupManager.h"
#include "config/Secrets.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// Real shared conf/ directory, resolved relative to this source file
// (tests -> ../conf), so the test works from any build dir.
static QString confDir()
{
    return QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
           + QStringLiteral("/../conf");
}

static void testGroupManager()
{
    const QString path = QDir::temp().filePath(QStringLiteral("cubeshell_groups_test.json"));
    QFile::remove(path);
    GroupManager gm(path);

    // Missing file -> empty structure, never an error.
    GroupData empty = gm.loadGroups();
    CHECK(empty.groups.isEmpty() && empty.deviceGroupMap.isEmpty());

    // CREATE (+ duplicate rejected)
    CHECK(gm.createGroup(QStringLiteral("华东地区")));
    CHECK(gm.createGroup(QStringLiteral("华南地区")));
    CHECK(!gm.createGroup(QStringLiteral("华东地区")));

    // Assign devices.
    gm.moveDeviceToGroup(QStringLiteral("设备A"), QStringLiteral("华东地区"));
    gm.moveDeviceToGroup(QStringLiteral("设备B"), QStringLiteral("华南地区"));
    gm.moveDeviceToGroup(QStringLiteral("设备C"), QStringLiteral("华东地区"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A")) == QStringLiteral("华东地区"));
    CHECK(gm.deviceGroup(QStringLiteral("不存在")).isEmpty());

    // On-disk shape must match the Python format exactly.
    {
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(root.value(QStringLiteral("groups")).isArray());
        CHECK(root.value(QStringLiteral("device_group_map")).isObject());
        CHECK(root.value(QStringLiteral("groups")).toArray().size() == 2);
        CHECK(root.value(QStringLiteral("device_group_map")).toObject()
                  .value(QStringLiteral("设备A")).toString() == QStringLiteral("华东地区"));
    }

    // RENAME updates the map references too.
    CHECK(gm.renameGroup(QStringLiteral("华东地区"), QStringLiteral("华东")));
    CHECK(!gm.renameGroup(QStringLiteral("不存在"), QStringLiteral("x")));
    CHECK(!gm.renameGroup(QStringLiteral("华南地区"), QStringLiteral("华东"))); // 已存在
    CHECK(gm.deviceGroup(QStringLiteral("设备A")) == QStringLiteral("华东"));

    // Grouped view: groups order kept, __ungrouped__ last and only if needed.
    const QStringList all = {QStringLiteral("设备A"), QStringLiteral("设备B"),
                             QStringLiteral("设备C"), QStringLiteral("设备D")};
    QList<GroupedDevices> view = gm.groupedDevices(all);
    CHECK(view.size() == 3);
    CHECK(view[0].group == QStringLiteral("华东") && view[0].devices.size() == 2);
    CHECK(view[1].group == QStringLiteral("华南地区") && view[1].devices == QStringList{QStringLiteral("设备B")});
    CHECK(view[2].group == GroupManager::kUngrouped && view[2].devices == QStringList{QStringLiteral("设备D")});

    // No ungrouped devices -> no __ungrouped__ entry; empty groups still listed.
    view = gm.groupedDevices({QStringLiteral("设备A")});
    CHECK(view.size() == 2);
    CHECK(view[1].devices.isEmpty());

    // Device rename / delete hooks.
    gm.onDeviceRenamed(QStringLiteral("设备A"), QStringLiteral("设备A2"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A")).isEmpty());
    CHECK(gm.deviceGroup(QStringLiteral("设备A2")) == QStringLiteral("华东"));
    gm.onDeviceDeleted(QStringLiteral("设备A2"));
    CHECK(gm.deviceGroup(QStringLiteral("设备A2")).isEmpty());

    // DELETE group drops its device mappings (devices become ungrouped).
    gm.deleteGroup(QStringLiteral("华南地区"));
    GroupData after = gm.loadGroups();
    CHECK(after.groups == QStringList{QStringLiteral("华东")});
    CHECK(!after.deviceGroupMap.contains(QStringLiteral("设备B")));

    // Round-trip through save/load preserves everything.
    GroupData rt;
    rt.groups = {QStringLiteral("g1"), QStringLiteral("g2")};
    rt.deviceGroupMap.insert(QStringLiteral("d1"), QStringLiteral("g1"));
    CHECK(gm.saveGroups(rt));
    const GroupData back = gm.loadGroups();
    CHECK(back.groups == rt.groups);
    CHECK(back.deviceGroupMap == rt.deviceGroupMap);

    QFile::remove(path);
}

static void testFrequentlyUsedCommands()
{
    FrequentlyUsedCommands cmds;
    QString err;
    CHECK(cmds.load(confDir() + QStringLiteral("/linux_commands.json"), &err));
    CHECK(err.isEmpty());
    CHECK(!cmds.isEmpty());

    // The real data has a category row with children ("文件管理" -> cat...).
    // 空树时 first() 是未定义行为，先记一次失败再退出，别把整个用例带崩。
    if (cmds.isEmpty()) {
        qWarning() << "FAIL: conf/linux_commands.json 不可读，后续断言跳过";
        ++failures;
        return;
    }
    CHECK(cmds.entries().first().hasChildren());
    const CommandEntry *cat = cmds.find(QStringLiteral("cat"));
    CHECK(cat != nullptr);
    CHECK(cat && !cat->description.isEmpty());

    // Recursive case-insensitive filtering keeps matching subtrees.
    const QList<CommandEntry> hits = cmds.filter(QStringLiteral("CHOWN"));
    CHECK(!hits.isEmpty());
    bool found = false;
    for (const CommandEntry &top : hits) {
        for (const CommandEntry &child : top.children)
            if (child.command == QStringLiteral("chown"))
                found = true;
    }
    CHECK(found);
    // Empty needle returns the full tree.
    CHECK(cmds.filter(QString()).size() == cmds.entries().size());

    // Write/read round-trip in the Python-compatible {"treeData": ...} shape.
    const QString tmp = QDir::temp().filePath(QStringLiteral("cubeshell_cmds_test.json"));
    CHECK(cmds.save(tmp, &err));
    FrequentlyUsedCommands reloaded;
    CHECK(reloaded.load(tmp, &err));
    CHECK(reloaded.entries().size() == cmds.entries().size());
    const CommandEntry *cat2 = reloaded.find(QStringLiteral("cat"));
    CHECK(cat2 && cat2->option == cat->option && cat2->description == cat->description);
    {
        QFile f(tmp);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(root.value(QStringLiteral("treeData")).isArray());
    }
    QFile::remove(tmp);
}

// 打包产物（macOS .app / Windows 安装包 / 鸿蒙 HAP）里没有 conf/ 目录，
// 「Linux常用命令查找」曾因此只显示「未找到 linux_commands.json」。命令库现在
// 编进二进制（conf/conf.qrc），这里校验内置副本确实在、且能解析出真实数据。
static void testEmbeddedCommandsResource()
{
    FrequentlyUsedCommands embedded;
    QString err;
    CHECK(embedded.load(QStringLiteral(":/conf/linux_commands.json"), &err));
    CHECK(err.isEmpty());
    CHECK(!embedded.isEmpty());
    CHECK(embedded.find(QStringLiteral("cat")) != nullptr);

    // 探测兜底到 qrc，永不返回空路径 —— 对话框因此不会再落到失败分支。
    const QString resolved = FrequentlyUsedCommands::defaultPath();
    CHECK(!resolved.isEmpty());
    FrequentlyUsedCommands byDefault;
    CHECK(byDefault.load(resolved, &err));
    CHECK(!byDefault.isEmpty());
}

static void testConfigUtil()
{
    QString err;

    // JSON: real theme.json sample.
    const QJsonValue theme = ConfigUtil::readJson(confDir() + QStringLiteral("/theme.json"), &err);
    CHECK(theme.isObject());
    CHECK(theme.toObject().value(QStringLiteral("appearance")).toString() == QStringLiteral("dark"));

    // JSON write + reload (indent=4 equivalent).
    const QString tmp = QDir::temp().filePath(QStringLiteral("cubeshell_cfg_test.json"));
    QJsonObject obj;
    obj.insert(QStringLiteral("键"), QStringLiteral("值")); // 非 ASCII 需原样保留
    obj.insert(QStringLiteral("n"), 42);
    CHECK(ConfigUtil::writeJson(tmp, obj, &err));
    const QJsonValue back = ConfigUtil::readJson(tmp, &err);
    CHECK(back.toObject() == obj);
    QFile::remove(tmp);

    // TOML: real frpc.toml sample (scalars, dotted key, [[proxies]]).
    const QVariantMap frpc = ConfigUtil::readToml(confDir() + QStringLiteral("/frpc.toml"), &err);
    CHECK(err.isEmpty());
    CHECK(frpc.value(QStringLiteral("serverAddr")).toString() == QStringLiteral("127.0.0.1"));
    CHECK(frpc.value(QStringLiteral("serverPort")).toLongLong() == 7000);
    CHECK(frpc.value(QStringLiteral("auth")).toMap()
              .value(QStringLiteral("token")).toString() == QStringLiteral("123456"));
    const QVariantList proxies = frpc.value(QStringLiteral("proxies")).toList();
    CHECK(proxies.size() == 1);
    const QVariantMap p0 = proxies.first().toMap();
    CHECK(p0.value(QStringLiteral("name")).toString() == QStringLiteral("ssh_01"));
    CHECK(p0.value(QStringLiteral("type")).toString() == QStringLiteral("tcp"));
    CHECK(p0.value(QStringLiteral("localPort")).toLongLong() == 8080);
    CHECK(p0.value(QStringLiteral("remotePort")).toLongLong() == 80);

    // Parser corner cases: comments, bools, floats, inline arrays, [table].
    const QVariantMap t = ConfigUtil::parseToml(QStringLiteral(
        "a = 1 # comment\n"
        "b = \"x # not a comment\"\n"
        "ok = true\n"
        "pi = 3.14\n"
        "list = [1, 2, 3]\n"
        "[sec]\n"
        "k = 'lit'\n"), &err);
    CHECK(err.isEmpty());
    CHECK(t.value(QStringLiteral("a")).toLongLong() == 1);
    CHECK(t.value(QStringLiteral("b")).toString() == QStringLiteral("x # not a comment"));
    CHECK(t.value(QStringLiteral("ok")).toBool());
    CHECK(qAbs(t.value(QStringLiteral("pi")).toDouble() - 3.14) < 1e-9);
    CHECK(t.value(QStringLiteral("list")).toList().size() == 3);
    CHECK(t.value(QStringLiteral("sec")).toMap().value(QStringLiteral("k")).toString() == QStringLiteral("lit"));

    // Extension routing.
    CHECK(ConfigUtil::loadConfig(confDir() + QStringLiteral("/theme.json")).isValid());
    CHECK(ConfigUtil::loadConfig(confDir() + QStringLiteral("/frpc.toml")).isValid());
    CHECK(!ConfigUtil::loadConfig(QStringLiteral("/no/such/file.ini")).isValid());
}

static void testGlobalState()
{
    // vars constants survive the port.
    CHECK(QString::fromLatin1(vars::CONF_FILE) == QStringLiteral("tunnel.json"));
    CHECK(QString::fromLatin1(vars::keys::SSH_ADDRESS) == QStringLiteral("ssh_address"));
    CHECK(QString::fromLatin1(vars::cmds::SSH_KILL_NIX) == QStringLiteral("pkill ssh"));

    // Paths must land where Python's appdirs puts them (config interop!).
    const QString cfg = GlobalState::configDir();
    CHECK(cfg.endsWith(QStringLiteral("/cube-shell")));
#ifdef Q_OS_MACOS
    CHECK(cfg.contains(QStringLiteral("Library/Application Support")));
    CHECK(GlobalState::dataDir() == cfg); // macOS: config == data
#endif
    CHECK(QDir(cfg).exists()); // created on demand, like os.makedirs
    CHECK(GlobalState::configFilePath(QStringLiteral("config.dat"))
          == cfg + QStringLiteral("/config.dat"));
    CHECK(GlobalState::tunnelConfigPath().endsWith(QStringLiteral("cube-shell/tunnel.json")));
    CHECK(GlobalState::groupsConfigPath().endsWith(QStringLiteral("cube-shell/groups.json")));

    // Theme state from the real conf/theme.json.
    GlobalState &gs = GlobalState::instance();
    CHECK(&gs == &GlobalState::instance()); // singleton
    QString err;
    CHECK(gs.loadTheme(confDir() + QStringLiteral("/theme.json"), &err));
    CHECK(gs.appearance() == QStringLiteral("dark"));
    CHECK(gs.language() == QStringLiteral("zh_CN"));
    CHECK(gs.fontSize() == 14);
    CHECK(!gs.fontFamily().isEmpty());

    // In-memory mutation only (do NOT saveTheme(): conf/theme.json is real data).
    gs.setAppearance(QStringLiteral("Light"));
    CHECK(gs.appearance() == QStringLiteral("light"));
    gs.setFont(QStringLiteral("Menlo"), 16);
    CHECK(gs.fontFamily() == QStringLiteral("Menlo") && gs.fontSize() == 16);
}

static void testSecretsCompileLevel()
{
    // Compile/link-level only: take the addresses so the symbols must exist,
    // but never touch the real Keychain from a unit test.
    auto storeFn = static_cast<bool (*)(const QString &, const QString &,
                                        const QString &, QString *)>(&Secrets::storeSecret);
    auto getFn = static_cast<QString (*)(const QString &, const QString &,
                                         QString *)>(&Secrets::retrieveSecret);
    auto delFn = static_cast<bool (*)(const QString &, const QString &,
                                      QString *)>(&Secrets::deleteSecret);
    auto keyFn = static_cast<QString (*)(const QString &)>(&Secrets::aiApiKey);
    CHECK(storeFn != nullptr && getFn != nullptr && delFn != nullptr && keyFn != nullptr);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testGroupManager();
    testEmbeddedCommandsResource();
    testFrequentlyUsedCommands();
    testConfigUtil();
    testGlobalState();
    testSecretsCompileLevel();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
