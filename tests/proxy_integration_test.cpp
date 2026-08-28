// 代理功能端到端集成测试（headless）。
//
// 跑的是真链路：SshClient → ProxyConnector（HTTP CONNECT / SOCKS5）/
// ProxyCommandDialer（子进程）/ SshJumpChain（逐跳 direct-tcpip 中继）→ 真 sshd。
// 对端全部由 tests/docker/proxy 里那套 compose 提供（squid / dante / openssh），
// 拉起方式见那个目录里的 up.sh；没拉起来时本测试 exit(2)，CTest 记为 SKIPPED。
//
//   cd tests/docker/proxy && ./up.sh
//   CUBESHELL_PROXY_DOCKER=1 ./build/bin/proxy_integration_test
//
// 为什么必须对真代理服务端而不是自己写一个假的：本轮要验的正是我们发出的报文
// 合不合规。拿自己写的服务端对自己的客户端，两边一起写错就一起通过，测不出东西。
//
// 每个场景的判定都做到"字节真的走通了"这一层——建连成功不算过，要开 pty、发一条
// echo、在回显里读到 marker 才算过。代理链最典型的坏法就是握手看着成功、随后
// 静默掉线（例如把 SSH banner 当成 HTTP 应答头一起吞掉），只看 connectToHost
// 的返回值一律看不出来。

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QString>

#include "config/DeviceConfigStore.h"
#include "config/GlobalState.h"
#include "net/ProxyConfig.h"
#include "net/SocketUtil.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

// --- 计分 -----------------------------------------------------------------

static int g_failures = 0;
static int g_passed   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            qWarning() << "FAIL:" << #cond << "line" << __LINE__;              \
            ++g_failures;                                                      \
        } else {                                                               \
            ++g_passed;                                                        \
        }                                                                      \
    } while (0)

// --- 环境常量 -------------------------------------------------------------
//
// 端口与 tests/docker/proxy/docker-compose.yml 一一对应，改一处要改两处。
// 刻意避开 2222（ssh_integration_test 占着），两套环境可以同时在跑。
namespace {

// 宿主机侧（compose 发布出来的端口）
const char *kHostAddr    = "127.0.0.1";
constexpr quint16 kTargetPort  = 2301;   // 最终目标机
constexpr quint16 kB1Port      = 2311;   // 跳板 1
constexpr quint16 kB2PortHost  = 2312;   // 跳板 2（仅自检用，链路里走内网名）
constexpr quint16 kHttpOpen    = 3328;   // squid 匿名
constexpr quint16 kHttpAuth    = 3329;   // squid 要 Basic 认证
constexpr quint16 kSocksOpen   = 1380;   // dante 匿名
constexpr quint16 kSocksAuth   = 1381;   // dante 要用户名口令

// docker 网络内部名。HTTP/SOCKS5 场景刻意用这些名字而不是 IP：主机名是**交给
// 代理去解析**的（SOCKS5 走 ATYP=DOMAIN，不在本机解析，见 ProxyConnector.cpp），
// 用一个本机根本解析不出来的名字，才能证明这条语义真的成立。
const char *kTargetInner   = "target";
const char *kBastion2Inner = "bastion2";
const char *kBastion3Inner = "bastion3";

// 凭据。sshd 与代理各一套，见对应的 Dockerfile / entrypoint.sh。
const char *kSshUser   = "testuser";
const char *kSshPass   = "testpass123";
const char *kProxyUser = "proxyuser";
const char *kProxyPass = "proxypass";

// 单个场景的建连预算。跳板链会被 effectiveConnectTimeoutMs 按跳数放大，
// 这里给的是"一跳该有多少"的量级。
constexpr int kConnectTimeoutMs = 15000;

// 读回显的上限。本地 docker，正常在几十毫秒内到。
constexpr int kReadTimeoutMs = 10000;

constexpr int kChunk = 32 * 1024;

} // namespace

// --- 通道读写小工具 -------------------------------------------------------

// 把通道读进 acc，直到出现 needle 或超时。
// readChannel 在 EOF / 出错时也返回空且不置 wouldBlock，两者在这里都当"没戏了"
// 处理——真正要区分的场景（对端主动断开）会由调用方的 needle 缺失体现出来。
static bool pumpUntil(SshClient &c, QByteArray &acc, const QByteArray &needle,
                      int timeoutMs = kReadTimeoutMs)
{
    if (acc.contains(needle))
        return true;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        bool wouldBlock = false;
        const QByteArray chunk = c.readChannel(kChunk, &wouldBlock);
        if (!chunk.isEmpty()) {
            acc += chunk;
            if (acc.contains(needle))
                return true;
            continue;
        }
        if (wouldBlock) {
            c.waitReadable(100);
            continue;
        }
        return false;   // EOF / 出错
    }
    return false;
}

// 把当前可读的都丢掉，返回丢掉的字节数。
// 灌大块上行数据时必须夹在中间调：pty 默认开回显，只写不读会把回显堵在对端的
// 发送缓冲里，对端于是读不下我们的输入——单线程测试自己就先死锁了，
// 那不是被测代码的问题。
static qint64 drain(SshClient &c, int budgetMs)
{
    qint64 total = 0;
    QElapsedTimer t;
    t.start();
    for (;;) {
        bool wouldBlock = false;
        const QByteArray chunk = c.readChannel(kChunk, &wouldBlock);
        if (!chunk.isEmpty()) {
            total += chunk.size();
            continue;
        }
        if (!wouldBlock)
            break;                       // EOF
        if (t.elapsed() >= budgetMs)
            break;
        c.waitReadable(10);
        if (t.elapsed() >= budgetMs)
            break;
    }
    return total;
}

// --- 建连 -----------------------------------------------------------------

struct DialSpec {
    QString     host;
    quint16     port = 22;
    ProxyConfig proxy;
    ProxyConfig globalProxy;   // proxy.type == Global 时才用得上
};

static bool dial(SshClient &c, const DialSpec &spec, SshError &err)
{
    c.setHost(spec.host, spec.port);
    c.setUsername(QString::fromLatin1(kSshUser));
    c.setPassword(QString::fromLatin1(kSshPass));
    c.setConnectTimeoutMs(kConnectTimeoutMs);
    c.setProxyConfig(spec.proxy, spec.globalProxy);
    // 本环境全是密码认证，动态码回调不会被调到；给个空实现只是为了满足签名。
    return c.connectToHost([](const QString &, bool) { return QString(); }, err);
}

// 开 pty、发一条 echo、在回显里等 marker。
// marker 里插一对空引号（MARK''ER）：pty 会把我们敲进去的命令行原样回显一遍，
// 不拆开的话那次回显自己就带上了 marker，任何"命令根本没执行"的坏法都测不出来。
static bool shellRoundTrip(SshClient &c, const QString &tag, QString *detail)
{
    SshError err;
    if (!c.openShell(QByteArrayLiteral("xterm-256color"), 80, 24, err)) {
        if (detail)
            *detail = QStringLiteral("openShell 失败：%1").arg(err.message);
        return false;
    }

    const QByteArray marker =
        QByteArrayLiteral("CUBE_OK_") + tag.toLatin1() + QByteArrayLiteral("_")
        + QByteArray::number(QCoreApplication::applicationPid());
    QByteArray sent = marker;
    sent.insert(4, QByteArrayLiteral("''"));   // CUBE''_OK_... → 回显里不含 marker

    // 先把登录横幅/提示符吃掉再发命令：提示符还没出来就写，bash 会连同命令一起
    // 重画，回显里出现半截 marker，判定就飘了。
    drain(c, 400);

    const QByteArray cmd = QByteArrayLiteral("echo ") + sent + QByteArrayLiteral("\n");
    if (c.writeChannel(cmd) != cmd.size()) {
        if (detail)
            *detail = QStringLiteral("writeChannel 写入不完整");
        return false;
    }

    QByteArray acc;
    if (!pumpUntil(c, acc, marker)) {
        if (detail)
            *detail = QStringLiteral("回显里没等到 marker，已读 %1 字节：%2")
                          .arg(acc.size())
                          .arg(QString::fromLatin1(acc.right(200)));
        return false;
    }
    return true;
}

// --- 跳板机名录 -----------------------------------------------------------
//
// 与 MainWindow::publishJumpHostCatalog 推给 GlobalState 的那份快照同构：
// 明文口令已经填好（真实路径上由钥匙串解出来），工作线程只认这一份。
static QList<DeviceEntry> buildCatalog()
{
    auto mk = [](const QString &id, const QString &name,
                 const QString &host, quint16 port) {
        DeviceEntry e;
        e.id       = id;
        e.name     = name;
        e.host     = host;
        e.port     = port;
        e.username = QString::fromLatin1(kSshUser);
        e.password = QString::fromLatin1(kSshPass);
        e.protocol = QStringLiteral("ssh");
        return e;
    };

    QList<DeviceEntry> out;

    // b1：链路的第一跳，由**本机**直接拨号，所以填宿主机可达的地址。
    out << mk(QStringLiteral("b1"), QStringLiteral("跳板一号"),
              QString::fromLatin1(kHostAddr), kB1Port);

    // b2：由 b1 通过 direct-tcpip 去连，地址是**在 b1 上看到的**地址。
    out << mk(QStringLiteral("b2"), QStringLiteral("跳板二号"),
              QString::fromLatin1(kBastion2Inner), 22);

    // b3：同样只在内网可达，而且它自己又挂了一级跳板（b1）。
    // compose 刻意没给 b3 发布宿主机端口——嵌套展平这一项如果实现漏了，
    // 只会失败，不可能"碰巧从宿主机直连上"而假通过。
    DeviceEntry b3 = mk(QStringLiteral("b3"), QStringLiteral("跳板三号"),
                        QString::fromLatin1(kBastion3Inner), 22);
    b3.proxy.type   = ProxyType::JumpHost;
    b3.proxy.hopIds = {QStringLiteral("b1")};
    out << b3;

    // b1viaHttp：第一跳自己躲在 HTTP 代理后面。地址交给 squid 去解析，
    // 所以填内网名（squid 与它同网）。
    DeviceEntry b1h = mk(QStringLiteral("b1viaHttp"), QStringLiteral("跳板一号(经HTTP代理)"),
                         QStringLiteral("bastion1"), 22);
    b1h.proxy.type = ProxyType::Http;
    b1h.proxy.host = QString::fromLatin1(kHostAddr);
    b1h.proxy.port = kHttpOpen;
    out << b1h;

    // 互相引用的一对，用来验环检测。地址随便填，永远走不到拨号那一步。
    DeviceEntry c1 = mk(QStringLiteral("cyc1"), QStringLiteral("环甲"),
                        QStringLiteral("10.255.255.1"), 22);
    c1.proxy.type   = ProxyType::JumpHost;
    c1.proxy.hopIds = {QStringLiteral("cyc2")};
    DeviceEntry c2 = mk(QStringLiteral("cyc2"), QStringLiteral("环乙"),
                        QStringLiteral("10.255.255.2"), 22);
    c2.proxy.type   = ProxyType::JumpHost;
    c2.proxy.hopIds = {QStringLiteral("cyc1")};
    out << c1 << c2;

    return out;
}

// --- 场景表 ---------------------------------------------------------------

struct Scenario {
    QString  name;
    DialSpec spec;
    bool     expectOk = true;
    QString  errorNeedle;   // expectOk == false 时，错误消息里必须出现的片段
};

static ProxyConfig httpProxy(quint16 port, const QString &user = QString(),
                             const QString &pass = QString())
{
    ProxyConfig p;
    p.type     = ProxyType::Http;
    p.host     = QString::fromLatin1(kHostAddr);
    p.port     = port;
    p.username = user;
    p.password = pass;
    return p;
}

static ProxyConfig socksProxy(quint16 port, const QString &user = QString(),
                              const QString &pass = QString())
{
    ProxyConfig p;
    p.type     = ProxyType::Socks5;
    p.host     = QString::fromLatin1(kHostAddr);
    p.port     = port;
    p.username = user;
    p.password = pass;
    return p;
}

static ProxyConfig jumpProxy(const QStringList &hopIds)
{
    ProxyConfig p;
    p.type   = ProxyType::JumpHost;
    p.hopIds = hopIds;
    return p;
}

static QList<Scenario> buildScenarios()
{
    const QString hostAddr   = QString::fromLatin1(kHostAddr);
    const QString innerTarget = QString::fromLatin1(kTargetInner);

    QList<Scenario> s;

    // 基线。这一项失败说明是环境或 sshd 的问题，与代理无关，先看它。
    s << Scenario{QStringLiteral("direct"), {hostAddr, kTargetPort, {}, {}}, true, {}};

    // --- HTTP CONNECT ---
    s << Scenario{QStringLiteral("http-open"),
                  {innerTarget, 22, httpProxy(kHttpOpen), {}}, true, {}};
    s << Scenario{QStringLiteral("http-auth-ok"),
                  {innerTarget, 22,
                   httpProxy(kHttpAuth, QString::fromLatin1(kProxyUser),
                             QString::fromLatin1(kProxyPass)),
                   {}},
                  true, {}};
    // 口令错要给出"407 / 要求认证"这种能自己看懂的话，而不是一句连接失败。
    s << Scenario{QStringLiteral("http-auth-bad"),
                  {innerTarget, 22,
                   httpProxy(kHttpAuth, QString::fromLatin1(kProxyUser),
                             QStringLiteral("wrong-password")),
                   {}},
                  false, QStringLiteral("407")};
    // 完全不给凭据同样要落在 407 那条路径上（而不是卡到超时）。
    s << Scenario{QStringLiteral("http-auth-missing"),
                  {innerTarget, 22, httpProxy(kHttpAuth), {}},
                  false, QStringLiteral("407")};

    // --- SOCKS5 ---
    s << Scenario{QStringLiteral("socks5-open"),
                  {innerTarget, 22, socksProxy(kSocksOpen), {}}, true, {}};
    s << Scenario{QStringLiteral("socks5-auth-ok"),
                  {innerTarget, 22,
                   socksProxy(kSocksAuth, QString::fromLatin1(kProxyUser),
                              QString::fromLatin1(kProxyPass)),
                   {}},
                  true, {}};
    s << Scenario{QStringLiteral("socks5-auth-bad"),
                  {innerTarget, 22,
                   socksProxy(kSocksAuth, QString::fromLatin1(kProxyUser),
                              QStringLiteral("wrong-password")),
                   {}},
                  false, QStringLiteral("认证失败")};
    // 代理要认证而我们没配用户名：这一条走的是方法协商就被拒（05 FF），
    // 与上面"凭据错"是两条不同的代码路径。
    s << Scenario{QStringLiteral("socks5-auth-missing"),
                  {innerTarget, 22, socksProxy(kSocksAuth), {}},
                  false, QStringLiteral("要求认证")};

    // --- 全局代理 ---
    // 显式把全局那一份传进来（真实路径上 connectToHost 会自己去 GlobalState 取）。
    // 验的是 resolveGlobalProxy 这一跳解析确实生效，而不是静默退化成直连——
    // 静默退化在内网里是最难发现的一种坏法。
    {
        Scenario g{QStringLiteral("global→http"),
                   {innerTarget, 22, {}, httpProxy(kHttpOpen)}, true, {}};
        g.spec.proxy.type = ProxyType::Global;
        s << g;
    }

    // --- 系统代理（默认不跑）---
    // 「系统代理」问的是操作系统（macOS 走 SCDynamicStore，不看 http_proxy 这类
    // 环境变量），本进程改不了它，所以这一项只能靠外部先把系统代理指到我们的
    // dante 上，再用环境变量打开：
    //
    //   sudo networksetup -setsocksfirewallproxy Wi-Fi 127.0.0.1 1380
    //   CUBESHELL_PROXY_TEST_SYSTEM=1 ./build/bin/proxy_integration_test
    //   sudo networksetup -setsocksfirewallproxystate Wi-Fi off   # 记得关
    //
    // 不默认打开的理由：开发机上的系统代理通常指着一个本机代理客户端，它对
    // 127.0.0.1 目标往往先回 05 00 再直接关连接——那会让这一项无理由地红，
    // 而问题并不在被测代码里。
    if (qEnvironmentVariableIsSet("CUBESHELL_PROXY_TEST_SYSTEM")) {
        Scenario sys{QStringLiteral("system"), {innerTarget, 22, {}, {}}, true, {}};
        sys.spec.proxy.type = ProxyType::System;
        s << sys;
    }

#ifdef CUBESHELL_WITH_LOCALPROC
    // --- 代理命令 ---
    // 用 nc 当载体：%h/%p 会被 substituteProxyCommand 换掉，等价于
    // OpenSSH 的 ProxyCommand。这条路的载体是子进程 stdin/stdout。
    {
        ProxyConfig p;
        p.type    = ProxyType::Command;
        p.command = QStringLiteral("nc %h %p");
        s << Scenario{QStringLiteral("command"), {hostAddr, kTargetPort, p, {}}, true, {}};
    }
    // 载体起得来但立刻退出：错误消息里得带上子进程的 stderr，否则用户只看到
    // 一句 "Failed getting banner"，无从下手。
    {
        ProxyConfig p;
        p.type    = ProxyType::Command;
        p.command = QStringLiteral("echo cubeshell-carrier-died >&2; exit 3");
        s << Scenario{QStringLiteral("command-dies"), {hostAddr, kTargetPort, p, {}},
                      false, QStringLiteral("cubeshell-carrier-died")};
    }
#endif

    // --- 跳转服务器 ---
    s << Scenario{QStringLiteral("jump-1hop"),
                  {innerTarget, 22, jumpProxy({QStringLiteral("b1")}), {}}, true, {}};
    s << Scenario{QStringLiteral("jump-2hop"),
                  {innerTarget, 22,
                   jumpProxy({QStringLiteral("b1"), QStringLiteral("b2")}), {}},
                  true, {}};
    // b3 自己的跳板是 b1，展平后应当是 [b1, b3]；b3 从宿主机不可达，
    // 所以这一项通过就等于展平真的生效了。
    s << Scenario{QStringLiteral("jump-nested"),
                  {innerTarget, 22, jumpProxy({QStringLiteral("b3")}), {}}, true, {}};
    // 第一跳自己在 HTTP 代理后面：两条代理路径叠在一起。
    s << Scenario{QStringLiteral("jump-hop-behind-http"),
                  {innerTarget, 22, jumpProxy({QStringLiteral("b1viaHttp")}), {}}, true, {}};
    // 成环 / 引用了不存在的设备：要在建连之前就被挡掉，且给出可读原因。
    s << Scenario{QStringLiteral("jump-cycle"),
                  {innerTarget, 22, jumpProxy({QStringLiteral("cyc1")}), {}},
                  false, QStringLiteral("有环")};
    s << Scenario{QStringLiteral("jump-missing"),
                  {innerTarget, 22, jumpProxy({QStringLiteral("no-such-id")}), {}},
                  false, QStringLiteral("已不存在")};
    s << Scenario{QStringLiteral("jump-empty"),
                  {innerTarget, 22, jumpProxy({}), {}},
                  false, QStringLiteral("未选择跳板机")};

    return s;
}

// --- 大流量：中继的两个方向同时打满 --------------------------------------
//
// 这两项是 SshJumpChain 那套非阻塞中继存在的唯一理由。用小 echo 测不出来：
// 单向、几十字节，永远碰不到"一个方向写不下、另一个方向还有数据要搬"这种
// 状态，而那正是阻塞实现必然死锁的地方。
static void testBulkThroughChain()
{
    qInfo() << "--- bulk (jump-2hop) ---";

    SshClient c;
    SshError err;
    DialSpec spec{QString::fromLatin1(kTargetInner), 22,
                  jumpProxy({QStringLiteral("b1"), QStringLiteral("b2")}), {}};
    if (!dial(c, spec, err)) {
        qWarning() << "FAIL: bulk 场景建连失败：" << err.message;
        ++g_failures;
        return;
    }
    if (!c.openShell(QByteArrayLiteral("xterm-256color"), 80, 24, err)) {
        qWarning() << "FAIL: bulk 场景 openShell 失败：" << err.message;
        ++g_failures;
        c.disconnectFromHost();
        return;
    }
    drain(c, 500);

    // --- 下行 200000 字节 ---
    // marker 同样插空引号，免得命令行回显自己就把 BEGIN 触发了。
    {
        const QByteArray cmd =
            QByteArrayLiteral("echo DOWN''_BEGIN; head -c 200000 /dev/zero | tr '\\0' 'A'; "
                              "echo; echo DOWN''_END\n");
        CHECK(c.writeChannel(cmd) == cmd.size());

        QByteArray acc;
        const bool got = pumpUntil(c, acc, QByteArrayLiteral("DOWN_END"), 30000);
        CHECK(got);
        if (got) {
            const int b = acc.indexOf(QByteArrayLiteral("DOWN_BEGIN"));
            const int e = acc.indexOf(QByteArrayLiteral("DOWN_END"));
            CHECK(b >= 0 && e > b);
            if (b >= 0 && e > b) {
                const QByteArray body = acc.mid(b, e - b);
                const int as = int(body.count('A'));
                // 只数 'A'：pty 输出会把 \n 变 \r\n，但不会碰 'A'。
                if (as != 200000)
                    qWarning() << "  下行字节数不符：期望 200000，实到" << as;
                CHECK(as == 200000);
            }
        } else {
            qWarning() << "  下行没等到结束标记，已读" << acc.size() << "字节";
        }
    }

    // --- 上行 200×1001 字节，回显不关 ---
    // 每写一小片就把回显排掉（见 drain 的注释）：这样两个方向是**同时**有数据的，
    // 正是要压的那个状态；只写不读的话死锁的是本测试自己，不是被测代码。
    //
    // 行长取 1000：pty 处于规范模式，单行有 MAX_CANON 上限（Linux 4096），
    // 超了会被内核截断，那样对不上字节数就不是中继的问题了。
    {
        const QByteArray cmd = QByteArrayLiteral("cat > /tmp/cube_up.bin\n");
        CHECK(c.writeChannel(cmd) == cmd.size());
        drain(c, 300);

        QByteArray line(1000, 'A');
        line.append('\n');
        QByteArray payload;
        payload.reserve(line.size() * 200);
        for (int i = 0; i < 200; ++i)
            payload += line;

        qint64 echoed = 0;
        bool   wrote  = true;
        for (int off = 0; off < payload.size(); off += 8 * 1024) {
            const QByteArray slice = payload.mid(off, 8 * 1024);
            if (c.writeChannel(slice) != slice.size()) {
                wrote = false;
                break;
            }
            echoed += drain(c, 50);
        }
        CHECK(wrote);

        // 行首的 EOT 让 cat 收到 EOF 回到 shell。
        const QByteArray eot = QByteArrayLiteral("\x04");
        CHECK(c.writeChannel(eot) == eot.size());
        echoed += drain(c, 500);

        const QByteArray verify =
            QByteArrayLiteral("echo UP''_DONE_$(wc -c < /tmp/cube_up.bin)\n");
        CHECK(c.writeChannel(verify) == verify.size());

        QByteArray acc;
        const bool got = pumpUntil(c, acc, QByteArrayLiteral("UP_DONE_"), 20000);
        CHECK(got);
        if (got) {
            const bool sizeOk = acc.contains(QByteArrayLiteral("UP_DONE_200200"));
            if (!sizeOk) {
                const int i = acc.indexOf(QByteArrayLiteral("UP_DONE_"));
                qWarning() << "  上行字节数不符，实得："
                           << QString::fromLatin1(acc.mid(i, 32));
            }
            CHECK(sizeOk);
        }
        // 回显本身也得真的流回来过——它是"另一个方向同时在动"的证据。
        if (echoed < 100000)
            qWarning() << "  上行期间回显只收到" << echoed << "字节，双向压力可能没形成";
        CHECK(echoed > 100000);
    }

    c.disconnectFromHost();
}

// 取消 / 拆链。刻意都用**确定性**的做法，不靠"睡 300ms 再打断"那种赛跑——
// 本地 docker 一跳握手常常几十毫秒就完了，那种写法会随机在"已经连上了"上翻车。
static void testCancelAndTeardown()
{
    qInfo() << "--- cancel-flag-not-sticky ---";
    // shutdownSocket() 会置建连取消标志（它是关标签页那条路的入口）。连接池会
    // 复用同一个 SshClient 对象重连，所以这个标志必须在下一次 connectToHost
    // 开头被清掉——不清的后果是"关过一次的标签页永远重连不上"，而且报的是
    // 一句"连接已取消"，看不出是上一次留下的。
    {
        SshClient c;
        c.shutdownSocket();

        SshError err;
        DialSpec spec{QString::fromLatin1(kTargetInner), 22,
                      jumpProxy({QStringLiteral("b1")}), {}};
        const bool ok = dial(c, spec, err);
        if (!ok)
            qWarning().noquote()
                << QStringLiteral("  置过取消标志后重连失败（标志没清？）：%1").arg(err.message);
        CHECK(ok);
        if (ok) {
            QString detail;
            const bool rt = shellRoundTrip(c, QStringLiteral("nosticky"), &detail);
            if (!rt)
                qWarning().noquote() << QStringLiteral("  通道不通：%1").arg(detail);
            CHECK(rt);
        }
        c.disconnectFromHost();
    }

    qInfo() << "--- teardown (jump-2hop) ---";
    // 关标签页那条路：shutdownSocket() 之后通道必须立刻断，disconnectFromHost()
    // 必须及时返回。中继线程 join 不上时实现走的是"宁可泄漏"分支，所以这里真正
    // 在防的是**卡死 UI 线程**——阻塞实现在这一步会挂到超时。
    {
        SshClient c;
        SshError err;
        DialSpec spec{QString::fromLatin1(kTargetInner), 22,
                      jumpProxy({QStringLiteral("b1"), QStringLiteral("b2")}), {}};
        if (!dial(c, spec, err)) {
            qWarning().noquote() << QStringLiteral("FAIL: 拆链场景建连失败：%1").arg(err.message);
            ++g_failures;
            return;
        }
        QString detail;
        const bool rt = shellRoundTrip(c, QStringLiteral("teardown"), &detail);
        if (!rt)
            qWarning().noquote() << QStringLiteral("  拆链前通道就不通：%1").arg(detail);
        CHECK(rt);

        c.shutdownSocket();

        // 通道应当在很短时间内读到 EOF/出错（不是一直 EAGAIN）。
        QElapsedTimer t;
        t.start();
        bool dead = false;
        while (t.elapsed() < 5000) {
            bool wouldBlock = false;
            const QByteArray chunk = c.readChannel(kChunk, &wouldBlock);
            if (chunk.isEmpty() && !wouldBlock) {
                dead = true;
                break;
            }
            c.waitReadable(50);
        }
        if (!dead)
            qWarning() << "  shutdownSocket() 之后通道仍未断，等了" << t.elapsed() << "ms";
        CHECK(dead);

        QElapsedTimer t2;
        t2.start();
        c.disconnectFromHost();
        const qint64 ms = t2.elapsed();
        // 3 段链正常路径远快于此；给到 15s 是为了容下最坏一次 join 等待，
        // 同时仍能抓住"真的卡死"。
        if (ms > 15000)
            qWarning() << "  disconnectFromHost 用了" << ms << "ms，疑似卡在拆链上";
        CHECK(ms <= 15000);
    }
}

// --- main -----------------------------------------------------------------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 环境探针。目标机的端口连不上就整体跳过——本测试全部依赖那套 compose，
    // 缺了它每一项都会失败，一堆红字里看不出"其实是没起环境"。
    {
        QString probeErr;
        qintptr probe = socket_util::connectTcp(QString::fromLatin1(kHostAddr),
                                                kTargetPort, 2000, &probeErr);
        if (probe < 0) {
            qWarning().noquote()
                << QStringLiteral("SKIP: %1:%2 连不上（%3）。先拉起验证环境：\n"
                                  "  cd tests/docker/proxy && ./up.sh")
                       .arg(QString::fromLatin1(kHostAddr))
                       .arg(kTargetPort)
                       .arg(probeErr);
            return 2;
        }
        socket_util::closeSocket(probe);
    }
    // 代理与跳板的端口也一起探一遍：只起了 sshd 没起 squid/dante 时，
    // 跳过比让一半场景红着更有用。
    for (const auto &pp : {std::pair<quint16, const char *>{kB1Port, "bastion1"},
                           {kB2PortHost, "bastion2"},
                           {kHttpOpen, "squid(匿名)"},
                           {kHttpAuth, "squid(认证)"},
                           {kSocksOpen, "dante(匿名)"},
                           {kSocksAuth, "dante(认证)"}}) {
        QString probeErr;
        qintptr probe = socket_util::connectTcp(QString::fromLatin1(kHostAddr),
                                                pp.first, 2000, &probeErr);
        if (probe < 0) {
            qWarning().noquote()
                << QStringLiteral("SKIP: %1（127.0.0.1:%2）连不上（%3），验证环境不完整")
                       .arg(QString::fromLatin1(pp.second))
                       .arg(pp.first)
                       .arg(probeErr);
            return 2;
        }
        socket_util::closeSocket(probe);
    }

    // 跳板机凭据快照。真实路径上由 MainWindow::publishJumpHostCatalog 推进来，
    // 工作线程只从 GlobalState 读（DeviceConfigStore 没有锁）。
    GlobalState::instance().setJumpHostCatalog(buildCatalog());

    const QList<Scenario> scenarios = buildScenarios();
    for (const Scenario &sc : scenarios) {
        qInfo().noquote() << QStringLiteral("--- %1 ---").arg(sc.name);

        SshClient c;
        SshError err;
        const bool connected = dial(c, sc.spec, err);

        if (sc.expectOk) {
            if (!connected) {
                qWarning().noquote()
                    << QStringLiteral("FAIL: %1 建连失败：%2").arg(sc.name, err.message);
                ++g_failures;
                c.disconnectFromHost();
                continue;
            }
            QString detail;
            if (!shellRoundTrip(c, sc.name, &detail)) {
                qWarning().noquote()
                    << QStringLiteral("FAIL: %1 通道不通：%2").arg(sc.name, detail);
                ++g_failures;
            } else {
                ++g_passed;
                qInfo().noquote() << QStringLiteral("  ok");
            }
        } else {
            if (connected) {
                qWarning().noquote()
                    << QStringLiteral("FAIL: %1 本该失败却连上了").arg(sc.name);
                ++g_failures;
            } else if (!err.message.contains(sc.errorNeedle)) {
                // 失败得对不算够，还要**说得清**：错误消息是用户唯一能看到的东西。
                qWarning().noquote()
                    << QStringLiteral("FAIL: %1 错误消息里没有 \"%2\"：%3")
                           .arg(sc.name, sc.errorNeedle, err.message);
                ++g_failures;
            } else {
                ++g_passed;
                qInfo().noquote() << QStringLiteral("  ok（%1）").arg(err.message);
            }
        }
        c.disconnectFromHost();
    }

    testCancelAndTeardown();
    testBulkThroughChain();

    qInfo().noquote()
        << QStringLiteral("=== proxy_integration_test: %1 通过 / %2 失败 ===")
               .arg(g_passed).arg(g_failures);
    return g_failures == 0 ? 0 : 1;
}
