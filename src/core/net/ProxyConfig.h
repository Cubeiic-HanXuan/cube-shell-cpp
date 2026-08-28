#pragma once

// ProxyConfig.h — 代理配置的数据模型（协议无关，不做任何 I/O）。
//
// 覆盖 WindTerm 的 6 种代理类型。本轮接线只做 SSH（见 SshClient::setProxyConfig），
// 但这一层刻意不提 SSH：net/ 是协议无关层（见 src/core/CMakeLists.txt 的 net/ 段），
// 以后 Telnet/TCP 走代理可以直接复用同一个结构体和同一套 JSON 键。
//
// 密码不在这里持久化。见 password 字段的注释。

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace cubeshell {

enum class ProxyType {
    None,      // 直连（缺省；与加代理功能之前的行为逐字节一致）
    Global,    // 用「设置 → 代理」里那份全局配置
    System,    // 问操作系统要代理（QNetworkProxyFactory）
    Http,      // HTTP CONNECT 隧道
    Socks5,    // SOCKS v5（RFC 1928 + RFC 1929 认证）
    Command,   // 代理命令：起一个本地进程，用它的 stdin/stdout 当传输
    JumpHost,  // 跳转服务器：经一台或多台已保存的 SSH 设备中转
};

// 跳板机链的深度上限。存在的意义是兜住配置写坏的情况——展平之后真正的
// 防线是环检测，这个上限管的是"没有环但链条荒谬地长"（比如手改配置文件
// 串了几十跳），以及给用户一个明确的失败而不是几十次超时的叠加。
inline constexpr int kMaxJumpHops = 8;

// 类型 ↔ JSON 字符串。未知字符串一律回落 None（容错优先：配置文件被手改坏
// 时宁可退化成直连并让连接因"连不上"失败，也不要整条设备读不出来）。
QString   proxyTypeToString(ProxyType type);
ProxyType proxyTypeFromString(const QString &s);

// 类型 → 默认端口。http=8080 / socks5=1080 / 其余=0（不适用）。
// 存在的理由同 defaultPortFor(protocol)：这个知识点会同时被对话框的类型切换
// 和连接期的兜底用到，放两处早晚漏改一处。
quint16 proxyDefaultPort(ProxyType type);

struct ProxyConfig {
    ProxyType type = ProxyType::None;

    // Http / Socks5 用。
    QString host;
    quint16 port = 0;
    QString username;

    // 明文口令，**仅存在于内存**，绝不写进 devices.json —— 与 DeviceEntry::password
    // 同一条不变量（见 DeviceConfigStore.h 的「密码」段）。toJson() 不写它，
    // fromJson() 不读它；落盘走钥匙串，由 DeviceConfigStore 填充。
    QString password;

    // Command 用。支持 OpenSSH 的 %h/%p/%r 占位符，见 substituteProxyCommand()。
    QString command;

    // JumpHost 用：按 DeviceEntry::id 引用侧栏里已保存的设备，凭据直接复用
    // 那台设备自己在钥匙串里的记录，不必重复录入。
    // 顺序是**从外到内**：先连 hopIds[0]，再经它连 hopIds[1]，最后到目标
    //（与 OpenSSH `-J a,b` 的语义一致）。
    QStringList hopIds;

    bool isDirect() const { return type == ProxyType::None; }
    // 需要 host/port 才能工作的类型。校验与 UI 显隐都看它。
    bool needsHostPort() const { return type == ProxyType::Http || type == ProxyType::Socks5; }
    // 取值还不确定、连接期才知道的类型（Global 要读全局设置，System 要问系统）。
    bool needsResolution() const { return type == ProxyType::Global || type == ProxyType::System; }

    // 扁平 JSON 读写。键名带 "proxy" 前缀平铺在设备对象里（proxyType /
    // proxyHost / …），与 DeviceEntry 其余协议字段的既有风格一致
    //（见 DeviceConfigStore::toJsonArray——串口、TCP 字段都是平铺的）。
    //
    // 全部键缺失时 fromJson 返回一个 type == None 的默认值，这是旧 devices.json
    // 的迁移路径：本项目没有 schema version 字段，靠这种内联默认值兼容旧文件。
    //
    // withHopIds=false 时不写 proxyHopIds。给导出路径用：hopIds 里装的是
    // DeviceEntry::id，而导出物刻意不带 id（那是本机钥匙串的索引，带到别的
    // 机器上只会撞车，见 DeviceConfigStore::exportJson）。类型仍照写——
    // 导入方看到"跳转服务器 + 跳板列表为空"能明白要自己补一个，
    // 悄悄改成直连反而会让人以为配好了。
    void writeJson(QJsonObject &out, bool withHopIds = true) const;
    static ProxyConfig fromJson(const QJsonObject &in);

    // 只有 type 之外还有内容才算"配过"。用于 UI 判断要不要提示"未配置代理"。
    bool isConfigured() const;
};

bool operator==(const ProxyConfig &a, const ProxyConfig &b);
inline bool operator!=(const ProxyConfig &a, const ProxyConfig &b) { return !(a == b); }

// 把 Global 解析成具体配置。
//
// globalProxy 是「设置 → 代理」那份配置。globalProxy.type 自己又是 Global 时
// （手改 theme.json 才可能出现——设置页的下拉框里没有这一项）按直连处理，
// 不递归：Global→Global 是无限递归的唯一入口。
//
// System 不在这里解析：QNetworkProxyFactory 要按**目标** host/port 查询，
// 而这一层看不到目标，故留给 ProxyConnector。
ProxyConfig resolveGlobalProxy(const ProxyConfig &cfg, const ProxyConfig &globalProxy);

// OpenSSH 风格的占位符替换：%h→目标主机、%p→目标端口、%r→登录用户、%%→%。
// 逐字符扫一遍而不是链式 replace()：后者会把前一次替换产生的字符再当占位符
//（用户名里恰好有 "%p" 时就会被二次替换）。
QString substituteProxyCommand(const QString &command, const QString &host,
                               quint16 port, const QString &user);

// --- 跳板机链展平 --------------------------------------------------------

// 展平所需的设备信息。lookup 返回 std::nullopt 表示这个 id 已经不存在了
//（用户删掉了被引用的设备），与"存在但自己没配跳板"（返回 hopIds 为空）区分。
struct JumpHopInfo {
    QString     name;     // 给错误消息用的可读名字
    QStringList hopIds;   // 这台设备自己配的跳板
};

using JumpHopLookup = std::function<std::optional<JumpHopInfo>(const QString &id)>;

struct JumpChainResult {
    QStringList hops;   // 展平后的有序跳板 id，从最外层到最里层（不含目标自身）
    QString     error;  // 非空即失败：成环 / 超过 kMaxJumpHops / 引用了已删除的设备
    bool ok() const { return error.isEmpty(); }
};

// 把「设备 A 的跳板是 B、B 自己的跳板是 C」这种嵌套关系展平成一条线性拨号序列。
//
// 为什么要展平：连接期是逐跳建立的（连 hop1 → 在 hop1 上开 direct-tcpip 到 hop2
// → …），需要线性序列；而配置里天然是嵌套的——用户在 A 的设置里填 B，在 B 的
// 设置里填 C，不会（也不该）在 A 里重复填 C。
//
// 展平规则：要连 H，得先把 H 自己的跳板连上，所以 H 的链插在 H 之前。
// startDeviceId 预置进已访问集合，于是"目标把自己填成跳板"也算成环。
JumpChainResult flattenJumpChain(const QString &startDeviceId,
                                const QStringList &directHopIds,
                                const JumpHopLookup &lookup);

} // namespace cubeshell
