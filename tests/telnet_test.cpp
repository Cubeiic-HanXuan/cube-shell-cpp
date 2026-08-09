// TCP/Telnet 单测：TelnetProtocol 的 IAC 状态机（含分块边界）、IAC 转义、
// 各选项协商应答、自动登录状态机的提示词匹配、defaultPortFor 的取值、
// netcombo 下拉框助手、DeviceEntry 的 tcp/telnet 字段 JSON 往返与旧配置兼容。
//
// 真实收发要有对端，不进单测；用 `nc -l` / socat + telnetd 手动验证
// （见 docs/Telnet功能测试验证说明书.md）。

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QFile>
#include <QTemporaryDir>

#include "config/DeviceConfigStore.h"
#include "dialogs/NetConnectDialog.h"   // netcombo 助手 + netSettingsFromDevice
#include "net/TcpBridge.h"
#include "net/TcpClient.h"
#include "net/TelnetProtocol.h"
#include "url_dispatch/UrlHandler.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 便于写期望值：把 IAC 命令序列拼成 QByteArray。
static QByteArray iac(std::initializer_list<int> bytes)
{
    QByteArray out;
    for (int b : bytes)
        out.append(char(static_cast<unsigned char>(b)));
    return out;
}

using T = TelnetProtocol;

// 收集一个 TelnetProtocol 发出的所有应答字节。
struct Collector {
    QByteArray sent;
    int echoChanges = 0;
    bool lastEcho = false;

    explicit Collector(TelnetProtocol *p)
    {
        QObject::connect(p, &TelnetProtocol::sendResponse,
                         [this](const QByteArray &b) { sent += b; });
        QObject::connect(p, &TelnetProtocol::serverEchoChanged,
                         [this](bool e) { ++echoChanges; lastEcho = e; });
    }
};

// --- 1. 分块边界：任意切分都必须与整块喂入等价 ---
//
// 这是 IAC 状态机最容易出错的地方：一个 `IAC DO NAWS` 完全可能被 TCP 分段
// 切成两次 readyRead，一段子协商能横跨十几个包。
static void testChunkBoundaries()
{
    // 混合流：普通数据 + 双字节命令 + 协商 + 子协商 + 转义的 0xFF。
    const QByteArray stream =
        QByteArray("hello")
        + iac({T::IAC, T::DO, T::OPT_TTYPE})
        + QByteArray(" mid ")
        + iac({T::IAC, T::SB, T::OPT_TTYPE, T::TTYPE_SEND, T::IAC, T::SE})
        + iac({T::IAC, T::IAC})            // 字面 0xFF
        + QByteArray("tail")
        + iac({T::IAC, T::NOP})
        + QByteArray("end");

    // 基准：整块喂入。
    TelnetProtocol whole;
    Collector wholeC(&whole);
    const QByteArray expectClean = whole.processIncoming(stream);

    // 净数据里不能残留任何 IAC 命令，但要保留那个还原出来的字面 0xFF。
    CHECK(expectClean == QByteArray("hello mid \xFF" "tailend"));

    for (int chunk : {1, 2, 3, 7}) {
        TelnetProtocol p;
        Collector c(&p);
        QByteArray clean;
        for (int i = 0; i < stream.size(); i += chunk)
            clean += p.processIncoming(stream.mid(i, chunk));
        if (clean != expectClean)
            qWarning() << "chunk" << chunk << "clean mismatch:" << clean.toHex()
                       << "expected" << expectClean.toHex();
        CHECK(clean == expectClean);
        // 应答字节也必须逐字节一致——分块不该改变协商结果。
        if (c.sent != wholeC.sent)
            qWarning() << "chunk" << chunk << "response mismatch:" << c.sent.toHex()
                       << "expected" << wholeC.sent.toHex();
        CHECK(c.sent == wholeC.sent);
    }
}

// --- 2. IAC IAC 还原 / escapeOutgoing 反向转义 ---
static void testIacEscaping()
{
    TelnetProtocol p;
    Collector c(&p);

    // 收方向：IAC IAC → 单个 0xFF。
    CHECK(p.processIncoming(iac({T::IAC, T::IAC})) == QByteArray("\xFF"));
    CHECK(p.processIncoming(QByteArray("a") + iac({T::IAC, T::IAC}) + QByteArray("b"))
          == QByteArray("a\xFF" "b"));
    // 跨块的 IAC IAC 同样要还原成一个字节。
    CHECK(p.processIncoming(iac({T::IAC})).isEmpty());
    CHECK(p.processIncoming(iac({T::IAC})) == QByteArray("\xFF"));
    // 整个过程不该产生任何应答。
    CHECK(c.sent.isEmpty());

    // 发方向：0xFF → IAC IAC，其余字节不动。
    CHECK(T::escapeOutgoing(QByteArray("abc")) == QByteArray("abc"));
    CHECK(T::escapeOutgoing(QByteArray("\xFF")) == iac({T::IAC, T::IAC}));
    CHECK(T::escapeOutgoing(QByteArray("a\xFF" "b"))
          == QByteArray("a") + iac({T::IAC, T::IAC}) + QByteArray("b"));
    CHECK(T::escapeOutgoing(QByteArray("\xFF\xFF"))
          == iac({T::IAC, T::IAC, T::IAC, T::IAC}));
    CHECK(T::escapeOutgoing(QByteArray()).isEmpty());

    // 往返：escape 出去的字节再喂回解析器，应还原成原文。
    TelnetProtocol back;
    const QByteArray raw("x\xFF\xFF" "y\xFF" "z");
    CHECK(back.processIncoming(T::escapeOutgoing(raw)) == raw);
}

// --- 3. TERMINAL-TYPE(24)：DO → WILL，SEND 子协商 → IS <termType> ---
static void testTerminalType()
{
    TelnetProtocol p;
    Collector c(&p);
    p.setTerminalType(QByteArrayLiteral("xterm-256color"));

    CHECK(p.processIncoming(iac({T::IAC, T::DO, T::OPT_TTYPE})).isEmpty());
    CHECK(c.sent == iac({T::IAC, T::WILL, T::OPT_TTYPE}));

    c.sent.clear();
    CHECK(p.processIncoming(
              iac({T::IAC, T::SB, T::OPT_TTYPE, T::TTYPE_SEND, T::IAC, T::SE}))
          .isEmpty());
    const QByteArray expect = iac({T::IAC, T::SB, T::OPT_TTYPE, T::TTYPE_IS})
                              + QByteArrayLiteral("xterm-256color")
                              + iac({T::IAC, T::SE});
    CHECK(c.sent == expect);

    // 重复 DO TTYPE 不该再回一次 WILL（RFC 855：状态没变化时不得回复，
    // 否则两端来回 DO/WILL 成协商环路）。
    c.sent.clear();
    p.processIncoming(iac({T::IAC, T::DO, T::OPT_TTYPE}));
    CHECK(c.sent.isEmpty());

    // 自定义终端类型走同一条路。
    TelnetProtocol vt;
    Collector vtC(&vt);
    vt.setTerminalType(QByteArrayLiteral("vt100"));
    vt.processIncoming(iac({T::IAC, T::DO, T::OPT_TTYPE}));
    vtC.sent.clear();
    vt.processIncoming(iac({T::IAC, T::SB, T::OPT_TTYPE, T::TTYPE_SEND, T::IAC, T::SE}));
    CHECK(vtC.sent == iac({T::IAC, T::SB, T::OPT_TTYPE, T::TTYPE_IS})
                          + QByteArrayLiteral("vt100") + iac({T::IAC, T::SE}));
}

// --- 4. NAWS(31)：DO → WILL + 尺寸子协商；字节必须完全匹配 RFC 1073 ---
static void testNaws()
{
    TelnetProtocol p;
    Collector c(&p);

    // 协商前先记下尺寸（终端在建连前就已经有尺寸了）：只记不发。
    p.sendWindowSize(80, 24);
    CHECK(c.sent.isEmpty());

    // 对端 DO NAWS → 回 WILL NAWS，并把已记下的尺寸补发一次。
    p.processIncoming(iac({T::IAC, T::DO, T::OPT_NAWS}));
    const QByteArray expect = iac({T::IAC, T::WILL, T::OPT_NAWS})
                              + iac({T::IAC, T::SB, T::OPT_NAWS,
                                     0, 80, 0, 24, T::IAC, T::SE});
    CHECK(c.sent == expect);

    // 协商完成后 resize 立即发出。
    c.sent.clear();
    p.sendWindowSize(132, 43);
    CHECK(c.sent == iac({T::IAC, T::SB, T::OPT_NAWS, 0, 132, 0, 43, T::IAC, T::SE}));

    // 尺寸没变时不重复发（每个字符宽的 resize 都发一遍是噪声）。
    c.sent.clear();
    p.sendWindowSize(132, 43);
    CHECK(c.sent.isEmpty());

    // 大于 255 列走高位字节。
    c.sent.clear();
    p.sendWindowSize(300, 100);
    CHECK(c.sent == iac({T::IAC, T::SB, T::OPT_NAWS, 1, 44, 0, 100, T::IAC, T::SE}));

    // 关键边界：255 列的低位字节就是 0xFF，必须转义成 IAC IAC，
    // 否则对端会把它当成命令前导，整段子协商就废了。
    c.sent.clear();
    p.sendWindowSize(255, 24);
    CHECK(c.sent == iac({T::IAC, T::SB, T::OPT_NAWS,
                         0, T::IAC, T::IAC, 0, 24, T::IAC, T::SE}));
}

// --- 5. ECHO(1)：服务端 WILL ECHO → 回 DO ECHO 且上报 serverEchoChanged ---
static void testServerEcho()
{
    TelnetProtocol p;
    Collector c(&p);
    CHECK(!p.serverEcho());

    p.processIncoming(iac({T::IAC, T::WILL, T::OPT_ECHO}));
    CHECK(c.sent == iac({T::IAC, T::DO, T::OPT_ECHO}));
    CHECK(p.serverEcho());
    CHECK(c.echoChanges == 1);
    CHECK(c.lastEcho == true);

    // 重复 WILL ECHO 既不重复应答也不重复上报。
    c.sent.clear();
    p.processIncoming(iac({T::IAC, T::WILL, T::OPT_ECHO}));
    CHECK(c.sent.isEmpty());
    CHECK(c.echoChanges == 1);

    // 服务端撤回（WONT ECHO）→ 回 DONT 且状态复位。
    c.sent.clear();
    p.processIncoming(iac({T::IAC, T::WONT, T::OPT_ECHO}));
    CHECK(!p.serverEcho());
    CHECK(c.echoChanges == 2);
    CHECK(c.lastEcho == false);

    // reset() 后重新开始（重连场景）。
    p.reset();
    CHECK(!p.serverEcho());
}

// --- 6. 未知选项一律拒绝，且其后的普通数据不被吞掉 ---
//
// 拒绝的方向必须与提议相反（RFC 855）：对端 DO（要我启用）→ 我 WONT；
// 对端 WILL（它要启用）→ 我 DONT。方向搞反会让协商永远收敛不了。
static void testUnknownOptions()
{
    TelnetProtocol p;
    Collector c(&p);

    // DO <未知> → WONT <未知>
    const QByteArray clean =
        p.processIncoming(iac({T::IAC, T::DO, 99}) + QByteArray("after"));
    CHECK(clean == QByteArray("after"));
    CHECK(c.sent == iac({T::IAC, T::WONT, 99}));

    // WILL <未知> → DONT <未知>
    c.sent.clear();
    CHECK(p.processIncoming(iac({T::IAC, T::WILL, 98}) + QByteArray("x"))
          == QByteArray("x"));
    CHECK(c.sent == iac({T::IAC, T::DONT, 98}));

    // 未知选项的子协商整段丢弃，载荷里的字节不能漏到终端。
    c.sent.clear();
    CHECK(p.processIncoming(iac({T::IAC, T::SB, 99})
                            + QByteArray("payload")
                            + iac({T::IAC, T::SE})
                            + QByteArray("visible"))
          == QByteArray("visible"));

    // SGA / BINARY 属于认识的选项，DO 和 WILL 两个方向都同意。
    TelnetProtocol sga;
    Collector sgaC(&sga);
    sga.processIncoming(iac({T::IAC, T::DO, T::OPT_SGA}));
    CHECK(sgaC.sent == iac({T::IAC, T::WILL, T::OPT_SGA}));
    sgaC.sent.clear();
    sga.processIncoming(iac({T::IAC, T::WILL, T::OPT_SGA}));
    CHECK(sgaC.sent == iac({T::IAC, T::DO, T::OPT_SGA}));
    sgaC.sent.clear();
    sga.processIncoming(iac({T::IAC, T::WILL, T::OPT_BINARY}));
    CHECK(sgaC.sent == iac({T::IAC, T::DO, T::OPT_BINARY}));
}

// --- 7. 双字节命令（NOP / GA / DM…）直接丢弃 ---
static void testTwoByteCommands()
{
    TelnetProtocol p;
    Collector c(&p);

    CHECK(p.processIncoming(QByteArray("a") + iac({T::IAC, T::NOP}) + QByteArray("b"))
          == QByteArray("ab"));
    CHECK(p.processIncoming(iac({T::IAC, T::GA}) + QByteArray("c")) == QByteArray("c"));
    CHECK(p.processIncoming(iac({T::IAC, T::DM})).isEmpty());
    CHECK(p.processIncoming(iac({T::IAC, T::AYT})).isEmpty());
    // 这些命令都不需要应答。
    CHECK(c.sent.isEmpty());
}

// --- 8. 子协商跨块 + 载荷内含 IAC IAC ---
static void testSubNegotiationEdges()
{
    // 载荷里的 IAC IAC 是字面 0xFF，不能被当成 SE 前导提前结束子协商。
    // 用 NAWS 承载：宽 255 的低位字节就是 0xFF，对端发回来时正是这个形态。
    const QByteArray stream = iac({T::IAC, T::SB, T::OPT_NAWS,
                                   0, T::IAC, T::IAC, 0, 24, T::IAC, T::SE})
                              + QByteArray("visible");

    TelnetProtocol whole;
    CHECK(whole.processIncoming(stream) == QByteArray("visible"));

    // 逐字节喂入结果相同（SubIac 阶段的状态必须跨调用保持）。
    for (int chunk : {1, 2, 5}) {
        TelnetProtocol p;
        QByteArray clean;
        for (int i = 0; i < stream.size(); i += chunk)
            clean += p.processIncoming(stream.mid(i, chunk));
        CHECK(clean == QByteArray("visible"));
    }

    // 畸形子协商：IAC 后跟的既不是 SE 也不是 IAC（这里是一个新的 IAC DO）。
    // 解析器应就地结束子协商并把后续字节当命令重新解读，不能把剩下的全吞掉。
    TelnetProtocol bad;
    Collector badC(&bad);
    const QByteArray malformed = iac({T::IAC, T::SB, T::OPT_NAWS, 0, 80})
                                 + iac({T::IAC, T::DO, T::OPT_TTYPE})
                                 + QByteArray("rest");
    CHECK(bad.processIncoming(malformed) == QByteArray("rest"));
    CHECK(badC.sent == iac({T::IAC, T::WILL, T::OPT_TTYPE}));
}

// --- 9. 协商关闭时仍剥 IAC，但不回任何应答 ---
static void testNegotiationDisabled()
{
    TelnetProtocol p;
    Collector c(&p);
    p.setNegotiationEnabled(false);

    // 不回应答……
    CHECK(p.processIncoming(iac({T::IAC, T::DO, T::OPT_TTYPE})).isEmpty());
    CHECK(p.processIncoming(iac({T::IAC, T::WILL, T::OPT_ECHO})).isEmpty());
    CHECK(c.sent.isEmpty());
    // ……但 IAC 序列照样要剥掉，否则 0xFF 开头的控制字节会当乱码上屏。
    CHECK(p.processIncoming(QByteArray("a") + iac({T::IAC, T::NOP}) + QByteArray("b"))
          == QByteArray("ab"));
    CHECK(p.processIncoming(iac({T::IAC, T::IAC})) == QByteArray("\xFF"));
}

// --- 10. 自动登录的提示词匹配（静态纯函数） ---
static void testLoginPrompts()
{
    // telnetd 的经典形态。
    CHECK(TcpBridge::looksLikeLoginPrompt(QByteArray("\r\nlogin: ")));
    CHECK(TcpBridge::looksLikeLoginPrompt(QByteArray("host login:")));
    // 网络设备常用 Username:，大小写与中间空格都要容忍。
    CHECK(TcpBridge::looksLikeLoginPrompt(QByteArray("Username: ")));
    CHECK(TcpBridge::looksLikeLoginPrompt(QByteArray("User Name: ")));
    CHECK(TcpBridge::looksLikeLoginPrompt(QByteArray("LOGIN:")));
    // 中文固件 + 全角冒号。
    CHECK(TcpBridge::looksLikeLoginPrompt(QString::fromUtf8("用户名：").toUtf8()));

    CHECK(TcpBridge::looksLikePasswordPrompt(QByteArray("Password: ")));
    CHECK(TcpBridge::looksLikePasswordPrompt(QByteArray("password:")));
    CHECK(TcpBridge::looksLikePasswordPrompt(QByteArray("Passcode: ")));
    CHECK(TcpBridge::looksLikePasswordPrompt(QString::fromUtf8("密码：").toUtf8()));

    // 只匹配"提示符在尾部"的情形：banner/MOTD 里出现的字样不算。
    CHECK(!TcpBridge::looksLikePasswordPrompt(
        QByteArray("Your password: expires in 3 days\r\n")));
    CHECK(!TcpBridge::looksLikeLoginPrompt(QByteArray("Last login: Mon Aug  3\r\n")));
    CHECK(!TcpBridge::looksLikeLoginPrompt(QByteArray("$ ")));
    CHECK(!TcpBridge::looksLikePasswordPrompt(QByteArray("")));
    // 两个提示符互不误判。
    CHECK(!TcpBridge::looksLikeLoginPrompt(QByteArray("Password: ")));
    CHECK(!TcpBridge::looksLikePasswordPrompt(QByteArray("login: ")));
}

// --- 11. defaultPortFor：五个协议的取值 ---
static void testDefaultPorts()
{
    CHECK(defaultPortFor(QStringLiteral("ssh")) == 22);
    CHECK(defaultPortFor(QStringLiteral("rdp")) == 3389);
    CHECK(defaultPortFor(QStringLiteral("telnet")) == 23);
    CHECK(defaultPortFor(QStringLiteral("tcp")) == 23);
    // 串口不用端口；空值是 protocol 字段缺失的旧配置。两者都回落 22。
    CHECK(defaultPortFor(QStringLiteral("serial")) == 22);
    CHECK(defaultPortFor(QString()) == 22);

    // DeviceEntry::hostPort() 用的就是它。port 字段留 0 表示"未指定"，
    // 此时按协议取默认端口。
    DeviceEntry e;
    e.protocol = QStringLiteral("telnet");
    e.host = QStringLiteral("10.0.0.1");
    e.port = 0;
    CHECK(e.hostPort().port == 23);
    CHECK(e.hostPort().host == QStringLiteral("10.0.0.1"));
    // host 里带了端口就以它为准。
    e.host = QStringLiteral("10.0.0.1:2323");
    CHECK(e.hostPort().port == 2323);
    // IPv6 字面量。
    e.host = QStringLiteral("[fe80::1]:2323");
    CHECK(e.hostPort().host == QStringLiteral("fe80::1"));
    CHECK(e.hostPort().port == 2323);
}

// --- 12. netcombo 助手 + TcpSettings 默认值 ---
static void testNetCombo()
{
    using NL = TcpSettings::NewlineMode;

    TcpSettings s;
    CHECK(s.mode == QStringLiteral("telnet"));
    CHECK(s.isTelnet());
    CHECK(s.port == 23);
    CHECK(s.newlineMode == NL::CrLf);   // RFC 854：Telnet 行尾即 CR LF
    CHECK(s.rxImplicitCr == true);
    CHECK(s.localEcho == false);
    CHECK(s.negotiate == true);
    CHECK(s.autoLogin == false);
    CHECK(s.termType == QByteArrayLiteral("xterm-256color"));
    s.host = QStringLiteral("10.0.0.1");
    CHECK(s.displayTarget() == QStringLiteral("telnet 10.0.0.1:23"));
    // IPv6 加方括号，与 formatHostPort 一致。
    s.host = QStringLiteral("fe80::1");
    CHECK(s.displayTarget() == QStringLiteral("telnet [fe80::1]:23"));

    // 模式下拉框。
    QComboBox mode;
    netcombo::fillMode(&mode);
    CHECK(mode.count() == 2);
    CHECK(netcombo::modeOf(&mode) == QStringLiteral("telnet"));
    netcombo::fillMode(&mode, QStringLiteral("tcp"));
    CHECK(netcombo::modeOf(&mode) == QStringLiteral("tcp"));
    CHECK(netcombo::modeOf(nullptr) == QStringLiteral("telnet"));

    // 换行下拉框。
    QComboBox nl;
    netcombo::fillNewlineMode(&nl);
    CHECK(nl.count() == 3);
    CHECK(netcombo::newlineModeOf(&nl) == NL::CrLf);
    netcombo::fillNewlineMode(&nl, NL::Lf);
    CHECK(netcombo::newlineModeOf(&nl) == NL::Lf);
    netcombo::selectData(&nl, int(NL::Cr));
    CHECK(netcombo::newlineModeOf(&nl) == NL::Cr);
    CHECK(netcombo::newlineModeOf(nullptr) == NL::CrLf);

    // 字符串 ↔ 枚举。缺省/未知回落 CRLF（DeviceEntry 的默认值是串口的 "cr"，
    // 直接按它走会让新建的 Telnet 设备回车不生效）。
    CHECK(netcombo::newlineModeFromString(QStringLiteral("cr")) == NL::Cr);
    CHECK(netcombo::newlineModeFromString(QStringLiteral("lf")) == NL::Lf);
    CHECK(netcombo::newlineModeFromString(QStringLiteral("crlf")) == NL::CrLf);
    CHECK(netcombo::newlineModeFromString(QStringLiteral("CRLF")) == NL::CrLf);
    CHECK(netcombo::newlineModeFromString(QString()) == NL::CrLf);
    CHECK(netcombo::newlineModeToString(NL::Cr) == QStringLiteral("cr"));
    CHECK(netcombo::newlineModeToString(NL::Lf) == QStringLiteral("lf"));
    CHECK(netcombo::newlineModeToString(NL::CrLf) == QStringLiteral("crlf"));

    // 终端类型：可编辑下拉框，显示文本即取值，手输也要拿得到。
    QComboBox term;
    netcombo::fillTermTypes(&term);
    CHECK(netcombo::termTypeOf(&term) == QByteArrayLiteral("xterm-256color"));
    netcombo::fillTermTypes(&term, QByteArrayLiteral("vt220"));
    CHECK(netcombo::termTypeOf(&term) == QByteArrayLiteral("vt220"));
    // 列表里没有的私有终端名：预填走 setCurrentText，仍要取得到。
    netcombo::fillTermTypes(&term, QByteArrayLiteral("cisco-term"));
    CHECK(netcombo::termTypeOf(&term) == QByteArrayLiteral("cisco-term"));
    term.setCurrentText(QStringLiteral("  screen  "));
    CHECK(netcombo::termTypeOf(&term) == QByteArrayLiteral("screen"));
    // 清空后回落默认值，而不是把空串报给对端。
    term.setCurrentText(QString());
    CHECK(netcombo::termTypeOf(&term) == QByteArrayLiteral("xterm-256color"));
    CHECK(netcombo::termTypeOf(nullptr) == QByteArrayLiteral("xterm-256color"));
}

// --- 13. telnet:// URL 解析 ---
static void testTelnetUrl()
{
    UrlConnectionInfo info = parseTelnetUrl(QStringLiteral("telnet://10.0.0.1"));
    CHECK(info.valid);
    CHECK(info.scheme == QStringLiteral("telnet"));
    CHECK(info.protocol == QStringLiteral("telnet"));
    CHECK(info.host == QStringLiteral("10.0.0.1"));
    CHECK(info.port == 23);   // 默认端口来自 defaultPortFor("telnet")
    CHECK(info.user.isEmpty());

    info = parseTelnetUrl(QStringLiteral("telnet://admin:s3cr3t@10.0.0.1:2323"));
    CHECK(info.valid);
    CHECK(info.host == QStringLiteral("10.0.0.1"));
    CHECK(info.port == 2323);
    CHECK(info.user == QStringLiteral("admin"));
    CHECK(info.password == QStringLiteral("s3cr3t"));

    // userinfo 的百分号编码要解开（'@' 出现在用户名里时必须编码）。
    info = parseTelnetUrl(QStringLiteral("telnet://a%40b:p%3Aw@host"));
    CHECK(info.valid);
    CHECK(info.user == QStringLiteral("a@b"));
    CHECK(info.password == QStringLiteral("p:w"));

    // IPv6 字面量：方括号由 QUrl 剥掉。
    info = parseTelnetUrl(QStringLiteral("telnet://[fe80::1]:2323"));
    CHECK(info.valid);
    CHECK(info.host == QStringLiteral("fe80::1"));
    CHECK(info.port == 2323);

    // 无主机 / 非 telnet scheme → 失败且带原因。
    info = parseTelnetUrl(QStringLiteral("telnet://"));
    CHECK(!info.valid);
    CHECK(!info.error.isEmpty());
    info = parseTelnetUrl(QStringLiteral("ssh://10.0.0.1"));
    CHECK(!info.valid);

    // parseUrl 的 scheme 分发也要认得 telnet://。
    const UrlConnectionInfo viaDispatch =
        parseUrl(QStringLiteral("telnet://admin@10.0.0.1:2323"));
    CHECK(viaDispatch.valid);
    CHECK(viaDispatch.scheme == QStringLiteral("telnet"));
    CHECK(viaDispatch.host == QStringLiteral("10.0.0.1"));
    CHECK(viaDispatch.port == 2323);
    CHECK(viaDispatch.user == QStringLiteral("admin"));
}

// --- 14. TcpClient 未连接时的行为（无对端也能测） ---
static void testClientIdleBehaviour()
{
    TcpClient client;
    CHECK(client.state() == TcpClient::State::Disconnected);
    CHECK(!client.isOpen());
    CHECK(!client.isLogging());

    // 未连接时写入应失败而不是崩。
    CHECK(!client.write(QByteArray("hello")));

    // 空主机 / 0 端口必须失败。
    TcpSettings s;
    CHECK(!client.connectToHost(s));
    CHECK(!client.isOpen());
    s.host = QStringLiteral("127.0.0.1");
    s.port = 0;
    CHECK(!client.connectToHost(s));
    CHECK(!client.isOpen());

    // 未连接时断开是空操作，不应崩、也不该发 disconnected()。
    int disconnects = 0;
    QObject::connect(&client, &TcpClient::disconnected, [&]() { ++disconnects; });
    client.disconnectFromHost();
    CHECK(disconnects == 0);
    CHECK(!client.isOpen());

    // 日志文件：空路径 = 停止录制；目录不存在时自动创建。
    QTemporaryDir dir;
    CHECK(dir.isValid());
    CHECK(client.setLogFile(QString()));
    CHECK(!client.isLogging());
    const QString path = dir.filePath(QStringLiteral("sub/telnet.log"));
    CHECK(client.setLogFile(path));
    CHECK(client.isLogging());
    CHECK(client.logFilePath() == path);
    CHECK(QFile::exists(path));
    CHECK(client.setLogFile(QString()));
    CHECK(!client.isLogging());
}

// --- 15. DeviceEntry 的 tcp/telnet 字段 JSON 往返 + 协议谓词 ---
static void testDeviceEntryRoundTrip()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("devices.json"));

    DeviceEntry telnetDev;
    telnetDev.name = QStringLiteral("交换机");
    telnetDev.protocol = QStringLiteral("telnet");
    telnetDev.host = QStringLiteral("10.0.0.1:2323");
    telnetDev.port = 2323;
    telnetDev.username = QStringLiteral("admin");
    telnetDev.password = QStringLiteral("s3cr3t");
    telnetDev.telnetNegotiate = false;
    telnetDev.termType = QStringLiteral("vt100");
    telnetDev.autoLogin = true;
    telnetDev.newlineMode = QStringLiteral("crlf");
    telnetDev.localEcho = true;
    telnetDev.rxImplicitCr = false;

    DeviceEntry tcpDev;
    tcpDev.name = QStringLiteral("调试服务");
    tcpDev.protocol = QStringLiteral("tcp");
    tcpDev.host = QStringLiteral("127.0.0.1:9999");
    tcpDev.port = 9999;
    tcpDev.newlineMode = QStringLiteral("lf");

    // 协议谓词互斥，且都不是 SSH。
    CHECK(telnetDev.isTelnet());
    CHECK(!telnetDev.isTcp());
    CHECK(!telnetDev.isSsh());
    CHECK(!telnetDev.isRdp());
    CHECK(!telnetDev.isSerial());
    CHECK(tcpDev.isTcp());
    CHECK(!tcpDev.isTelnet());
    CHECK(!tcpDev.isSsh());

    // SSH 设备不被误判成 tcp/telnet；protocol 为空的旧配置也算 SSH。
    DeviceEntry sshDev;
    sshDev.name = QStringLiteral("web");
    sshDev.host = QStringLiteral("10.0.0.2:22");
    sshDev.username = QStringLiteral("root");
    CHECK(sshDev.isSsh());
    CHECK(!sshDev.isTcp());
    CHECK(!sshDev.isTelnet());
    DeviceEntry blank;
    blank.protocol.clear();
    CHECK(blank.isSsh());

    DeviceConfigStore store;
    store.addDevice(telnetDev);
    store.addDevice(tcpDev);
    store.addDevice(sshDev);
    QString err;
    CHECK(store.saveJson(jsonPath, &err));

    DeviceConfigStore reloaded;
    CHECK(reloaded.loadJson(jsonPath, &err));
    const DeviceEntry *got = reloaded.find(QStringLiteral("交换机"));
    CHECK(got != nullptr);
    if (got) {
        CHECK(got->isTelnet());
        CHECK(got->hostPort().host == QStringLiteral("10.0.0.1"));
        CHECK(got->hostPort().port == 2323);
        CHECK(got->username == QStringLiteral("admin"));
        CHECK(got->telnetNegotiate == false);
        CHECK(got->termType == QStringLiteral("vt100"));
        CHECK(got->autoLogin == true);
        CHECK(got->newlineMode == QStringLiteral("crlf"));
        CHECK(got->localEcho == true);
        CHECK(got->rxImplicitCr == false);

        // 持久化形态 → 运行时形态的映射。
        const TcpSettings s = netSettingsFromDevice(*got);
        CHECK(s.mode == QStringLiteral("telnet"));
        CHECK(s.host == QStringLiteral("10.0.0.1"));
        CHECK(s.port == 2323);
        CHECK(s.username == QStringLiteral("admin"));
        CHECK(s.password == QStringLiteral("s3cr3t"));
        CHECK(s.negotiate == false);
        CHECK(s.termType == QByteArrayLiteral("vt100"));
        CHECK(s.autoLogin == true);
        CHECK(s.newlineMode == TcpSettings::NewlineMode::CrLf);
        CHECK(s.localEcho == true);
        CHECK(s.rxImplicitCr == false);
    }

    const DeviceEntry *gotTcp = reloaded.find(QStringLiteral("调试服务"));
    CHECK(gotTcp != nullptr);
    if (gotTcp) {
        CHECK(gotTcp->isTcp());
        CHECK(gotTcp->hostPort().port == 9999);
        const TcpSettings s = netSettingsFromDevice(*gotTcp);
        CHECK(s.mode == QStringLiteral("tcp"));
        CHECK(!s.isTelnet());
        CHECK(s.newlineMode == TcpSettings::NewlineMode::Lf);
    }

    // SSH 条目不受影响。
    const DeviceEntry *gotSsh = reloaded.find(QStringLiteral("web"));
    CHECK(gotSsh != nullptr);
    if (gotSsh) {
        CHECK(gotSsh->isSsh());
        CHECK(gotSsh->username == QStringLiteral("root"));
    }
}

// --- 16. 旧版 JSON（无 tcp/telnet 键）的默认回落 ---
static void testLegacyJsonFallback()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("legacy.json"));

    // 手写一份加 TCP/Telnet 之前的 JSON：没有 telnetNegotiate/termType/autoLogin。
    QFile f(jsonPath);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(R"([{"name":"旧设备","username":"root","password":"p",)"
            R"("host":"192.168.1.5:22","port":22,"keyType":"","keyFile":"",)"
            R"("protocol":"ssh","domain":"","auth":"ntlm"}])");
    f.close();

    DeviceConfigStore store;
    QString err;
    CHECK(store.loadJson(jsonPath, &err));
    const DeviceEntry *e = store.find(QStringLiteral("旧设备"));
    CHECK(e != nullptr);
    if (e) {
        CHECK(e->isSsh());
        // 缺键 → 结构体默认值，而不是 false / 空串。
        // telnetNegotiate 回落 true：协商是 Telnet 的正常行为，回落 false 会让
        // 保存过的设备表现得和新建的不一样（拿不到 NAWS/TTYPE）。
        CHECK(e->telnetNegotiate == true);
        CHECK(e->termType == QStringLiteral("xterm-256color"));
        // autoLogin 回落 false：自动送密码要用户显式开启。
        CHECK(e->autoLogin == false);
    }

    // 一份只写了 protocol=telnet、其余 telnet 键全缺的配置（手改配置文件的场景）。
    const QString partialPath = dir.filePath(QStringLiteral("partial.json"));
    QFile pf(partialPath);
    CHECK(pf.open(QIODevice::WriteOnly));
    pf.write(R"([{"name":"手改","host":"10.0.0.9","protocol":"telnet"}])");
    pf.close();

    DeviceConfigStore partial;
    CHECK(partial.loadJson(partialPath, &err));
    const DeviceEntry *pe = partial.find(QStringLiteral("手改"));
    CHECK(pe != nullptr);
    if (pe) {
        CHECK(pe->isTelnet());
        // port 键缺失 → loadJson 的回落值按 protocol 取（defaultPortFor），
        // 不是一律 22——否则手改配置只写 protocol=telnet 会连到 SSH 端口上。
        CHECK(pe->port == 23);
        CHECK(pe->hostPort().port == 23);
        const TcpSettings s = netSettingsFromDevice(*pe);
        CHECK(s.port == 23);
        CHECK(s.negotiate == true);
        CHECK(s.termType == QByteArrayLiteral("xterm-256color"));
        CHECK(s.autoLogin == false);
        // newlineMode 缺键 → 按 protocol 回落（defaultNewlineModeFor）：
        // telnet 落 "crlf"，不是串口的 "cr"——RFC 854 规定 Telnet 行尾是 CR LF，
        // 落 "cr" 会让手改出来的 telnet 条目回车不生效。
        CHECK(pe->newlineMode == QStringLiteral("crlf"));
        CHECK(s.newlineMode == TcpSettings::NewlineMode::CrLf);
    }

    // 同样只写 protocol=tcp：裸 TCP 落 "lf"（对端多是 Unix 侧服务，CR 会显示成 ^M）。
    const QString tcpPartialPath = dir.filePath(QStringLiteral("partial_tcp.json"));
    QFile tf(tcpPartialPath);
    CHECK(tf.open(QIODevice::WriteOnly));
    tf.write(R"([{"name":"裸TCP","host":"10.0.0.8","protocol":"tcp"}])");
    tf.close();

    DeviceConfigStore tcpPartial;
    CHECK(tcpPartial.loadJson(tcpPartialPath, &err));
    const DeviceEntry *te = tcpPartial.find(QStringLiteral("裸TCP"));
    CHECK(te != nullptr);
    if (te) {
        CHECK(te->newlineMode == QStringLiteral("lf"));
        CHECK(netSettingsFromDevice(*te).newlineMode == TcpSettings::NewlineMode::Lf);
    }

    // 串口/SSH 不受影响：缺键仍落 "cr"（RS-232 设备惯例）。
    CHECK(defaultNewlineModeFor(QStringLiteral("serial")) == QStringLiteral("cr"));
    CHECK(defaultNewlineModeFor(QStringLiteral("ssh")) == QStringLiteral("cr"));

    // 显式写了 "cr" 的 telnet 条目必须原样保留——CR 是换行下拉框里用户可以
    // 主动选的一项，映射层把它纠成 CRLF 会让用户的选择存进去又读不回来。
    const QString explicitCrPath = dir.filePath(QStringLiteral("explicit_cr.json"));
    QFile cf(explicitCrPath);
    CHECK(cf.open(QIODevice::WriteOnly));
    cf.write(R"([{"name":"选了CR","host":"10.0.0.7","protocol":"telnet","newlineMode":"cr"}])");
    cf.close();

    DeviceConfigStore explicitCr;
    CHECK(explicitCr.loadJson(explicitCrPath, &err));
    const DeviceEntry *ce = explicitCr.find(QStringLiteral("选了CR"));
    CHECK(ce != nullptr);
    if (ce) {
        CHECK(ce->newlineMode == QStringLiteral("cr"));
        CHECK(netSettingsFromDevice(*ce).newlineMode == TcpSettings::NewlineMode::Cr);
    }
}

int main(int argc, char **argv)
{
    // netcombo 用例要构造 QComboBox，需要 QApplication；无显示环境下走
    // offscreen 平台插件，CI 里也能跑。
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    testChunkBoundaries();
    testIacEscaping();
    testTerminalType();
    testNaws();
    testServerEcho();
    testUnknownOptions();
    testTwoByteCommands();
    testSubNegotiationEdges();
    testNegotiationDisabled();
    testLoginPrompts();
    testDefaultPorts();
    testNetCombo();
    testTelnetUrl();
    testClientIdleBehaviour();
    testDeviceEntryRoundTrip();
    testLegacyJsonFallback();

    if (failures == 0)
        qInfo() << "telnet_test: all checks passed";
    else
        qWarning() << "telnet_test:" << failures << "check(s) failed";
    return failures == 0 ? 0 : 1;
}
