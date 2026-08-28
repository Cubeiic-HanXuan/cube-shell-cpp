// ProxyConnector.cpp — 见 ProxyConnector.h。

#include "ProxyConnector.h"

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>

Q_DECLARE_LOGGING_CATEGORY(proxyLog)
Q_LOGGING_CATEGORY(proxyLog, "cubeshell.proxy")

namespace cubeshell {

using namespace socket_util;

namespace {

// HTTP 应答头的容量上限。协议上没有上限，但正常的 CONNECT 应答只有几百字节；
// 设个上限是为了不被一个只管往回吐字节、永不发空行的对端拖死。
constexpr int kMaxHttpHeaderBytes = 8 * 1024;

// SOCKS5 常量。DynamicPortForwarder 里有一份服务端实现（PortForwarder.cpp
// 的匿名 namespace），报文格式可以对照，但那边是服务端、这边是客户端，
// 常量集合并不重合，故不共用。
constexpr quint8 kSocksVer          = 0x05;
constexpr quint8 kSocksAuthNone     = 0x00;
constexpr quint8 kSocksAuthUserPass = 0x02;
constexpr quint8 kSocksAuthNone_Unacceptable = 0xFF;
constexpr quint8 kSocksCmdConnect   = 0x01;
constexpr quint8 kSocksAtypIPv4     = 0x01;
constexpr quint8 kSocksAtypDomain   = 0x03;
constexpr quint8 kSocksAtypIPv6     = 0x04;
constexpr quint8 kSocksUserPassVer  = 0x01;

void setErr(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

// SOCKS5 的 REP 码 → 人话。原始数字对用户毫无意义，而这些码之间的处置方式
// 完全不同（认证失败要改配置，主机不可达要看网络）。
QString socks5ReplyMessage(quint8 rep)
{
    switch (rep) {
    case 0x00: return QString();
    case 0x01: return QStringLiteral("SOCKS5 代理返回一般性失败");
    case 0x02: return QStringLiteral("SOCKS5 代理规则不允许该连接");
    case 0x03: return QStringLiteral("SOCKS5 代理报告网络不可达");
    case 0x04: return QStringLiteral("SOCKS5 代理报告主机不可达");
    case 0x05: return QStringLiteral("目标主机拒绝连接");
    case 0x06: return QStringLiteral("SOCKS5 连接超时（TTL 过期）");
    case 0x07: return QStringLiteral("SOCKS5 代理不支持 CONNECT 命令");
    case 0x08: return QStringLiteral("SOCKS5 代理不支持该地址类型");
    default:   return QStringLiteral("SOCKS5 代理返回未知错误码 0x%1")
                          .arg(rep, 2, 16, QLatin1Char('0'));
    }
}

// 从 QNetworkProxy 映射出我们自己的类型。认不出的（FtpCaching 之类）返回 None。
ProxyType fromQtProxyType(QNetworkProxy::ProxyType t)
{
    switch (t) {
    case QNetworkProxy::HttpProxy:
    case QNetworkProxy::HttpCachingProxy:
        return ProxyType::Http;
    case QNetworkProxy::Socks5Proxy:
        return ProxyType::Socks5;
    default:
        return ProxyType::None;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP CONNECT
// ---------------------------------------------------------------------------

bool httpConnectHandshake(qintptr sock, const QString &targetHost, quint16 targetPort,
                          const QString &username, const QString &password,
                          int timeoutMs, QString *errorOut,
                          const std::atomic<bool> *cancelled)
{
    QDeadlineTimer budget(timeoutMs);

    const QString authority = QStringLiteral("%1:%2").arg(targetHost).arg(targetPort);
    QByteArray req;
    req += "CONNECT " + authority.toUtf8() + " HTTP/1.1\r\n";
    req += "Host: " + authority.toUtf8() + "\r\n";
    if (!username.isEmpty()) {
        // RFC 7617 Basic。凭据是 base64 而非加密——HTTP 代理本身就是明文协议，
        // 这一点值得在 UI 上让用户知道，但协议这么规定，照做。
        const QByteArray raw = username.toUtf8() + ':' + password.toUtf8();
        req += "Proxy-Authorization: Basic " + raw.toBase64() + "\r\n";
    }
    // 有些老代理（squid 2.x）默认在 CONNECT 应答后就关连接，显式要求保持。
    req += "Proxy-Connection: keep-alive\r\n";
    req += "\r\n";

    QString ioErr;
    if (!sendAll(sock, req.constData(), req.size(), int(budget.remainingTime()),
                 &ioErr, cancelled)) {
        setErr(errorOut, QStringLiteral("向 HTTP 代理发送 CONNECT 失败：%1").arg(ioErr));
        return false;
    }

    // 逐字节读到空行为止。**必须一个字节一个字节读**：CONNECT 成功之后，
    // 紧接着到来的就是被隧道的字节流（SSH 服务端 banner）。一次读一大块
    // 会把 banner 的头几个字节一起吞掉，libssh2 随后就会因为 banner 不完整
    // 而报 "Failed getting banner"——现象离病因很远，极难查。
    QByteArray header;
    header.reserve(512);
    char c = 0;
    while (!header.endsWith("\r\n\r\n")) {
        if (header.size() >= kMaxHttpHeaderBytes) {
            setErr(errorOut, QStringLiteral("HTTP 代理应答头过大（超过 %1 字节），已放弃")
                                 .arg(kMaxHttpHeaderBytes));
            return false;
        }
        if (!recvExact(sock, &c, 1, int(budget.remainingTime()), &ioErr, cancelled)) {
            setErr(errorOut, header.isEmpty()
                       ? QStringLiteral("HTTP 代理没有应答 CONNECT：%1").arg(ioErr)
                       : QStringLiteral("读取 HTTP 代理应答中断：%1").arg(ioErr));
            return false;
        }
        header.append(c);
        // 只有 \r\n\r\n 才是头结束。裸 \n\n 是非标准的，但确实有代理这么发，
        // 一并认下——认错的代价是把 SSH banner 的字节当成头，比连不上更难查。
        if (header.endsWith("\n\n"))
            break;
    }

    // 状态行：HTTP/1.x <code> <reason>
    const int lineEnd = header.indexOf('\r');
    const QByteArray statusLine =
        header.left(lineEnd > 0 ? lineEnd : header.indexOf('\n')).trimmed();
    const QList<QByteArray> parts = statusLine.split(' ');
    if (parts.size() < 2 || !parts[0].startsWith("HTTP/")) {
        setErr(errorOut, QStringLiteral("HTTP 代理应答不是合法的状态行：%1")
                             .arg(QString::fromLatin1(statusLine.left(120))));
        return false;
    }
    bool okCode = false;
    const int code = parts[1].toInt(&okCode);
    if (!okCode) {
        setErr(errorOut, QStringLiteral("HTTP 代理返回无法解析的状态码：%1")
                             .arg(QString::fromLatin1(statusLine.left(120))));
        return false;
    }
    if (code != 200) {
        // 状态行原文必须带进错误消息：407（要认证）和 403（策略禁止）对用户是
        // 完全不同的处置，只说"代理拒绝"等于什么都没说。
        QString hint;
        if (code == 407)
            hint = QStringLiteral("（代理要求认证，请检查用户名/密码）");
        else if (code == 403)
            hint = QStringLiteral("（代理策略禁止访问该目标）");
        else if (code == 405 || code == 501)
            hint = QStringLiteral("（该代理可能不支持 CONNECT 方法）");
        setErr(errorOut, QStringLiteral("HTTP 代理拒绝 CONNECT：%1%2")
                             .arg(QString::fromLatin1(statusLine.left(120)), hint));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SOCKS5（RFC 1928 + RFC 1929）
// ---------------------------------------------------------------------------

bool socks5Handshake(qintptr sock, const QString &targetHost, quint16 targetPort,
                     const QString &username, const QString &password,
                     int timeoutMs, QString *errorOut,
                     const std::atomic<bool> *cancelled)
{
    QDeadlineTimer budget(timeoutMs);
    QString ioErr;

    // --- 1. 方法协商 ---
    QByteArray greeting;
    greeting.append(char(kSocksVer));
    if (username.isEmpty()) {
        greeting.append(char(1));
        greeting.append(char(kSocksAuthNone));
    } else {
        // 同时报 none 和 user/pass：填了凭据不代表代理一定要求认证，
        // 只报 user/pass 会让不要求认证的代理直接回 0xFF。
        greeting.append(char(2));
        greeting.append(char(kSocksAuthNone));
        greeting.append(char(kSocksAuthUserPass));
    }
    if (!sendAll(sock, greeting.constData(), greeting.size(),
                 int(budget.remainingTime()), &ioErr, cancelled)) {
        setErr(errorOut, QStringLiteral("向 SOCKS5 代理发送方法协商失败：%1").arg(ioErr));
        return false;
    }

    char methodReply[2] = {0, 0};
    if (!recvExact(sock, methodReply, 2, int(budget.remainingTime()), &ioErr, cancelled)) {
        setErr(errorOut, QStringLiteral("SOCKS5 代理没有应答方法协商：%1").arg(ioErr));
        return false;
    }
    if (quint8(methodReply[0]) != kSocksVer) {
        setErr(errorOut, QStringLiteral("对端不是 SOCKS5 代理（版本号 %1，期望 5）")
                             .arg(int(quint8(methodReply[0]))));
        return false;
    }
    const quint8 method = quint8(methodReply[1]);
    if (method == kSocksAuthNone_Unacceptable) {
        setErr(errorOut, username.isEmpty()
                   ? QStringLiteral("SOCKS5 代理要求认证，但未配置用户名/密码")
                   : QStringLiteral("SOCKS5 代理不接受用户名/密码认证方式"));
        return false;
    }
    if (method == kSocksAuthUserPass) {
        // RFC 1929：VER(1) ULEN(1) UNAME PLEN(1) PASSWD
        const QByteArray u = username.toUtf8();
        const QByteArray p = password.toUtf8();
        if (u.size() > 255 || p.size() > 255) {
            setErr(errorOut, QStringLiteral("SOCKS5 用户名或密码超过 255 字节，协议不支持"));
            return false;
        }
        QByteArray auth;
        auth.append(char(kSocksUserPassVer));
        auth.append(char(u.size()));
        auth.append(u);
        auth.append(char(p.size()));
        auth.append(p);
        if (!sendAll(sock, auth.constData(), auth.size(),
                     int(budget.remainingTime()), &ioErr, cancelled)) {
            setErr(errorOut, QStringLiteral("向 SOCKS5 代理发送凭据失败：%1").arg(ioErr));
            return false;
        }
        char authReply[2] = {0, 0};
        if (!recvExact(sock, authReply, 2, int(budget.remainingTime()), &ioErr, cancelled)) {
            setErr(errorOut, QStringLiteral("SOCKS5 代理没有应答认证：%1").arg(ioErr));
            return false;
        }
        // RFC 1929 只规定 0 表示成功，非 0 一律失败（且要求关闭连接）。
        if (quint8(authReply[1]) != 0) {
            setErr(errorOut, QStringLiteral("SOCKS5 代理认证失败：用户名或密码错误"));
            return false;
        }
    } else if (method != kSocksAuthNone) {
        setErr(errorOut, QStringLiteral("SOCKS5 代理选择了不支持的认证方式 0x%1")
                             .arg(method, 2, 16, QLatin1Char('0')));
        return false;
    }

    // --- 2. CONNECT 请求 ---
    QByteArray request;
    request.append(char(kSocksVer));
    request.append(char(kSocksCmdConnect));
    request.append(char(0x00)); // RSV

    // 目标本来就是字面 IP 时按 IP 类型发。有些代理对"域名格式其实是 IP"的
    // ATYP=DOMAIN 会直接回 0x08（地址类型不支持）。
    const QHostAddress literal(targetHost);
    if (literal.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v4 = literal.toIPv4Address();
        request.append(char(kSocksAtypIPv4));
        request.append(char((v4 >> 24) & 0xff));
        request.append(char((v4 >> 16) & 0xff));
        request.append(char((v4 >> 8) & 0xff));
        request.append(char(v4 & 0xff));
    } else if (literal.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR v6 = literal.toIPv6Address();
        request.append(char(kSocksAtypIPv6));
        for (int i = 0; i < 16; ++i)
            request.append(char(v6[i]));
    } else {
        // 域名交给代理去解析（ATYP=DOMAIN），**不在本机解析**。
        // 这是关键：内网域名在本机根本解不出来，本地解析等于代理白做。
        const QByteArray hostBytes = targetHost.toUtf8();
        if (hostBytes.isEmpty() || hostBytes.size() > 255) {
            setErr(errorOut, QStringLiteral("目标主机名长度不合法（%1 字节），SOCKS5 上限 255")
                                 .arg(hostBytes.size()));
            return false;
        }
        request.append(char(kSocksAtypDomain));
        request.append(char(hostBytes.size()));
        request.append(hostBytes);
    }
    request.append(char((targetPort >> 8) & 0xff));
    request.append(char(targetPort & 0xff));

    if (!sendAll(sock, request.constData(), request.size(),
                 int(budget.remainingTime()), &ioErr, cancelled)) {
        setErr(errorOut, QStringLiteral("向 SOCKS5 代理发送 CONNECT 请求失败：%1").arg(ioErr));
        return false;
    }

    // --- 3. 应答 ---
    // VER REP RSV ATYP 之后是变长的 BND.ADDR + 2 字节 BND.PORT。**必须按 ATYP
    // 精确读完**：多读会吃掉 SSH banner，少读会把剩余字节当成 banner 的开头。
    char head[4] = {0, 0, 0, 0};
    if (!recvExact(sock, head, 4, int(budget.remainingTime()), &ioErr, cancelled)) {
        setErr(errorOut, QStringLiteral("SOCKS5 代理没有应答 CONNECT：%1").arg(ioErr));
        return false;
    }
    if (quint8(head[0]) != kSocksVer) {
        setErr(errorOut, QStringLiteral("SOCKS5 应答版本号异常（%1，期望 5）")
                             .arg(int(quint8(head[0]))));
        return false;
    }
    const quint8 rep = quint8(head[1]);
    const quint8 atyp = quint8(head[3]);

    qint64 addrLen = 0;
    if (atyp == kSocksAtypIPv4) {
        addrLen = 4;
    } else if (atyp == kSocksAtypIPv6) {
        addrLen = 16;
    } else if (atyp == kSocksAtypDomain) {
        char dlen = 0;
        if (!recvExact(sock, &dlen, 1, int(budget.remainingTime()), &ioErr, cancelled)) {
            setErr(errorOut, QStringLiteral("读取 SOCKS5 应答地址长度失败：%1").arg(ioErr));
            return false;
        }
        addrLen = quint8(dlen);
    } else if (rep == 0x00) {
        // 只在"声称成功"时才算致命：失败应答里有些代理会把 ATYP 填 0。
        setErr(errorOut, QStringLiteral("SOCKS5 应答地址类型未知（0x%1）")
                             .arg(atyp, 2, 16, QLatin1Char('0')));
        return false;
    }

    if (addrLen > 0) {
        QByteArray bnd(int(addrLen) + 2, 0);   // BND.ADDR + BND.PORT
        if (!recvExact(sock, bnd.data(), bnd.size(), int(budget.remainingTime()),
                       &ioErr, cancelled)) {
            setErr(errorOut, QStringLiteral("读取 SOCKS5 应答绑定地址失败：%1").arg(ioErr));
            return false;
        }
    }

    if (rep != 0x00) {
        setErr(errorOut, QStringLiteral("%1（目标 %2:%3）")
                             .arg(socks5ReplyMessage(rep), targetHost).arg(targetPort));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

namespace {

// 直连（也是 ProxyType::None 的实现）。与改造前 connectToHost 里那段裸
// ::connect 的行为等价，只是多了超时和可取消。
qintptr dialDirect(const QString &host, quint16 port, int timeoutMs,
                   const std::atomic<bool> *cancelled, QString *errorOut)
{
    QString err;
    const qintptr sock = connectTcp(host, port, timeoutMs, &err, cancelled);
    if (sock < 0)
        setErr(errorOut, err);
    return sock;
}

// 先连到代理，再在同一个 fd 上就地把握手做完。
qintptr dialViaHandshakeProxy(const ProxyConfig &cfg, const QString &targetHost,
                              quint16 targetPort, int timeoutMs,
                              const std::atomic<bool> *cancelled, QString *errorOut)
{
    if (cfg.host.isEmpty()) {
        setErr(errorOut, QStringLiteral("代理地址为空，请先在设备或全局设置里填写代理服务器"));
        return kInvalidSocket;
    }
    const quint16 proxyPort = cfg.port != 0 ? cfg.port : proxyDefaultPort(cfg.type);

    QDeadlineTimer budget(timeoutMs);
    QString err;
    const qintptr sock = connectTcp(cfg.host, proxyPort, int(budget.remainingTime()),
                                    &err, cancelled);
    if (sock < 0) {
        setErr(errorOut, QStringLiteral("连接代理服务器 %1:%2 失败：%3")
                             .arg(cfg.host).arg(proxyPort).arg(err));
        return kInvalidSocket;
    }

    const bool ok = (cfg.type == ProxyType::Http)
        ? httpConnectHandshake(sock, targetHost, targetPort, cfg.username, cfg.password,
                               int(budget.remainingTime()), &err, cancelled)
        : socks5Handshake(sock, targetHost, targetPort, cfg.username, cfg.password,
                          int(budget.remainingTime()), &err, cancelled);
    if (!ok) {
        closeFd(sock);
        setErr(errorOut, err);
        return kInvalidSocket;
    }
    qCDebug(proxyLog) << "proxy handshake ok via" << proxyTypeToString(cfg.type)
                      << cfg.host << proxyPort << "->" << targetHost << targetPort;
    return sock;
}

// 问操作系统要代理，映射成 Http/Socks5。查不到就直连。
ProxyConfig resolveSystemProxy(const QString &targetHost, quint16 targetPort)
{
    QNetworkProxyQuery query(targetHost, targetPort, QString(),
                             QNetworkProxyQuery::TcpSocket);
    const QList<QNetworkProxy> found = QNetworkProxyFactory::systemProxyForQuery(query);
    for (const QNetworkProxy &p : found) {
        const ProxyType t = fromQtProxyType(p.type());
        if (t == ProxyType::None)
            continue;   // NoProxy 或我们用不上的类型（FtpCaching 之类）
        ProxyConfig cfg;
        cfg.type = t;
        cfg.host = p.hostName();
        cfg.port = p.port();
        // 系统里存过凭据时 Qt 会带出来（macOS 从钥匙串取），一并用上。
        cfg.username = p.user();
        cfg.password = p.password();
        if (!cfg.host.isEmpty())
            return cfg;
    }
    // 系统没配代理是正常状态，不是错误——退化成直连，与"系统代理=关"一致。
    return ProxyConfig{};
}

} // namespace

qintptr proxyConnect(const ProxyDialRequest &req, ProxyTransportPtr *transportOut,
                     QString *errorOut)
{
    if (req.cancelled && req.cancelled->load()) {
        setErr(errorOut, QStringLiteral("连接已取消"));
        return kInvalidSocket;
    }

    const int timeoutMs = req.timeoutMs > 0 ? req.timeoutMs : kDefaultConnectTimeoutMs;

    // Global → 换成全局设置里那份（含 Global→Global 的递归防护）。
    ProxyConfig cfg = resolveGlobalProxy(req.proxy, req.globalProxy);
    // System → 问操作系统。放在 Global 之后：全局设置里选的可能就是"系统代理"。
    if (cfg.type == ProxyType::System)
        cfg = resolveSystemProxy(req.host, req.port);

    switch (cfg.type) {
    case ProxyType::None:
        return dialDirect(req.host, req.port, timeoutMs, req.cancelled, errorOut);

    case ProxyType::Http:
    case ProxyType::Socks5:
        return dialViaHandshakeProxy(cfg, req.host, req.port, timeoutMs,
                                     req.cancelled, errorOut);

    case ProxyType::Command: {
        if (!req.commandDialer) {
            setErr(errorOut, QStringLiteral("当前构建不支持代理命令（未启用本地进程能力）"));
            return kInvalidSocket;
        }
        if (cfg.command.trimmed().isEmpty()) {
            setErr(errorOut, QStringLiteral("代理命令为空，请先填写要执行的命令"));
            return kInvalidSocket;
        }
        const QString cmd = substituteProxyCommand(cfg.command, req.host, req.port, req.user);
        return req.commandDialer(cmd, timeoutMs, req.cancelled, transportOut, errorOut);
    }

    case ProxyType::JumpHost: {
        if (!req.jumpDialer) {
            setErr(errorOut, QStringLiteral("跳转服务器不可用（未注入 SSH 拨号器）"));
            return kInvalidSocket;
        }
        if (cfg.hopIds.isEmpty()) {
            setErr(errorOut, QStringLiteral("未选择跳板机，请至少添加一台"));
            return kInvalidSocket;
        }
        return req.jumpDialer(req.host, req.port, cfg.hopIds, timeoutMs,
                             req.cancelled, transportOut, errorOut);
    }

    case ProxyType::Global:
    case ProxyType::System:
        // 上面两步已经把这两种解析掉了；能走到这里说明 resolve 的分支漏了一种。
        setErr(errorOut, QStringLiteral("代理配置无法解析（类型 %1）")
                             .arg(proxyTypeToString(cfg.type)));
        return kInvalidSocket;
    }

    setErr(errorOut, QStringLiteral("未知的代理类型"));
    return kInvalidSocket;
}

} // namespace cubeshell
