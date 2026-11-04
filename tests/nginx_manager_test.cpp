// NginxManager 纯逻辑单测（无 nginx / 网络）：列表输出解析（sites-enabled 去重
// + 启用态判定）、shellQuote、命令拼装（可用性探测 / 启停 / 校验）。

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QStringList>

#include "nginx/NginxConfig.h"
#include "nginx/NginxManager.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testParseListing()
{
    const QString out = QStringLiteral(
        "@@@CUBESHELL_FILE:/etc/nginx/conf.d/app.conf@@@\n"
        "server { listen 80; server_name a.com; }\n"
        "\n"
        "@@@CUBESHELL_FILE:/etc/nginx/sites-available/site1@@@\n"
        "server { listen 8080; server_name s1.local; }\n"
        "\n"
        "@@@CUBESHELL_FILE:/etc/nginx/sites-available/site2@@@\n"
        "server { listen 8081; server_name s2.local; }\n"
        "\n"
        "@@@CUBESHELL_FILE:/etc/nginx/sites-enabled/site1@@@\n"
        "server { listen 8080; server_name s1.local; }\n"
        "\n"
        "@@@CUBESHELL_FILE:/etc/nginx/sites-enabled/standalone@@@\n"
        "server { listen 9000; server_name lone.local; }\n"
        "\n"
        "@@@CUBESHELL_ENABLED@@@\n"
        "site1\n");
    const QList<nginx::SiteView> sites = NginxManager::parseListing(out);
    // app.conf(conf.d) + site1(available,enabled) + site2(available,disabled)
    // + standalone(enabled, 无 available 对应) —— sites-enabled/site1 被去重。
    CHECK(sites.size() == 4);

    bool sawApp = false, sawS1 = false, sawS2 = false, sawLone = false;
    for (const nginx::SiteView &s : sites) {
        if (s.fileName == QStringLiteral("app.conf")) {
            sawApp = true;
            CHECK(s.dirKind == QStringLiteral("conf.d"));
            CHECK(s.enabled);
            CHECK(s.serverCount == 1);
            CHECK(s.serverNames == QStringList{QStringLiteral("a.com")});
        } else if (s.fileName == QStringLiteral("site1")) {
            sawS1 = true;
            CHECK(s.dirKind == QStringLiteral("sites-available"));
            CHECK(s.enabled); // 在 sites-enabled 里
            CHECK(s.listenPorts == QList<int>{8080});
        } else if (s.fileName == QStringLiteral("site2")) {
            sawS2 = true;
            CHECK(!s.enabled); // 未启用
        } else if (s.fileName == QStringLiteral("standalone")) {
            sawLone = true;
            CHECK(s.enabled);
            CHECK(s.dirKind == QStringLiteral("sites-enabled"));
        }
    }
    CHECK(sawApp && sawS1 && sawS2 && sawLone);
    CHECK(NginxManager::parseListing(QString()).isEmpty());
}

static void testShellQuote()
{
    CHECK(NginxManager::shellQuote(QStringLiteral("/etc/nginx")) == QStringLiteral("'/etc/nginx'"));
    CHECK(NginxManager::shellQuote(QStringLiteral("a b")) == QStringLiteral("'a b'"));
    CHECK(NginxManager::shellQuote(QStringLiteral("it's")) == QStringLiteral("'it'\\''s'"));
    CHECK(NginxManager::siteFileName(QStringLiteral("/etc/nginx/conf.d/app.conf"))
          == QStringLiteral("app.conf"));
}

static void testCommandBuilders()
{
    NginxManager m;
    m.setNginxBin(QStringLiteral("/usr/sbin/nginx"));
    m.setConfDir(QStringLiteral("/etc/nginx"));
    m.setLogDir(QStringLiteral("/var/log/nginx"));

    // 可用性探测：包含解析标记与常见候选路径，且用 command -v 兜底。
    const QString avail = m.buildAvailabilityCommand();
    CHECK(avail.contains(QString::fromLatin1(NginxManager::kBinMarker)));
    CHECK(avail.contains(QStringLiteral("/usr/sbin/nginx")));
    CHECK(avail.contains(QStringLiteral("command -v")));

    // 启停：systemctl 优先，失败回退 nginx 二进制（用解析出的绝对路径）。
    CHECK(m.buildStartCommand().contains(QStringLiteral("systemctl start nginx")));
    CHECK(m.buildStopCommand().contains(QStringLiteral("systemctl stop nginx")));
    CHECK(m.buildStartCommand().contains(QStringLiteral("'/usr/sbin/nginx'")));
    CHECK(m.buildStopCommand().contains(QStringLiteral("-s stop")));
    CHECK(m.buildRestartCommand().contains(QStringLiteral("systemctl restart nginx")));

    // 校验：confDir 非默认时带 -c。
    CHECK(m.buildTestCommand().contains(QStringLiteral("-t")));

    CHECK(m.logFilePath(QStringLiteral("error")).endsWith(QStringLiteral("/error.log")));
    CHECK(m.logFilePath(QStringLiteral("access")).endsWith(QStringLiteral("/access.log")));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testParseListing();
    testShellQuote();
    testCommandBuilders();
    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "nginx_manager_test: all checks passed";
    return 0;
}
