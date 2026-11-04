// NginxConfigParser 单元测试（纯逻辑，无 IO / 网络）。
// 覆盖：词法（注释/引号/转义）、指令树、server 抽取（含 http 包裹下沉）、
// listen/server_name/root/location 摘要、renderSite ↔ parseServers 往返、
// fillSpecFromServer 的未知指令保真、错误行号、quoteArg。

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QStringList>

#include "nginx/NginxConfig.h"
#include "nginx/NginxConfigParser.h"

using namespace cubeshell::nginx;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testParseBasics()
{
    QList<Directive> top;
    QString err;
    const QString cfg = QStringLiteral(
        "worker_processes 4;\n"
        "http {\n"
        "    include mime.types;\n"
        "    server {\n"
        "        listen 80;\n"
        "        server_name a.com;\n"
        "        location / { root /var/www; }\n"
        "    }\n"
        "}\n");
    CHECK(NginxConfigParser::parse(cfg, &top, &err));
    CHECK(err.isEmpty());
    CHECK(top.size() == 2);
    CHECK(top.at(0).name == QStringLiteral("worker_processes"));
    CHECK(top.at(0).args == QStringList{QStringLiteral("4")});
    CHECK(top.at(1).name == QStringLiteral("http"));
    CHECK(top.at(1).hasBlock);
    CHECK(top.at(1).children.size() == 2);
    CHECK(top.at(1).children.at(1).name == QStringLiteral("server"));
}

static void testCommentsAndQuotes()
{
    const QString cfg = QStringLiteral(
        "# leading comment\n"
        "server_name \"example.com www.example.com\"; # trailing\n"
        "root '/var/www/my site';\n");
    QList<Directive> top;
    QString err;
    CHECK(NginxConfigParser::parse(cfg, &top, &err));
    CHECK(top.size() == 2);
    CHECK(top.at(0).args == QStringList{QStringLiteral("example.com www.example.com")});
    CHECK(top.at(1).args == QStringList{QStringLiteral("/var/www/my site")});

    // 正则里的反斜杠在解析后原样保留。
    QList<Directive> t2;
    QString e2;
    CHECK(NginxConfigParser::parse(QStringLiteral("location ~ \\.php$ {\n}\n"), &t2, &e2));
    CHECK(t2.size() == 1);
    CHECK(t2.at(0).args == (QStringList{QStringLiteral("~"), QStringLiteral("\\.php$")}));
}

static void testListenParsing()
{
    Directive a;
    a.name = QStringLiteral("listen");
    a.args = {QStringLiteral("443"), QStringLiteral("ssl"), QStringLiteral("default_server")};
    CHECK(NginxConfigParser::listenPortsOf(a) == QList<int>{443});
    CHECK(NginxConfigParser::listenHasFlag(a, QStringLiteral("ssl")));
    CHECK(NginxConfigParser::listenHasFlag(a, QStringLiteral("default_server")));

    Directive b;
    b.args = {QStringLiteral("[::]:80")};
    CHECK(NginxConfigParser::listenPortsOf(b) == QList<int>{80});

    Directive c;
    c.args = {QStringLiteral("0.0.0.0:8080")};
    CHECK(NginxConfigParser::listenPortsOf(c) == QList<int>{8080});

    Directive d;
    d.args = {QStringLiteral("80"), QStringLiteral("default_server")};
    CHECK(NginxConfigParser::listenPortsOf(d) == QList<int>{80});
    CHECK(NginxConfigParser::listenHasFlag(d, QStringLiteral("default_server")));
}

static void testParseSite()
{
    const QString cfg = QStringLiteral(
        "server {\n"
        "    listen 80;\n"
        "    listen 443 ssl;\n"
        "    server_name example.com www.example.com;\n"
        "    root /var/www/html;\n"
        "    index index.html index.htm;\n"
        "    ssl_certificate /etc/nginx/ssl/a.crt;\n"
        "    ssl_certificate_key /etc/nginx/ssl/a.key;\n"
        "    location /api/ {\n"
        "        proxy_pass http://127.0.0.1:8080;\n"
        "    }\n"
        "    location / {\n"
        "        try_files $uri $uri/ /index.html;\n"
        "    }\n"
        "}\n");
    const SiteView s = NginxConfigParser::parseSite(cfg, QStringLiteral("/etc/nginx/conf.d/app.conf"));
    CHECK(s.parseOk);
    CHECK(s.serverCount == 1);
    CHECK(s.dirKind == QStringLiteral("conf.d"));
    CHECK(s.fileName == QStringLiteral("app.conf"));
    CHECK(s.serverNames == (QStringList{QStringLiteral("example.com"), QStringLiteral("www.example.com")}));
    CHECK(s.listenPorts.contains(80) && s.listenPorts.contains(443));
    CHECK(s.ssl);
    CHECK(s.isProxy);
    CHECK(s.rootOrTarget == QStringLiteral("http://127.0.0.1:8080"));
    CHECK(s.servers.size() == 1);
    const ServerView &sv = s.servers.first();
    CHECK(sv.root == QStringLiteral("/var/www/html"));
    CHECK(sv.indexFiles == (QStringList{QStringLiteral("index.html"), QStringLiteral("index.htm")}));
    CHECK(sv.sslCertificate == QStringLiteral("/etc/nginx/ssl/a.crt"));
    CHECK(sv.sslCertificateKey == QStringLiteral("/etc/nginx/ssl/a.key"));
    CHECK(sv.locations.size() == 2);
    CHECK(sv.locations.at(0).kind == QStringLiteral("proxy"));
    CHECK(sv.locations.at(0).pattern == QStringLiteral("/api/"));
    CHECK(sv.locations.at(1).kind == QStringLiteral("static"));
}

static void testHttpWrapped()
{
    const QString cfg = QStringLiteral(
        "http {\n"
        "    server {\n"
        "        listen 8080;\n"
        "        server_name wrapped.local;\n"
        "    }\n"
        "}\n");
    const QList<ServerView> servers = NginxConfigParser::parseServers(cfg, QStringLiteral("nginx.conf"));
    CHECK(servers.size() == 1);
    CHECK(servers.first().listenPorts == QList<int>{8080});
    CHECK(servers.first().serverNames == QStringList{QStringLiteral("wrapped.local")});
}

static void testParseErrors()
{
    QList<Directive> top;
    QString err;
    CHECK(!NginxConfigParser::parse(QStringLiteral("server {\n    listen 80;\n"), &top, &err));
    CHECK(!err.isEmpty());

    QList<Directive> t2;
    QString e2;
    CHECK(!NginxConfigParser::parse(QStringLiteral("server {\n    listen 80\n}\n"), &t2, &e2));
    CHECK(!e2.isEmpty());

    QList<Directive> t3;
    QString e3;
    CHECK(!NginxConfigParser::parse(QStringLiteral("}\n"), &t3, &e3));
    CHECK(!e3.isEmpty());
}

static void testRenderRoundTrip()
{
    SiteSpec spec;
    spec.fileName = QStringLiteral("app.conf");
    spec.serverNames = {QStringLiteral("example.com")};
    spec.listenPort = 80;
    spec.root = QStringLiteral("/var/www/myapp/dist");
    spec.indexFiles = {QStringLiteral("index.html")};
    spec.spaFallback = true;
    LocationSpec api;
    api.kind = QStringLiteral("proxy");
    api.pattern = QStringLiteral("/api/");
    api.proxyPass = QStringLiteral("http://127.0.0.1:8080");
    spec.locations.append(api);

    const QString text = NginxConfigParser::renderSite(spec);
    CHECK(text.contains(QStringLiteral("server {")));
    CHECK(text.contains(QStringLiteral("listen 80;")));
    CHECK(text.contains(QStringLiteral("server_name example.com;")));
    CHECK(text.contains(QStringLiteral("root /var/www/myapp/dist;")));
    CHECK(text.contains(QStringLiteral("try_files $uri $uri/ /index.html;")));
    CHECK(text.contains(QStringLiteral("location /api/ {")));
    CHECK(text.contains(QStringLiteral("proxy_pass http://127.0.0.1:8080;")));
    CHECK(text.contains(QString::fromLatin1(kManagedMarker)));

    const QList<ServerView> servers = NginxConfigParser::parseServers(text, QStringLiteral("app.conf"));
    CHECK(servers.size() == 1);
    const ServerView &sv = servers.first();
    CHECK(sv.serverNames == QStringList{QStringLiteral("example.com")});
    CHECK(sv.listenPorts == QList<int>{80});
    CHECK(sv.root == QStringLiteral("/var/www/myapp/dist"));
    CHECK(sv.locations.size() == 2);
    CHECK(sv.locations.at(0).pattern == QStringLiteral("/"));
    CHECK(sv.locations.at(1).pattern == QStringLiteral("/api/"));
    CHECK(sv.locations.at(1).kind == QStringLiteral("proxy"));
}

static void testRenderForceHttps()
{
    SiteSpec s;
    s.serverNames = {QStringLiteral("x.com")};
    s.listenSsl = true;
    s.listenPort = 443;
    s.sslCertificate = QStringLiteral("/c.crt");
    s.sslCertificateKey = QStringLiteral("/c.key");
    s.forceHttps = true;
    const QString text = NginxConfigParser::renderSite(s);
    CHECK(text.contains(QStringLiteral("listen 443 ssl;")));
    CHECK(text.contains(QStringLiteral("return 301 https://$host$request_uri;")));
    const QList<ServerView> servers = NginxConfigParser::parseServers(text, QStringLiteral("x.conf"));
    CHECK(servers.size() == 2);
    CHECK(servers.at(0).listenPorts.contains(80));
    CHECK(servers.at(1).listenSsl);
}

static void testFillPreservesExtras()
{
    const QString cfg = QStringLiteral(
        "server {\n"
        "    listen 80;\n"
        "    server_name keep.local;\n"
        "    client_max_body_size 10m;\n"
        "    error_page 404 /404.html;\n"
        "    location / {\n"
        "        proxy_pass http://backend:9000;\n"
        "        proxy_set_header X-Custom yes;\n"
        "        proxy_read_timeout 120s;\n"
        "    }\n"
        "}\n");
    QList<Directive> top;
    QString err;
    CHECK(NginxConfigParser::parse(cfg, &top, &err));
    const QList<Directive> servers = NginxConfigParser::extractServerDirectives(top);
    CHECK(servers.size() == 1);

    SiteSpec spec;
    CHECK(NginxConfigParser::fillSpecFromServer(servers.first(), &spec));
    CHECK(spec.listenPort == 80);
    CHECK(spec.clientMaxBodySize == QStringLiteral("10m"));
    CHECK(spec.extraServerDirectives.contains(QStringLiteral("error_page 404 /404.html;")));
    CHECK(spec.locations.size() == 1);
    const LocationSpec &loc = spec.locations.first();
    CHECK(loc.kind == QStringLiteral("proxy"));
    CHECK(loc.proxyPass == QStringLiteral("http://backend:9000"));
    CHECK(loc.extra.contains(QStringLiteral("proxy_set_header X-Custom yes;")));
    CHECK(loc.extra.contains(QStringLiteral("proxy_read_timeout 120s;")));

    spec.fileName = QStringLiteral("keep.conf");
    const QString text = NginxConfigParser::renderSite(spec);
    CHECK(text.contains(QStringLiteral("error_page 404 /404.html;")));
    CHECK(text.contains(QStringLiteral("proxy_set_header X-Custom yes;")));
    CHECK(text.contains(QStringLiteral("proxy_read_timeout 120s;")));
}

static void testQuoteArg()
{
    CHECK(NginxConfigParser::quoteArg(QStringLiteral("plain")) == QStringLiteral("plain"));
    CHECK(NginxConfigParser::quoteArg(QStringLiteral("a b")) == QStringLiteral("\"a b\""));
    CHECK(NginxConfigParser::quoteArg(QString()) == QStringLiteral("\"\""));
    CHECK(NginxConfigParser::quoteArg(QStringLiteral("a\"b")) == QStringLiteral("\"a\\\"b\""));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testParseBasics();
    testCommentsAndQuotes();
    testListenParsing();
    testParseSite();
    testHttpWrapped();
    testParseErrors();
    testRenderRoundTrip();
    testRenderForceHttps();
    testFillPreservesExtras();
    testQuoteArg();
    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "nginx_config_parser_test: all checks passed";
    return 0;
}
