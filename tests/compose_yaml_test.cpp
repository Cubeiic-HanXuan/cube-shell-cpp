// Docker module unit test (ComposeYaml): parseCompose structure + YAML 1.1
// scalar semantics (plain vs quoted), invalid input handling, dumpCompose ->
// parseCompose round-trip (semantic equality — dump reorders keys by fixed
// priority, so we never compare text), environment dict / "k=v" list dual
// format, command string/list dual format, and loadComposeServices against
// the real conf/docker-compose-full.yml (file order + labels.description).

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariantList>
#include <QVariantMap>

#include "docker/ComposeYaml.h"

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

static void testParseBasicStructure()
{
    QString err;
    const QVariantMap root = ComposeYaml::parseCompose(QStringLiteral(
        "version: '3.5'\n"
        "services:\n"
        "  web:\n"
        "    image: nginx:latest\n"
        "    ports:\n"
        "    - 80:80\n"
        "    - \"443:443\"\n"
        "    depends_on:\n"
        "    - db\n"
        "  db:\n"
        "    image: mysql:8\n"
        "    environment:\n"
        "      MYSQL_ROOT_PASSWORD: secret\n"
        "networks:\n"
        "  front: {}\n"), &err);
    CHECK(err.isEmpty());
    CHECK(!root.isEmpty());

    // version 加了引号 -> 字符串（不能被吃成 float 3.5）。
    CHECK(root.value(QStringLiteral("version")).typeId() == QMetaType::QString);
    CHECK(root.value(QStringLiteral("version")).toString() == QStringLiteral("3.5"));

    const QVariantMap services = root.value(QStringLiteral("services")).toMap();
    CHECK(services.size() == 2);
    const QVariantMap web = services.value(QStringLiteral("web")).toMap();
    CHECK(web.value(QStringLiteral("image")).toString() == QStringLiteral("nginx:latest"));

    // 嵌套 list：ports 两种写法都是字符串（"80:80" 含冒号不是 int）。
    const QVariantList ports = web.value(QStringLiteral("ports")).toList();
    CHECK(ports.size() == 2);
    CHECK(ports.at(0).toString() == QStringLiteral("80:80"));
    CHECK(ports.at(1).toString() == QStringLiteral("443:443"));
    CHECK(web.value(QStringLiteral("depends_on")).toList()
          == QVariantList{QStringLiteral("db")});

    // 嵌套 map。
    const QVariantMap dbEnv = services.value(QStringLiteral("db")).toMap()
                                  .value(QStringLiteral("environment")).toMap();
    CHECK(dbEnv.value(QStringLiteral("MYSQL_ROOT_PASSWORD")).toString()
          == QStringLiteral("secret"));

    // 空 flow map -> 空 QVariantMap。
    CHECK(root.value(QStringLiteral("networks")).toMap()
              .value(QStringLiteral("front")).toMap().isEmpty());
}

static void testScalarSemantics()
{
    QString err;
    const QVariantMap root = ComposeYaml::parseCompose(QStringLiteral(
        "flags:\n"
        "  plain_yes: yes\n"
        "  plain_on: On\n"
        "  plain_no: no\n"
        "  quoted_yes: 'yes'\n"
        "  quoted_true: \"true\"\n"
        "  int_v: 42\n"
        "  neg_int: -7\n"
        "  hex_v: 0x10\n"
        "  float_v: 0.5\n"
        "  quoted_num: '123'\n"
        "  exp_str: 1e5\n"
        "  nothing: null\n"
        "  tilde: ~\n"
        "  plain_str: hello world\n"), &err);
    CHECK(err.isEmpty());
    const QVariantMap flags = root.value(QStringLiteral("flags")).toMap();

    // YAML 1.1 隐式 bool（yes/on/no...），与 yaml.safe_load 一致。
    CHECK(flags.value(QStringLiteral("plain_yes")).typeId() == QMetaType::Bool);
    CHECK(flags.value(QStringLiteral("plain_yes")).toBool());
    CHECK(flags.value(QStringLiteral("plain_on")).toBool());
    CHECK(flags.value(QStringLiteral("plain_no")).typeId() == QMetaType::Bool);
    CHECK(!flags.value(QStringLiteral("plain_no")).toBool());

    // 引号标量一律字符串，与未引号语义严格区分。
    CHECK(flags.value(QStringLiteral("quoted_yes")).typeId() == QMetaType::QString);
    CHECK(flags.value(QStringLiteral("quoted_yes")).toString() == QStringLiteral("yes"));
    CHECK(flags.value(QStringLiteral("quoted_true")).typeId() == QMetaType::QString);
    CHECK(flags.value(QStringLiteral("quoted_num")).toString() == QStringLiteral("123"));

    // int / hex / float。
    CHECK(flags.value(QStringLiteral("int_v")).typeId() == QMetaType::LongLong);
    CHECK(flags.value(QStringLiteral("int_v")).toLongLong() == 42);
    CHECK(flags.value(QStringLiteral("neg_int")).toLongLong() == -7);
    CHECK(flags.value(QStringLiteral("hex_v")).toLongLong() == 16);
    CHECK(flags.value(QStringLiteral("float_v")).typeId() == QMetaType::Double);
    CHECK(qAbs(flags.value(QStringLiteral("float_v")).toDouble() - 0.5) < 1e-9);

    // PyYAML 的 float 需要小数点："1e5" 是字符串。
    CHECK(flags.value(QStringLiteral("exp_str")).typeId() == QMetaType::QString);
    CHECK(flags.value(QStringLiteral("exp_str")).toString() == QStringLiteral("1e5"));

    // null / ~ -> 无效 QVariant（None）。
    CHECK(flags.contains(QStringLiteral("nothing")));
    CHECK(!flags.value(QStringLiteral("nothing")).isValid());
    CHECK(!flags.value(QStringLiteral("tilde")).isValid());

    CHECK(flags.value(QStringLiteral("plain_str")).toString()
          == QStringLiteral("hello world"));
}

static void testInvalidYaml()
{
    // 非法 YAML -> 空 map + errorOut 非空。
    QString err;
    CHECK(ComposeYaml::parseCompose(QStringLiteral("services: [\n  unclosed"), &err).isEmpty());
    CHECK(!err.isEmpty());

    // 根节点不是映射 -> 空 map + errorOut 非空。
    err.clear();
    CHECK(ComposeYaml::parseCompose(QStringLiteral("- a\n- b\n"), &err).isEmpty());
    CHECK(!err.isEmpty());

    // 空文档 -> 空 map，不算错误（Python 侧 `or {}`）。
    err.clear();
    CHECK(ComposeYaml::parseCompose(QString(), &err).isEmpty());
    CHECK(err.isEmpty());
}

static void testDumpRoundTrip()
{
    QString err;
    const QVariantMap m1 = ComposeYaml::parseCompose(QStringLiteral(
        "version: '3.5'\n"
        "services:\n"
        "  redis:\n"
        "    image: redis:latest\n"
        "    restart: always\n"
        "    privileged: yes\n"
        "    mem_limit: 512\n"
        "    cpus: 0.5\n"
        "    command: redis-server --requirepass foo\n"
        "    ports:\n"
        "    - \"6379:6379\"\n"
        "    environment:\n"
        "      REDIS_ARGS: 'yes'\n"
        "      LEVEL: 3\n"
        "    labels:\n"
        "      description: \"内存数据库\"\n"
        "    extra: ~\n"
        "networks:\n"
        "  cube-test: {}\n"), &err);
    CHECK(err.isEmpty());
    CHECK(!m1.isEmpty());

    // dump 后回读：字段不丢、值语义相等（dump 按固定优先级排序，不比文本）。
    const QString text2 = ComposeYaml::dumpCompose(m1);
    CHECK(text2.endsWith(QLatin1Char('\n')));
    err.clear();
    const QVariantMap m2 = ComposeYaml::parseCompose(text2, &err);
    CHECK(err.isEmpty());
    CHECK(m2.keys() == m1.keys());
    CHECK(m2 == m1);

    // 字符串 "yes" 必须带引号输出，否则回读会变 bool（往返保真的关键）。
    CHECK(text2.contains(QStringLiteral("\"yes\"")));
    const QVariantMap redis2 = m2.value(QStringLiteral("services")).toMap()
                                   .value(QStringLiteral("redis")).toMap();
    CHECK(redis2.value(QStringLiteral("privileged")).typeId() == QMetaType::Bool);
    CHECK(redis2.value(QStringLiteral("environment")).toMap()
              .value(QStringLiteral("REDIS_ARGS")).typeId() == QMetaType::QString);
    CHECK(redis2.value(QStringLiteral("environment")).toMap()
              .value(QStringLiteral("LEVEL")).toLongLong() == 3);

    // allow_unicode=True 等价：中文原样输出。
    CHECK(text2.contains(QStringLiteral("内存数据库")));
}

static void testEnvironmentDualFormat()
{
    QString err;
    const QVariantMap root = ComposeYaml::parseCompose(QStringLiteral(
        "services:\n"
        "  a:\n"
        "    environment:\n"
        "      DEBUG: yes\n"
        "      PORT: 8080\n"
        "  b:\n"
        "    environment:\n"
        "    - STORE_MODE=db\n"
        "    - SEATA_IP=10.0.0.74\n"), &err);
    CHECK(err.isEmpty());
    const QVariantMap services = root.value(QStringLiteral("services")).toMap();

    // dict 形式。
    const QVariant envA = services.value(QStringLiteral("a")).toMap()
                              .value(QStringLiteral("environment"));
    CHECK(envA.typeId() == QMetaType::QVariantMap);
    CHECK(envA.toMap().value(QStringLiteral("DEBUG")).toBool());
    CHECK(envA.toMap().value(QStringLiteral("PORT")).toLongLong() == 8080);

    // "k=v" 列表形式。
    const QVariant envB = services.value(QStringLiteral("b")).toMap()
                              .value(QStringLiteral("environment"));
    CHECK(envB.typeId() == QMetaType::QVariantList);
    CHECK(envB.toList().size() == 2);
    CHECK(envB.toList().at(0).toString() == QStringLiteral("STORE_MODE=db"));
    CHECK(envB.toList().at(1).toString() == QStringLiteral("SEATA_IP=10.0.0.74"));
}

static void testCommandDualFormat()
{
    QString err;
    const QVariantMap root = ComposeYaml::parseCompose(QStringLiteral(
        "services:\n"
        "  r:\n"
        "    command: redis-server --requirepass foo\n"
        "  s:\n"
        "    command: [\"redis-server\", \"--appendonly\", \"yes\"]\n"), &err);
    CHECK(err.isEmpty());
    const QVariantMap services = root.value(QStringLiteral("services")).toMap();

    // 字符串形式。
    const QVariant cmdR = services.value(QStringLiteral("r")).toMap()
                              .value(QStringLiteral("command"));
    CHECK(cmdR.typeId() == QMetaType::QString);
    CHECK(cmdR.toString() == QStringLiteral("redis-server --requirepass foo"));

    // 列表形式（引号内 "yes" 保持字符串）。
    const QVariant cmdS = services.value(QStringLiteral("s")).toMap()
                              .value(QStringLiteral("command"));
    CHECK(cmdS.typeId() == QMetaType::QVariantList);
    CHECK(cmdS.toList().size() == 3);
    CHECK(cmdS.toList().at(2).typeId() == QMetaType::QString);
    CHECK(cmdS.toList().at(2).toString() == QStringLiteral("yes"));
}

static void testLoadComposeServices()
{
    // 真实内置清单：conf/docker-compose-full.yml。
    QString err;
    const QList<ComposeYaml::ComposeServiceInfo> services =
        ComposeYaml::loadComposeServices(
            confDir() + QStringLiteral("/docker-compose-full.yml"), &err);
    CHECK(err.isEmpty());
    CHECK(services.size() > 0);

    // 文件书写顺序保留（QVariantMap 会重排 key，这里必须按 Node 顺序）。
    CHECK(services.first().name == QStringLiteral("nacos"));
    CHECK(services.size() > 1 && services.at(1).name == QStringLiteral("seata"));

    // labels.description 提取（map 写法）。
    CHECK(services.first().description == QStringLiteral("用于服务注册和服务发现的中间件"));
    CHECK(services.first().config.value(QStringLiteral("image")).toString()
          == QStringLiteral("nacos/nacos-server:v2.4.3"));

    // labels 的 "k=v" 字符串列表写法同样能提取 description。
    const QString tmp = QDir::temp().filePath(QStringLiteral("cubeshell_compose_labels_test.yml"));
    {
        QFile f(tmp);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral(
            "services:\n"
            "  svc:\n"
            "    image: busybox\n"
            "    labels:\n"
            "    - description=列表写法\n"
            "    - other=x\n").toUtf8());
    }
    err.clear();
    const QList<ComposeYaml::ComposeServiceInfo> listForm =
        ComposeYaml::loadComposeServices(tmp, &err);
    CHECK(err.isEmpty());
    CHECK(listForm.size() == 1);
    CHECK(listForm.first().description == QStringLiteral("列表写法"));
    QFile::remove(tmp);

    // 文件不存在 -> 空列表 + errorOut 非空。
    err.clear();
    CHECK(ComposeYaml::loadComposeServices(QStringLiteral("/no/such/file.yml"), &err).isEmpty());
    CHECK(!err.isEmpty());
}

static void testDefaultComposeFullPath()
{
    // 开发态：CMake 注入的源码树 conf/ 兜底必然命中。
    const QString path = ComposeYaml::defaultComposeFullPath();
    CHECK(!path.isEmpty());
    CHECK(path.endsWith(QStringLiteral("docker-compose-full.yml")));
    CHECK(QFileInfo::exists(path));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testParseBasicStructure();
    testScalarSemantics();
    testInvalidYaml();
    testDumpRoundTrip();
    testEnvironmentDualFormat();
    testCommandDualFormat();
    testLoadComposeServices();
    testDefaultComposeFullPath();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
