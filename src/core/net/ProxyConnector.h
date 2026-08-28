#pragma once

// ProxyConnector.h — 「拨号」的唯一入口：给定目标与代理配置，返回一个已连接、
// 可直接交给 libssh2_session_handshake 的 fd。
//
// 设计要点（为什么返回的是一个普通 fd）：
//   SshClient::socketFd() 在整个应用里是承重的——SshBridge 读循环的 select、
//   shutdownSocket() 这个全应用取消机制、以及 openShell() 之后把 fd 本身翻成
//   O_NONBLOCK，三处都直接摸它。所以代理无论怎么实现，交出去的都必须是一个
//   真的、可 select 的 socket：
//     * Http / Socks5 / 全局 / 系统 —— 在真 socket 上就地（in-band）完成握手，
//       握完把同一个 fd 原样交出去，既有代码改动面为零；
//     * 代理命令 / 跳转服务器 —— 传输载体是管道或 libssh2 channel，不是 socket，
//       于是建一对已连接的本地 socket pair（socket_util::makeSocketPair），
//       一端交出去，另一端由泵线程与真实载体对搬字节。
//
//   明确**不用** libssh2 的自定义传输回调（LIBSSH2_CALLBACK_SEND/RECV）：那样
//   socketFd() 就失去意义，上面三处全要重写，波及 SshBridge、SftpTransferPool、
//   PortForwarder。socket pair 方案把代价锁死在新增代码里。
//
// 分层：本文件在 net/ 层，不依赖 ssh/（见 src/core/CMakeLists.txt 的 net/ 段：
// 这一层定位为协议无关、无条件编译）。跳转服务器必须用 libssh2，所以走依赖
// 倒置——调用方注入一个 JumpDialer，真正的链式实现在 ssh/SshJumpChain。

#include <QString>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

#include "ProxyConfig.h"
#include "SocketUtil.h"

namespace cubeshell {

// 代理链的「载体」句柄。
//
// Http/Socks5/系统/直连都不需要它：握手完就是同一个 fd，没有额外活物。
// 代理命令和跳转服务器需要——它们背后还挂着子进程、泵线程、以及（跳板机的）
// 一整条 SSH 会话链。这些东西的存活期必须**恰好**覆盖那个 fd 的存活期：
//   * 早拆 → libssh2 手里的 fd 变成一根断掉的管子，表现是随机掉线；
//   * 不拆 → 每开一次连接漏一个子进程/线程，跑一天下来几十个僵尸。
//
// 于是拨号器把句柄交回给 SshClient，由它在 disconnectFromHost() 里按固定顺序
// 拆除（socket 关完之后才拆载体——libssh2_session_disconnect 那句 "bye" 还要
// 经过载体发出去）。
class ProxyTransport {
public:
    virtual ~ProxyTransport() = default;

    // 打断在途 I/O 并停掉载体。可从任意线程调用，可重复调用。
    // 用于 shutdownSocket() 这条取消路径：目标 socket 被 shutdown 之后，
    // 泵线程也必须醒过来，否则它会一直守着一个已经没人读的 fd。
    virtual void shutdown() = 0;

    // 载体侧攒下的诊断信息（子进程 stderr、退出码等），可为空。
    //
    // 存在的理由很具体：`nc` 之类的载体进程能正常启动、然后立刻打印
    // "connection refused" 退出。这时拨号早已成功返回，用户看到的是 libssh2 的
    // "Failed getting banner"——现象离病因极远。SshClient 在握手/认证失败时把
    // 这段文字附在错误后面，用户才看得到真正的原因。
    virtual QString diagnostics() const { return QString(); }

    // 这条载体能否在**无人交互**的前提下原样重建。
    //
    // 只有跳板链会返回 false：链上任一跳用了 keyboard-interactive 动态码，重建
    // 就得再管用户要一次。SftpTransferPool 靠 SshClient::isAuthReplayable() 决定
    // 能不能克隆兄弟连接做并行传输——不把载体算进去的话，目标机用密码、跳板机
    // 用 OTP 的那种配置会被判成"可克隆"，然后每开一个克隆都去弹一次动态码框。
    virtual bool isReplayable() const { return true; }
};

using ProxyTransportPtr = std::shared_ptr<ProxyTransport>;

// 跳板拨号器。由 SSH 层注入（见上面的分层说明）。
// 语义与 proxyConnect 一致：成功返回已连接的 fd，失败返回 <0 并填 errorOut。
// hops 是有序的跳板 id 列表，可能还含嵌套引用，由拨号器自己用
// flattenJumpChain 展平（它需要同一个 JumpHopLookup 去取凭据）。
// transportOut 非空时接收整条链的句柄（见 ProxyTransport）。
using JumpDialer = std::function<qintptr(const QString &targetHost, quint16 targetPort,
                                        const QStringList &hops,
                                        int timeoutMs,
                                        const std::atomic<bool> *cancelled,
                                        ProxyTransportPtr *transportOut,
                                        QString *errorOut)>;

// 本地进程拨号器（代理命令）。同样由上层注入——net/ 层不起子进程
// （鸿蒙沙箱禁 exec，相关能力统一由 CUBESHELL_WITH_LOCALPROC 门控）。
// command 是已经做过 %h/%p/%r 替换的完整命令行。
// transportOut 非空时接收子进程 + 泵线程的句柄（见 ProxyTransport）。
using CommandDialer = std::function<qintptr(const QString &command,
                                           int timeoutMs,
                                           const std::atomic<bool> *cancelled,
                                           ProxyTransportPtr *transportOut,
                                           QString *errorOut)>;

struct ProxyDialRequest {
    // 最终目标（不是代理的地址）。
    QString host;
    quint16 port = 0;
    // 登录用户名。仅用于代理命令的 %r 替换，与代理自身的认证无关。
    QString user;

    ProxyConfig proxy;
    // proxy.type == Global 时的取值来源（「设置 → 代理」那一份）。
    ProxyConfig globalProxy;

    // 整个拨号的预算（毫秒），含代理握手。多跳时由 JumpDialer 内部按跳再分。
    int timeoutMs = socket_util::kDefaultConnectTimeoutMs;

    // 建连途中的取消标志。非空时每个 select 切片查一次。
    //
    // 为什么必须有：现在关标签页靠 shutdownSocket() 打断在途握手，但建代理链
    // 途中目标 SshClient 还没有自己的 socket（m_sock 仍是 -1），shutdownSocket()
    // 会直接 early-return，什么都打断不了。用户在第 3 跳还在连的时候关掉标签页，
    // 没有这个标志就只能等到 OS 超时。
    const std::atomic<bool> *cancelled = nullptr;

    // 仅 JumpHost / Command 需要；没注入时对应类型会返回可读错误而不是崩。
    JumpDialer    jumpDialer;
    CommandDialer commandDialer;
};

// 拨号。成功返回一个**阻塞模式**、已设好 SO_NOSIGPIPE/TCP_NODELAY 的 fd
//（与改造之前 connectToHost 里那个裸 ::connect 的产物状态一致，故后续
// handshake / openShell 的行为不变）；失败返回 socket_util::kInvalidSocket
// 并填 errorOut。
//
// transportOut 非空时，代理命令/跳转服务器会往里放一个载体句柄——调用方**必须
// 一直持有它**，直到那个 fd 用完为止（见 ProxyTransport）。其余代理类型不动它。
qintptr proxyConnect(const ProxyDialRequest &req, ProxyTransportPtr *transportOut,
                     QString *errorOut);

// --- 供测试直接调用的握手实现 ---------------------------------------------
//
// 单独暴露是因为这是本次最值得测的部分：协议报文写错的表现只是「连不上」，
// 不看字节根本定不了位。测试在 127.0.0.1 起一个假代理服务端，断言客户端
// 发出的字节序列符合 RFC，以及 407 / REP≠0 等失败路径给出可读错误。
//
// 两者都要求 sock 已连到代理上，且处于阻塞模式；成功后 sock 上就是通往
// targetHost:targetPort 的裸字节流。失败时**不关** sock，由调用方处置。
bool httpConnectHandshake(qintptr sock, const QString &targetHost, quint16 targetPort,
                          const QString &username, const QString &password,
                          int timeoutMs, QString *errorOut,
                          const std::atomic<bool> *cancelled = nullptr);

bool socks5Handshake(qintptr sock, const QString &targetHost, quint16 targetPort,
                     const QString &username, const QString &password,
                     int timeoutMs, QString *errorOut,
                     const std::atomic<bool> *cancelled = nullptr);

} // namespace cubeshell
