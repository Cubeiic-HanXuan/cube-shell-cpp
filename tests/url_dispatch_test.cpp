// URL dispatch unit test: jms:// Base64-JSON v2 parsing (incl. trailing
// slash / stripped padding), legacy query-string fallback, ssh:// and
// cubeshell:// parsing, argv scanning, and BastionClient connection
// parameter construction. Pure logic — no real JumpServer needed.
//
// 对应Python: core/url_dispatch/url_handler.py / bastion_client.py 的行为对照

#include <QCoreApplication>
#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "url_dispatch/BastionClient.h"
#include "url_dispatch/UrlHandler.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// Build a jms:// URL from raw JSON the way JumpServer does (Base64 payload).
static QString makeJmsUrl(const QByteArray &json, bool stripPadding = false)
{
    QByteArray b64 = json.toBase64();
    if (stripPadding) {
        while (b64.endsWith('='))
            b64.chop(1);
    }
    return QStringLiteral("jms://") + QString::fromLatin1(b64);
}

// Field layout mirrors the real JumpServer v2 payload (see project docs:
// version/id/value/name/protocol + token{id,value} + asset{...} + endpoint{host,port}).
static const char kFullJson[] =
    "{\"version\":2,\"id\":\"tok-outer\",\"value\":\"val-outer\","
    "\"name\":\"web-1\",\"protocol\":\"sftp\","
    "\"token\":{\"id\":\"abc123\",\"value\":\"secret-pw\"},"
    "\"asset\":{\"id\":\"a-1\",\"name\":\"web服务器\",\"address\":\"10.0.0.8\"},"
    "\"endpoint\":{\"host\":\"jms.example.com\",\"port\":2222}}";

static void testJmsV2Basic()
{
    const UrlConnectionInfo info = parseJmsUrl(makeJmsUrl(kFullJson));
    CHECK(info.valid);
    CHECK(info.scheme == QStringLiteral("jms"));
    CHECK(info.host == QStringLiteral("jms.example.com"));
    CHECK(info.port == 2222);
    CHECK(info.user == QStringLiteral("JMS-abc123"));         // "JMS-" + token.id
    CHECK(info.password == QStringLiteral("secret-pw"));      // token.value
    CHECK(info.assetName == QString::fromUtf8("web服务器"));   // asset.name
    CHECK(info.protocol == QStringLiteral("sftp"));
    CHECK(info.token.isEmpty() && info.server.isEmpty());     // v2 has no legacy fields
}

static void testJmsV2Boundaries()
{
    // 浏览器自动追加的尾部斜杠（单个和多个）必须被剥离后再解 Base64
    const QString base = makeJmsUrl(kFullJson);
    UrlConnectionInfo a = parseJmsUrl(base + QStringLiteral("/"));
    CHECK(a.valid && a.host == QStringLiteral("jms.example.com"));
    UrlConnectionInfo b = parseJmsUrl(base + QStringLiteral("//"));
    CHECK(b.valid && b.user == QStringLiteral("JMS-abc123"));

    // Base64 padding 被省略时需自动补齐
    UrlConnectionInfo c = parseJmsUrl(makeJmsUrl(kFullJson, /*stripPadding=*/true));
    CHECK(c.valid && c.password == QStringLiteral("secret-pw"));

    // endpoint 无 port -> 默认 2222；无 protocol -> "ssh"；port 为字符串也接受
    const QByteArray noPort =
        "{\"token\":{\"id\":\"t\",\"value\":\"v\"},"
        "\"endpoint\":{\"host\":\"h1\"}}";
    UrlConnectionInfo d = parseJmsUrl(makeJmsUrl(noPort));
    CHECK(d.valid && d.port == 2222 && d.protocol == QStringLiteral("ssh"));
    CHECK(d.assetName.isEmpty()); // asset 缺失 -> asset_name 为空

    const QByteArray strPort =
        "{\"token\":{\"id\":\"t\",\"value\":\"v\"},"
        "\"endpoint\":{\"host\":\"h1\",\"port\":\"2223\"}}";
    UrlConnectionInfo e = parseJmsUrl(makeJmsUrl(strPort));
    CHECK(e.valid && e.port == 2223);

    // 非 jms:// 前缀
    UrlConnectionInfo f = parseJmsUrl(QStringLiteral("http://example.com"));
    CHECK(!f.valid);
}

static void testJmsBase64ErrorPaths()
{
    // 非法 Base64（含非字母表字符，且无 '?'）：Python 走 legacy 回退，
    // 得到一个全空 dict —— host 为空但解析本身不失败、绝不崩溃。
    UrlConnectionInfo a = parseJmsUrl(QStringLiteral("jms://!!!not-base64!!!"));
    CHECK(a.valid && a.host.isEmpty() && a.token.isEmpty());
    CHECK(a.port == 22 && a.protocol == QStringLiteral("ssh"));
    // 空 host 在 BastionClient 层被拒绝并给出明确错误
    BastionConnectParams pa = BastionClient::resolveConnectParams(a);
    CHECK(!pa.valid && !pa.error.isEmpty());

    // 合法 Base64 但不是 JSON -> soft fail -> legacy 回退（同样全空）
    UrlConnectionInfo b = parseJmsUrl(
        QStringLiteral("jms://") + QString::fromLatin1(QByteArray("hello world").toBase64()));
    CHECK(b.valid && b.host.isEmpty());

    // 合法 Base64 JSON 但缺 endpoint/token -> soft fail -> legacy 回退
    UrlConnectionInfo c = parseJmsUrl(makeJmsUrl("{\"foo\":1}"));
    CHECK(c.valid && c.host.isEmpty());

    // v2 结构性缺字段（endpoint/token 存在但缺 host / id / value）
    // -> 对应 Python KeyError 逃逸 -> 整体解析失败，且带明确错误信息
    UrlConnectionInfo d = parseJmsUrl(makeJmsUrl(
        "{\"token\":{\"id\":\"t\",\"value\":\"v\"},\"endpoint\":{\"port\":2222}}"));
    CHECK(!d.valid && !d.error.isEmpty());
    UrlConnectionInfo e = parseJmsUrl(makeJmsUrl(
        "{\"token\":{\"id\":\"t\"},\"endpoint\":{\"host\":\"h\"}}"));
    CHECK(!e.valid && !e.error.isEmpty());

    // port 非数字 -> 对应 Python int() ValueError -> 整体解析失败
    UrlConnectionInfo f = parseJmsUrl(makeJmsUrl(
        "{\"token\":{\"id\":\"t\",\"value\":\"v\"},"
        "\"endpoint\":{\"host\":\"h\",\"port\":\"oops\"}}"));
    CHECK(!f.valid && !f.error.isEmpty());

    // endpoint/token 类型错误（不是对象）
    UrlConnectionInfo g = parseJmsUrl(makeJmsUrl(
        "{\"token\":\"abc\",\"endpoint\":\"def\"}"));
    CHECK(!g.valid && !g.error.isEmpty());
}

static void testJmsLegacyQueryString()
{
    // token 模式（旧格式）：jms://ssh?token=xxx&server=xxx
    UrlConnectionInfo a = parseJmsUrl(
        QStringLiteral("jms://ssh?token=tok-1&server=https://jms.example.com"));
    CHECK(a.valid);
    CHECK(a.token == QStringLiteral("tok-1"));
    CHECK(a.server == QStringLiteral("https://jms.example.com"));
    CHECK(a.host.isEmpty() && a.port == 22);
    CHECK(a.protocol == QStringLiteral("ssh"));

    // 直连字段 + asset 路径 + 百分号/加号解码 + account 兜底 user
    UrlConnectionInfo b = parseJmsUrl(QStringLiteral(
        "jms://asset/uuid-42?host=1.2.3.4&port=2200&account=root&password=p%40ss+1"));
    CHECK(b.valid);
    CHECK(b.assetId == QStringLiteral("uuid-42"));
    CHECK(b.host == QStringLiteral("1.2.3.4") && b.port == 2200);
    CHECK(b.user == QStringLiteral("root"));                 // user 缺失时取 account
    CHECK(b.password == QStringLiteral("p@ss 1"));           // %40 -> @, + -> 空格

    // parse_qs 跳过空值：token= 空 -> 视为缺失
    UrlConnectionInfo c = parseJmsUrl(
        QStringLiteral("jms://ssh?token=&server=https://jms.example.com"));
    CHECK(c.valid && c.token.isEmpty());

    // 尾部斜杠对 legacy 也生效
    UrlConnectionInfo d = parseJmsUrl(
        QStringLiteral("jms://ssh?token=tok-2&server=https://jms.example.com/"));
    CHECK(d.valid && d.server == QStringLiteral("https://jms.example.com"));

    // port 非数字 -> 解析失败（对应 int() ValueError）
    UrlConnectionInfo e = parseJmsUrl(QStringLiteral("jms://ssh?host=h&port=abc"));
    CHECK(!e.valid && !e.error.isEmpty());
}

static void testSshUrl()
{
    UrlConnectionInfo a = parseSshUrl(QStringLiteral("ssh://root@10.0.0.1:2200"));
    CHECK(a.valid && a.host == QStringLiteral("10.0.0.1") && a.port == 2200);
    CHECK(a.user == QStringLiteral("root") && a.password.isEmpty());

    // user:password + 百分号解码
    UrlConnectionInfo b = parseSshUrl(QStringLiteral("ssh://my%20user:p%40ss@host.example.com"));
    CHECK(b.valid && b.port == 22);
    CHECK(b.user == QStringLiteral("my user"));
    CHECK(b.password == QStringLiteral("p@ss"));

    // 仅 host
    UrlConnectionInfo c = parseSshUrl(QStringLiteral("ssh://host-only"));
    CHECK(c.valid && c.host == QStringLiteral("host-only") && c.user.isEmpty());

    // 无 host -> 失败
    UrlConnectionInfo d = parseSshUrl(QStringLiteral("ssh://"));
    CHECK(!d.valid && !d.error.isEmpty());
    UrlConnectionInfo e = parseSshUrl(QStringLiteral("jms://abc"));
    CHECK(!e.valid);
}

static void testCubeshellUrl()
{
    const QString dir = QDir::tempPath(); // 一定存在的目录
    UrlConnectionInfo a = parseCubeshellUrl(
        QStringLiteral("cubeshell://open-local?path=%1&command=claude%20--resume%20xyz")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(dir))));
    CHECK(a.valid);
    CHECK(a.action == QStringLiteral("open-local"));
    CHECK(a.path == dir);
    CHECK(a.command == QStringLiteral("claude --resume xyz"));

    // 缺 path 参数 / 路径不存在 -> 失败
    UrlConnectionInfo b = parseCubeshellUrl(QStringLiteral("cubeshell://open-local"));
    CHECK(!b.valid && !b.error.isEmpty());
    UrlConnectionInfo c = parseCubeshellUrl(
        QStringLiteral("cubeshell://open-local?path=/no/such/dir/xyz-42"));
    CHECK(!c.valid && !c.error.isEmpty());
}

static void testParseUrlDispatchAndArgvScan()
{
    CHECK(parseUrl(makeJmsUrl(kFullJson)).valid);
    CHECK(parseUrl(QStringLiteral("ssh://root@h")).valid);
    CHECK(!parseUrl(QStringLiteral("ftp://h")).valid); // unsupported scheme

    // argv 扫描：裸 URL、-url flag、无 URL
    UrlConnectionInfo a = scanArgvForUrl(
        {QStringLiteral("cube-shell"), QStringLiteral("--foo"), makeJmsUrl(kFullJson)});
    CHECK(a.valid && a.host == QStringLiteral("jms.example.com"));
    UrlConnectionInfo b = scanArgvForUrl(
        {QStringLiteral("cube-shell"), QStringLiteral("-url"), QStringLiteral("ssh://u@h:23")});
    CHECK(b.valid && b.port == 23);
    UrlConnectionInfo c = scanArgvForUrl({QStringLiteral("cube-shell"), QStringLiteral("-v")});
    CHECK(!c.valid);
}

#ifdef CUBESHELL_WITH_RDP
// rdp:// / rdp+ntlm-password:// 解析（对应 build_rdp_url 的两种 scheme）。
static void testRdpUrl()
{
    // 明文 scheme：user:pwd@host:port，密码 percent-decode
    UrlConnectionInfo a = parseRdpUrl(
        QStringLiteral("rdp://Administrator:P%40ssw0rd@10.0.0.5:3390"));
    CHECK(a.valid && a.scheme == QStringLiteral("rdp"));
    CHECK(a.host == QStringLiteral("10.0.0.5") && a.port == 3390);
    CHECK(a.user == QStringLiteral("Administrator"));
    CHECK(a.password == QStringLiteral("P@ssw0rd"));
    CHECK(a.domain.isEmpty() && a.protocol == QStringLiteral("rdp"));

    // NTLM scheme + DOMAIN\user（裸反斜杠，build_rdp_url 的 quote safe="\\" 形式）
    UrlConnectionInfo b = parseRdpUrl(QStringLiteral(
        "rdp+ntlm-password://CORP\\admin:pw@192.168.1.100:3389"));
    CHECK(b.valid && b.host == QStringLiteral("192.168.1.100") && b.port == 3389);
    CHECK(b.domain == QStringLiteral("CORP") && b.user == QStringLiteral("admin"));
    CHECK(b.password == QStringLiteral("pw"));

    // 端口缺省 3389；IPv6 主机方括号剥除
    UrlConnectionInfo c = parseRdpUrl(QStringLiteral("rdp://10.10.10.2"));
    CHECK(c.valid && c.port == 3389 && c.user.isEmpty());
    UrlConnectionInfo d = parseRdpUrl(QStringLiteral("rdp://u:p@[fe80::1]:3389"));
    CHECK(d.valid && d.host == QStringLiteral("fe80::1"));

    // 无 host / 非 rdp scheme -> 失败
    CHECK(!parseRdpUrl(QStringLiteral("rdp://")).valid);
    CHECK(!parseRdpUrl(QStringLiteral("ssh://u@h")).valid);

    // parseUrl 分发 + argv 扫描认得 rdp:// 与 rdp+
    CHECK(parseUrl(QStringLiteral("rdp://u@h")).valid);
    CHECK(parseUrl(QStringLiteral("rdp+ntlm-password://u@h")).valid);
    UrlConnectionInfo e = scanArgvForUrl(
        {QStringLiteral("cube-shell"), QStringLiteral("rdp://u@h:3390")});
    CHECK(e.valid && e.port == 3390);
}
#endif // CUBESHELL_WITH_RDP

static void testBastionConnectParams()
{
    // v2 结果直通：host/port/user/password 原样，tab 名 "user@host"
    const UrlConnectionInfo v2 = parseJmsUrl(makeJmsUrl(kFullJson));
    BastionConnectParams a = BastionClient::resolveConnectParams(v2);
    CHECK(a.valid);
    CHECK(a.host == QStringLiteral("jms.example.com") && a.port == 2222);
    CHECK(a.user == QStringLiteral("JMS-abc123"));
    CHECK(a.password == QStringLiteral("secret-pw"));
    CHECK(a.tabName == QStringLiteral("JMS-abc123@jms.example.com"));

    // legacy token 模式：host 取 server 的 hostname，port 固定 2222（koko），
    // user = "JMS-<token>"，password 置空
    const UrlConnectionInfo legacy = parseJmsUrl(
        QStringLiteral("jms://ssh?token=tok-9&server=https://jms.corp.cn:443"));
    BastionConnectParams b = BastionClient::resolveConnectParams(legacy);
    CHECK(b.valid);
    CHECK(b.host == QStringLiteral("jms.corp.cn") && b.port == 2222);
    CHECK(b.user == QStringLiteral("JMS-tok-9") && b.password.isEmpty());
    CHECK(b.tabName == QStringLiteral("JMS-tok-9@jms.corp.cn"));

    // server 无法解析出 hostname -> 明确失败
    UrlConnectionInfo badServer = legacy;
    badServer.server = QStringLiteral("not a url");
    CHECK(!BastionClient::resolveConnectParams(badServer).valid);

    // 无 host 且非 token 模式 -> 跳过
    UrlConnectionInfo noHost;
    noHost.valid = true;
    CHECK(!BastionClient::resolveConnectParams(noHost).valid);

    // user 为空时 tab 名只有 host
    UrlConnectionInfo hostOnly;
    hostOnly.valid = true;
    hostOnly.host = QStringLiteral("10.0.0.9");
    BastionConnectParams c = BastionClient::resolveConnectParams(hostOnly);
    CHECK(c.valid && c.tabName == QStringLiteral("10.0.0.9"));

    // handle_url + connectRequested 信号：合法 jms URL 触发一次连接请求
    BastionClient client;
    int emitted = 0;
    BastionConnectParams got;
    QObject::connect(&client, &BastionClient::connectRequested,
                     [&](const BastionConnectParams &p) { ++emitted; got = p; });
    CHECK(client.handleUrl(makeJmsUrl(kFullJson)));
    CHECK(emitted == 1 && got.host == QStringLiteral("jms.example.com"));
    // cubeshell:// 不属于 handle_url 的职责（Python 只处理 jms/ssh）
    CHECK(!client.handleUrl(QStringLiteral("cubeshell://open-local?path=/tmp")));
    CHECK(emitted == 1);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testJmsV2Basic();
    testJmsV2Boundaries();
    testJmsBase64ErrorPaths();
    testJmsLegacyQueryString();
    testSshUrl();
    testCubeshellUrl();
    testParseUrlDispatchAndArgvScan();
#ifdef CUBESHELL_WITH_RDP
    testRdpUrl();
#endif
    testBastionConnectParams();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
