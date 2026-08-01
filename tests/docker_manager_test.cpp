// Docker module unit test (DockerManager static helpers, pure logic — no
// docker CLI, no network): parsePsJsonLines NDJSON tolerance, parsePipeLines
// "|||" six-field rows, parseComposeLsJson (Name, ConfigFiles) pairs,
// parseComposePsJson NDJSON / JSON-array dual format + Project fallback,
// validateDaemonJson, and the compose file path rule (root vs normal user).
// 分组合并逻辑（refreshGroupedContainers）依赖 executor，属成员方法，此处仅
// 通过上述 parse helper 的组合覆盖其纯逻辑部分。

#include <QCoreApplication>
#include <QDebug>
#include <QPair>
#include <QString>
#include <QStringList>

#include "docker/DockerManager.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testParsePsJsonLines()
{
    // 正常 NDJSON + sudo 提示行 + 非 JSON 行 + 坏 JSON 行混排。
    const QString output = QStringLiteral(
        "{\"ID\":\"abc123\",\"Image\":\"nginx:latest\",\"Names\":\"web\","
        "\"Command\":\"\\\"nginx -g\\\"\",\"Status\":\"Up 3 hours\","
        "\"State\":\"running\",\"Ports\":\"0.0.0.0:80->80/tcp\","
        "\"CreatedAt\":\"2024-01-01 10:00:00\"}\n"
        "[sudo] password for user:\n"
        "{\"ID\":\"def456\",\"Image\":\"mysql:8\",\"Names\":\"db\","
        "\"Command\":\"mysqld\",\"Status\":\"Exited (0) 2 days ago\","
        "\"State\":\"exited\",\"Ports\":\"\",\"CreatedAt\":\"2024-01-02 10:00:00\"}\n"
        "garbage not json\n"
        "{broken json line\n");
    const QList<ContainerInfo> list = DockerManager::parsePsJsonLines(output);
    CHECK(list.size() == 2);
    CHECK(list.at(0).id == QStringLiteral("abc123"));
    CHECK(list.at(0).image == QStringLiteral("nginx:latest"));
    CHECK(list.at(0).names == QStringLiteral("web"));
    CHECK(list.at(0).status == QStringLiteral("Up 3 hours"));
    CHECK(list.at(0).ports == QStringLiteral("0.0.0.0:80->80/tcp"));
    CHECK(list.at(0).isRunning());
    CHECK(list.at(1).id == QStringLiteral("def456"));
    CHECK(list.at(1).state == QStringLiteral("exited"));
    CHECK(!list.at(1).isRunning());

    // State 为空时回退 Status 前缀判断（docker < 20）。
    const QList<ContainerInfo> old = DockerManager::parsePsJsonLines(QStringLiteral(
        "{\"ID\":\"x\",\"Status\":\"Up 5 minutes\",\"State\":\"\"}"));
    CHECK(old.size() == 1 && old.first().isRunning());

    // 空输出。
    CHECK(DockerManager::parsePsJsonLines(QString()).isEmpty());
}

static void testParsePipeLines()
{
    // 六字段正常行 + 少于 6 字段丢弃 + \r\n 容忍 + 空行跳过。
    const QString output = QStringLiteral(
        "abc123|||web|||nginx:latest|||running|||2024-01-01 10:00:00|||0.0.0.0:80->80/tcp\r\n"
        "def456|||db|||mysql:8|||exited|||2024-01-02 10:00:00|||\r\n"
        "\r\n"
        "short|||only|||three\r\n");
    const QList<DockerContainerRow> rows = DockerManager::parsePipeLines(output);
    CHECK(rows.size() == 2);
    CHECK(rows.at(0).id        == QStringLiteral("abc123"));
    CHECK(rows.at(0).name      == QStringLiteral("web"));
    CHECK(rows.at(0).image     == QStringLiteral("nginx:latest"));
    CHECK(rows.at(0).state     == QStringLiteral("running"));
    CHECK(rows.at(0).createdAt == QStringLiteral("2024-01-01 10:00:00"));
    CHECK(rows.at(0).ports     == QStringLiteral("0.0.0.0:80->80/tcp"));
    // 尾字段为空（无端口映射）仍是合法 6 字段行，且 \r 不能残留在字段里。
    CHECK(rows.at(1).ports.isEmpty());
    CHECK(rows.at(1).createdAt == QStringLiteral("2024-01-02 10:00:00"));
    // 独立容器行没有 Project。
    CHECK(rows.at(0).project.isEmpty());

    // 空输出。
    CHECK(DockerManager::parsePipeLines(QString()).isEmpty());
    CHECK(DockerManager::parsePipeLines(QStringLiteral("\n\n")).isEmpty());
}

static void testParseComposeLsJson()
{
    // JSON 数组 -> (Name, ConfigFiles) 保序；空 Name/空 ConfigFiles 条目跳过。
    const QString output = QStringLiteral(
        "[{\"Name\":\"proj1\",\"Status\":\"running(2)\","
        "\"ConfigFiles\":\"/home/app/docker-compose.yml\"},"
        "{\"Name\":\"proj2\",\"Status\":\"exited(1)\","
        "\"ConfigFiles\":\"/opt/stack/docker-compose.yml\"},"
        "{\"Name\":\"broken\",\"ConfigFiles\":\"\"},"
        "{\"Name\":\"\",\"ConfigFiles\":\"/x.yml\"}]");
    const QList<QPair<QString, QString>> projects =
        DockerManager::parseComposeLsJson(output);
    CHECK(projects.size() == 2);
    CHECK(projects.at(0).first  == QStringLiteral("proj1"));
    CHECK(projects.at(0).second == QStringLiteral("/home/app/docker-compose.yml"));
    CHECK(projects.at(1).first  == QStringLiteral("proj2"));
    CHECK(projects.at(1).second == QStringLiteral("/opt/stack/docker-compose.yml"));

    // 前缀提示行容忍：从第一个 '[' 开始解析。
    const QList<QPair<QString, QString>> withPrefix =
        DockerManager::parseComposeLsJson(QStringLiteral(
            "sudo: a password is required\n"
            "[{\"Name\":\"p\",\"ConfigFiles\":\"/a.yml\"}]"));
    CHECK(withPrefix.size() == 1 && withPrefix.first().first == QStringLiteral("p"));

    // 解析失败（旧版 compose 表格输出）-> 空列表回退，不崩。
    CHECK(DockerManager::parseComposeLsJson(QStringLiteral(
        "NAME  STATUS\nproj1  running(2)")).isEmpty());
    CHECK(DockerManager::parseComposeLsJson(QString()).isEmpty());
}

static void testParseComposePsJson()
{
    // NDJSON（compose v2.21+）：Project 键存在则用之，为空串保留空串；
    // ID 空行丢弃；坏行/提示行跳过。
    const QString ndjson = QStringLiteral(
        "{\"ID\":\"aaa\",\"Name\":\"proj-web-1\",\"Image\":\"nginx\","
        "\"State\":\"running\",\"CreatedAt\":\"2024-01-01\","
        "\"Ports\":\"80/tcp\",\"Project\":\"proj\"}\n"
        "{\"ID\":\"bbb\",\"Name\":\"proj-db-1\",\"Image\":\"mysql\","
        "\"State\":\"exited\",\"CreatedAt\":\"2024-01-02\",\"Ports\":\"\","
        "\"Project\":\"\"}\n"
        "{\"ID\":\"\",\"Name\":\"ghost\"}\n"
        "{\"ID\":\"ccc\",\"Name\":\"proj-cache-1\",\"Image\":\"redis\","
        "\"State\":\"running\",\"CreatedAt\":\"2024-01-03\",\"Ports\":\"\"}\n"
        "not a json line\n");
    const QList<DockerContainerRow> rows =
        DockerManager::parseComposePsJson(ndjson, QStringLiteral("fallback"));
    CHECK(rows.size() == 3);
    CHECK(rows.at(0).id == QStringLiteral("aaa"));
    CHECK(rows.at(0).name == QStringLiteral("proj-web-1"));
    CHECK(rows.at(0).project == QStringLiteral("proj"));
    // Project 键存在但为空串 -> 保留空串（get('Project', ...) 语义：键在不回退）。
    CHECK(rows.at(1).project.isEmpty());
    // Project 键缺失 -> 回退项目名。
    CHECK(rows.at(2).id == QStringLiteral("ccc"));
    CHECK(rows.at(2).project == QStringLiteral("fallback"));

    // JSON 数组（compose v2 早期版本）同样容忍。
    const QList<DockerContainerRow> arr = DockerManager::parseComposePsJson(
        QStringLiteral(
            "[{\"ID\":\"x1\",\"Name\":\"a\",\"State\":\"running\"},"
            "{\"ID\":\"\",\"Name\":\"noid\"},"
            "{\"ID\":\"x2\",\"Name\":\"b\",\"State\":\"exited\"}]"),
        QStringLiteral("projA"));
    CHECK(arr.size() == 2);
    CHECK(arr.at(0).id == QStringLiteral("x1"));
    CHECK(arr.at(0).project == QStringLiteral("projA")); // 数组条目无 Project 键
    CHECK(arr.at(1).id == QStringLiteral("x2"));

    // 空输出。
    CHECK(DockerManager::parseComposePsJson(QString(), QStringLiteral("p")).isEmpty());
}

static void testValidateDaemonJson()
{
    // 合法 JSON 对象通过。
    QString err;
    CHECK(DockerManager::validateDaemonJson(QStringLiteral(
        "{\"registry-mirrors\": [\"https://mirror.example.com\"]}"), &err));
    CHECK(err.isEmpty());
    CHECK(DockerManager::validateDaemonJson(QStringLiteral("{}"), &err));

    // UI 提供的默认配置本身必须合法。
    CHECK(DockerManager::validateDaemonJson(DockerManager::defaultDaemonJson(), &err));

    // 非法 JSON。
    err.clear();
    CHECK(!DockerManager::validateDaemonJson(QStringLiteral("{bad json"), &err));
    CHECK(!err.isEmpty());

    // 合法 JSON 但根不是对象。
    err.clear();
    CHECK(!DockerManager::validateDaemonJson(QStringLiteral("[1, 2, 3]"), &err));
    CHECK(!err.isEmpty());
    err.clear();
    CHECK(!DockerManager::validateDaemonJson(QString(), &err));
    CHECK(!err.isEmpty());
}

static void testComposeFilePath()
{
    DockerManager mgr;

    // 未设置用户（本地模式）与 root 均为 /home/app/。
    CHECK(mgr.composeFilePath() == QStringLiteral("/home/app/docker-compose.yml"));
    mgr.setRemoteUser(QStringLiteral("root"));
    CHECK(mgr.composeDir() == QStringLiteral("/home/app/"));
    CHECK(mgr.composeFilePath() == QStringLiteral("/home/app/docker-compose.yml"));

    // 普通用户 -> /home/<user>/app/。
    mgr.setRemoteUser(QStringLiteral("ubuntu"));
    CHECK(mgr.composeDir() == QStringLiteral("/home/ubuntu/app/"));
    CHECK(mgr.composeFilePath() == QStringLiteral("/home/ubuntu/app/docker-compose.yml"));

    // 显式目录覆盖推导，无尾斜杠自动补齐。
    mgr.setComposeDir(QStringLiteral("/opt/stack"));
    CHECK(mgr.composeFilePath() == QStringLiteral("/opt/stack/docker-compose.yml"));
    mgr.setComposeDir(QStringLiteral("/opt/stack2/"));
    CHECK(mgr.composeFilePath() == QStringLiteral("/opt/stack2/docker-compose.yml"));

    // 清空后回到按用户推导。
    mgr.setComposeDir(QString());
    CHECK(mgr.composeDir() == QStringLiteral("/home/ubuntu/app/"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testParsePsJsonLines();
    testParsePipeLines();
    testParseComposeLsJson();
    testParseComposePsJson();
    testValidateDaemonJson();
    testComposeFilePath();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
