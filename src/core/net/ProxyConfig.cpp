// ProxyConfig.cpp — 见 ProxyConfig.h。纯数据 + 纯函数，不做 I/O。

#include "ProxyConfig.h"

#include <QJsonArray>
#include <QSet>

namespace cubeshell {

namespace {

// JSON 键。平铺在设备对象里，前缀 "proxy"（见 ProxyConfig::writeJson 的注释）。
// 用 QLatin1String 常量而不是各处内联 QStringLiteral：这些键要在写和读两个
// 函数里各出现一次，写错一处的表现是"保存后读不回来"，很难查。
constexpr QLatin1String kKeyType("proxyType");
constexpr QLatin1String kKeyHost("proxyHost");
constexpr QLatin1String kKeyPort("proxyPort");
constexpr QLatin1String kKeyUsername("proxyUsername");
constexpr QLatin1String kKeyCommand("proxyCommand");
constexpr QLatin1String kKeyHopIds("proxyHopIds");

} // namespace

QString proxyTypeToString(ProxyType type)
{
    switch (type) {
    case ProxyType::None:     return QStringLiteral("none");
    case ProxyType::Global:   return QStringLiteral("global");
    case ProxyType::System:   return QStringLiteral("system");
    case ProxyType::Http:     return QStringLiteral("http");
    case ProxyType::Socks5:   return QStringLiteral("socks5");
    case ProxyType::Command:  return QStringLiteral("command");
    case ProxyType::JumpHost: return QStringLiteral("jump");
    }
    return QStringLiteral("none");
}

ProxyType proxyTypeFromString(const QString &s)
{
    if (s == QLatin1String("global"))  return ProxyType::Global;
    if (s == QLatin1String("system"))  return ProxyType::System;
    if (s == QLatin1String("http"))    return ProxyType::Http;
    if (s == QLatin1String("socks5"))  return ProxyType::Socks5;
    if (s == QLatin1String("command")) return ProxyType::Command;
    if (s == QLatin1String("jump"))    return ProxyType::JumpHost;
    // "none"、空串、以及任何认不出来的值。见头文件：容错优先。
    return ProxyType::None;
}

quint16 proxyDefaultPort(ProxyType type)
{
    switch (type) {
    case ProxyType::Http:   return 8080;
    case ProxyType::Socks5: return 1080;
    default:                return 0;
    }
}

void ProxyConfig::writeJson(QJsonObject &out, bool withHopIds) const
{
    out[kKeyType]     = proxyTypeToString(type);
    out[kKeyHost]     = host;
    out[kKeyPort]     = int(port);
    out[kKeyUsername] = username;
    out[kKeyCommand]  = command;
    // password 刻意不写：见头文件该字段的注释（与 DeviceEntry::password 同一条不变量）。
    if (!withHopIds)
        return;   // 导出路径：hopIds 是本机的设备 id，带出去无意义（见头文件）
    QJsonArray hops;
    for (const QString &id : hopIds)
        hops.append(id);
    out[kKeyHopIds] = hops;
}

ProxyConfig ProxyConfig::fromJson(const QJsonObject &in)
{
    ProxyConfig cfg;
    cfg.type     = proxyTypeFromString(in[kKeyType].toString());
    cfg.host     = in[kKeyHost].toString();
    // 缺 proxyPort 键时按类型回落，而不是一律 0：手改配置只写了
    // proxyType=socks5 + proxyHost 是常见情形，回落 0 会连到 0 端口上。
    cfg.port     = quint16(in[kKeyPort].toInt(proxyDefaultPort(cfg.type)));
    cfg.username = in[kKeyUsername].toString();
    cfg.command  = in[kKeyCommand].toString();
    const QJsonArray hops = in[kKeyHopIds].toArray();
    for (const QJsonValue &v : hops) {
        const QString id = v.toString();
        if (!id.isEmpty())
            cfg.hopIds.append(id);
    }
    return cfg;
}

bool ProxyConfig::isConfigured() const
{
    switch (type) {
    case ProxyType::None:     return false;
    case ProxyType::Global:
    case ProxyType::System:   return true;   // 取值在别处，本身就算配过
    case ProxyType::Http:
    case ProxyType::Socks5:   return !host.isEmpty();
    case ProxyType::Command:  return !command.isEmpty();
    case ProxyType::JumpHost: return !hopIds.isEmpty();
    }
    return false;
}

bool operator==(const ProxyConfig &a, const ProxyConfig &b)
{
    // password 参与比较：设置页/设备对话框靠 != 判断"用户改没改"，
    // 只改了口令也必须算改过。
    return a.type == b.type && a.host == b.host && a.port == b.port
        && a.username == b.username && a.password == b.password
        && a.command == b.command && a.hopIds == b.hopIds;
}

ProxyConfig resolveGlobalProxy(const ProxyConfig &cfg, const ProxyConfig &globalProxy)
{
    if (cfg.type != ProxyType::Global)
        return cfg;
    // Global→Global 是无限递归的唯一入口，就地掐断（见头文件注释）。
    if (globalProxy.type == ProxyType::Global)
        return ProxyConfig{};
    return globalProxy;
}

QString substituteProxyCommand(const QString &command, const QString &host,
                               quint16 port, const QString &user)
{
    QString out;
    out.reserve(command.size() + 16);
    for (int i = 0; i < command.size(); ++i) {
        const QChar c = command.at(i);
        if (c != QLatin1Char('%') || i + 1 >= command.size()) {
            out.append(c);
            continue;
        }
        const QChar spec = command.at(++i);
        if (spec == QLatin1Char('h'))
            out.append(host);
        else if (spec == QLatin1Char('p'))
            out.append(QString::number(port));
        else if (spec == QLatin1Char('r'))
            out.append(user);
        else if (spec == QLatin1Char('%'))
            out.append(QLatin1Char('%'));
        else {
            // 认不出的占位符原样保留（含 '%'）。OpenSSH 会报错，但这里的取舍是
            // 别把用户命令里合法的 % 吃掉——比如 awk '{print $1%2}'。
            out.append(QLatin1Char('%'));
            out.append(spec);
        }
    }
    return out;
}

JumpChainResult flattenJumpChain(const QString &startDeviceId,
                                const QStringList &directHopIds,
                                const JumpHopLookup &lookup)
{
    JumpChainResult result;
    QSet<QString> visited;
    // 预置目标自身：这样"目标把自己填成跳板"（直接或绕一圈）都会被判成环。
    // startDeviceId 为空（未保存的临时条目）时不预置，否则空串会挡掉
    // 一个同样为空的 hop id——那种情形由下面的 isEmpty 分支处理。
    if (!startDeviceId.isEmpty())
        visited.insert(startDeviceId);

    // 递归展平：要连 H，得先把 H 自己的跳板连上，所以 H 的链插在 H 之前。
    // 自递归 lambda 用显式 std::function：递归里要拿到自己。
    std::function<bool(const QStringList &)> expand = [&](const QStringList &hops) -> bool {
        for (const QString &id : hops) {
            if (id.isEmpty())
                continue;   // 配置里的空洞（删了一行没落盘之类），跳过而非报错
            // 上限在下探**之前**查：放在 append 之前的话，一条荒谬长的链会先把
            // 整棵树递归展开完才报错，白等一堆 lookup。
            if (result.hops.size() >= kMaxJumpHops) {
                result.error = QStringLiteral("跳板机层级过深（上限 %1 跳）").arg(kMaxJumpHops);
                return false;
            }
            if (visited.contains(id)) {
                // 两种情形都落在这里：真成环（A 的跳板是 B，B 的跳板又是 A），
                // 以及同一台机器在链上出现两次（展平后要连它两遍，同样是错的）。
                const std::optional<JumpHopInfo> dup = lookup ? lookup(id) : std::nullopt;
                const QString label = (dup && !dup->name.isEmpty()) ? dup->name : id;
                result.error =
                    QStringLiteral("跳板机配置有环或重复引用：%1 在链路上出现了多次").arg(label);
                return false;
            }
            const std::optional<JumpHopInfo> info = lookup ? lookup(id) : std::nullopt;
            if (!info) {
                result.error = QStringLiteral("跳板机 %1 已不存在（可能已被删除）").arg(id);
                return false;
            }
            // 先标记再下探：否则 A→B、B→A 的互引在下探时看不到自己已在路径上。
            visited.insert(id);
            if (!expand(info->hopIds))
                return false;
            result.hops.append(id);
        }
        return true;
    };

    if (!expand(directHopIds))
        result.hops.clear();
    return result;
}

} // namespace cubeshell
