// SshJumpChain.cpp — 见 SshJumpChain.h。
//
// 结构：
//   ChainDiagnostics    整条链共用的诊断沉淀池（是哪一跳的中继先断的）
//   RelayState          中继线程与句柄共享的那一小块状态（stop 标志 + 自己那一端的 fd）
//   runRelay()          中继线程主体：libssh2 通道 ↔ socket pair 双向对搬
//   JumpChainTransport  句柄，交给目标 SshClient 持有；析构 = 按序拆掉整条链
//   makeSshJumpDialer() 逐跳建链
//
// 全局的一条不变量：**每个中间跳的 session 在它的出向通道打开之后、对应中继线程
// 启动之前，必须翻成非阻塞**（SshClient::setTransportNonBlocking）。阻塞 session
// 上的 libssh2_channel_read 会持着 sessionLock 睡进去，中继的另一个方向于是永远
// 拿不到锁——那是个必然的死锁，不是概率问题。反过来，通道**打开**那一下要在还是
// 阻塞模式时做完：非阻塞下 direct_tcpip 会返回 EAGAIN，得自己写重试循环，而且
// 拿不到"下一跳拒绝连接"这类真实错误消息。所以顺序是：连上 → 开出向通道 →
// 翻非阻塞 → 起中继。

#include "SshJumpChain.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <libssh2.h>

#include <memory>
#include <utility>

#include "config/GlobalState.h"
#include "net/SocketUtil.h"

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/select.h>
#  include <sys/socket.h>
#endif

// 复用 ProxyConnector.cpp 注册的 "cubeshell.proxy" 分类：代理相关的日志开一个
// 开关就该全部出来，分两个分类反而要记两个名字（同 ProxyCommandDialer.cpp）。
Q_DECLARE_LOGGING_CATEGORY(proxyLog)

namespace cubeshell {

using namespace socket_util;

namespace {

// 中继缓冲。32KB 与 SSH 报文上限同量级，也与 ProxyCommandDialer 的取值一致。
constexpr int kRelayChunkBytes = 32 * 1024;

// 等中继线程退出的上限。它是个非阻塞循环、每片最多睡 kSelectSliceMs，正常路径
// 远用不到这个数；用不掉说明真的卡住了，那时走"宁可泄漏"分支（见 teardownLink）。
constexpr int kRelayJoinMs = 5000;

// 拆链时给每一跳 libssh2_session_disconnect/free 的上限。见 prepareGracefulClose。
constexpr int kGracefulCloseMs = 2000;

// 诊断信息容量上限。理由同 ProxyCommandDialer：攒的目的只是给用户一句真正的
// 原因，几 KB 足够。
constexpr int kMaxDiagnosticBytes = 4 * 1024;

void setErr(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

// --- 诊断沉淀池 -----------------------------------------------------------
//
// 为什么需要它：中继线程断掉的时候，拨号早已成功返回，目标 SshClient 正在
// 做自己的握手。用户看到的是 libssh2 一句 "Failed getting banner"，而真正的
// 原因（"跳板机 bastion-hk 的中继已结束：libssh2 侧已关闭连接"）只有中继线程
// 知道。SshClient 在握手/认证失败时会把这段文字附在错误后面。
struct ChainDiagnostics {
    mutable QMutex mutex;
    QString text;

    void append(const QString &line)
    {
        QMutexLocker lock(&mutex);
        if (text.size() >= kMaxDiagnosticBytes)
            return;
        if (!text.isEmpty())
            text += QLatin1Char('\n');
        text += line;
    }

    QString snapshot() const
    {
        QMutexLocker lock(&mutex);
        return text;
    }
};

// --- 中继状态 -------------------------------------------------------------
//
// 单独提成一个 shared_ptr 持有的结构体（而不是直接放进 JumpChainTransport），
// 理由与 ProxyCommandDialer::PumpState 完全相同：让「等不到线程退出」这种极端
// 情况能**安全地泄漏**——线程体自己捏着一份 shared_ptr，句柄对象析构掉也不会
// 把它脚下的内存抽走，没有 UAF 窗口。
struct RelayState {
    // 通道挂在谁的 session 上。裸指针：所有权在 JumpChainTransport 手里，
    // 而它保证「线程 join 成功之后才 delete client」（join 不成功就一起泄漏）。
    SshClient *owner = nullptr;
    LIBSSH2_CHANNEL *channel = nullptr;

    // 中继线程这一端。**只由本结构体的析构负责回收**——别处一律只 shutdown
    // 不 close，否则 fd 号会被系统重新分配给别人，而 shutdown() 还在往那个号上打。
    qintptr serverEnd = kInvalidSocket;

    // 给诊断消息用的可读名字（"跳板机 bastion-hk" / "目标主机"）。
    QString label;
    std::shared_ptr<ChainDiagnostics> diag;

    std::atomic<bool> stop{false};

    ~RelayState() { closeSocket(serverEnd); }
};

// 非阻塞地往通道写一次。返回值三态，与 socket_util::sendNonBlocking 对齐：
//   >= 0 实际写出的字节数（**可能小于 size**）
//   -1   这一刻写不进去（EAGAIN），稍后再来
//   -2   真的出错
//
// 为什么不用 SshClient::channelWrite：那个会一直循环到全部写完，中途还
// waitSocket(50) 睡着等。双向中继在同一个线程里搬两个方向，一旦某个方向的
// 对端不读，它会把整个线程按在原地，另一个方向就活活饿死——而这两个方向
// **互为对方的消费者**，于是不是饿死而是死锁：目标 libssh2 写满 socket pair
// 之后就不再读，我们却正卡在往它那一端写。见 SocketUtil.h 里
// sendNonBlocking 的注释（同一条理由）。
qint64 channelWriteSome(SshClient *client, LIBSSH2_CHANNEL *channel,
                        const char *data, qint64 size)
{
    QMutexLocker lock(&client->sessionLock());
    if (!client->rawSession())
        return -2;
    const ssize_t n = libssh2_channel_write(channel, data, size_t(size));
    if (n == LIBSSH2_ERROR_EAGAIN)
        return -1;
    if (n < 0)
        return -2;
    return qint64(n);
}

// 两边都没进展时睡一小会儿。
//
// 想读哪一边就只挂哪一边，这不是优化而是必需：假如「已经有数据攒着等写通道」
// 时仍然挂上 socket 的可读事件，那 socket 上剩余的数据会让 select 立刻返回，
// 而这一轮什么都做不了（缓冲区没腾空），于是变成 100% CPU 的忙转。
void waitRelay(qintptr sock, qintptr hopSock,
               bool wantSockRead, bool wantSockWrite,
               bool wantHopRead, bool wantHopWrite)
{
    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxFd = 0;
    const auto add = [&](qintptr fd, fd_set &set) {
        if (fd < 0)
            return;
        FD_SET(fd, &set);
        maxFd = qMax(maxFd, int(fd));
    };
    if (wantSockRead)  add(sock, rfds);
    if (wantSockWrite) add(sock, wfds);
    if (wantHopRead)   add(hopSock, rfds);
    if (wantHopWrite)  add(hopSock, wfds);

    // 一定要有超时上限：libssh2 可能把解密后的数据攒在自己的缓冲里，socket 上
    // 却没有任何可读事件；同样地 stop 标志也得有人回头看。切片到点就重试一轮，
    // 于是所有"醒不过来"的情形整类被兜掉。
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = kSelectSliceMs * 1000;
    ::select(maxFd + 1, &rfds, &wfds, nullptr, &tv);
}

// 中继线程主体。st 按值捕获（shared_ptr）——见 RelayState 的注释。
//
// 一个线程搬两个方向，两个方向都非阻塞 + 各留一个待写缓冲。为什么不能一个方向
// 一个线程：那样两条线程会同时抢 sessionLock，而持锁的那条在 libssh2 内部还要
// 等 socket，另一条就被它按死——等价于回到阻塞 session 的死锁。
void runRelay(std::shared_ptr<RelayState> st)
{
    SshClient *const client = st->owner;
    LIBSSH2_CHANNEL *const channel = st->channel;
    const qintptr sock = st->serverEnd;

    QByteArray toChannel;   // 从 socket 读到、还没写进通道的
    QByteArray toSocket;    // 从通道读到、还没写进 socket 的
    bool socketEof = false;
    bool channelDone = false;
    QString why;

    while (!st->stop.load()) {
        // 跳板机那条 session 死了（网断/被 shutdownSocket 打断），继续搬没有意义。
        if (!client->isTransportAlive()) {
            why = QStringLiteral("上一跳的 SSH 会话已断开");
            break;
        }

        bool progressed = false;

        // --- socket（目标 libssh2 写出的）→ 通道 ---
        // 只在待写缓冲空着时才去读：这就是背压，否则慢链路上会把内存吃光。
        if (toChannel.isEmpty() && !socketEof) {
            char buf[kRelayChunkBytes];
            QString err;
            const qint64 n = recvNonBlocking(sock, buf, sizeof(buf), &err);
            if (n > 0) {
                toChannel.append(buf, int(n));
                progressed = true;
            } else if (n == 0) {
                socketEof = true;
                progressed = true;
            } else if (n == -2) {
                why = QStringLiteral("读取本地侧失败：%1").arg(err);
                break;
            }
        }
        if (!toChannel.isEmpty()) {
            const qint64 n = channelWriteSome(client, channel, toChannel.constData(),
                                              toChannel.size());
            if (n > 0) {
                toChannel.remove(0, int(n));
                progressed = true;
            } else if (n == -2) {
                why = QStringLiteral("写入上一跳的转发通道失败");
                break;
            }
        }
        // 目标那一端已经关了、且攒着的字节也送完了：这条中继的活干完了。
        // 通道的 EOF/close 不在这里发，交给 freeChannel()——它会把通道翻回阻塞
        // 再 close，那才是能真正把报文发出去的时机。
        if (socketEof && toChannel.isEmpty()) {
            why = QStringLiteral("本地侧已关闭连接");
            break;
        }

        // --- 通道 → socket（喂给目标 libssh2 读）---
        if (toSocket.isEmpty() && !channelDone) {
            bool wouldBlock = false;
            const QByteArray data = client->channelRead(channel, kRelayChunkBytes, &wouldBlock);
            if (!data.isEmpty()) {
                toSocket = data;
                progressed = true;
            } else if (!wouldBlock) {
                // channelRead 把 EOF 和硬错误都压成"空且不 wouldBlock"。
                // 两者的处置一样（收工），只在诊断文本里分开说。
                channelDone = true;
                progressed = true;
            }
        }
        if (!toSocket.isEmpty()) {
            QString err;
            const qint64 n = sendNonBlocking(sock, toSocket.constData(), toSocket.size(), &err);
            if (n > 0) {
                toSocket.remove(0, int(n));
                progressed = true;
            } else if (n == -2) {
                why = QStringLiteral("写回本地侧失败：%1").arg(err);
                break;
            }
        }
        if (channelDone && toSocket.isEmpty()) {
            why = client->channelEof(channel)
                      ? QStringLiteral("转发通道已被对端关闭")
                      : QStringLiteral("转发通道读取失败");
            break;
        }

        if (!progressed) {
            waitRelay(sock, client->socketFd(),
                      /*wantSockRead*/ toChannel.isEmpty() && !socketEof,
                      /*wantSockWrite*/ !toSocket.isEmpty(),
                      /*wantHopRead*/ toSocket.isEmpty() && !channelDone,
                      /*wantHopWrite*/ !toChannel.isEmpty());
        }
    }

    // 让目标那一端立刻看到 EOF，而不是守着一个再也不会来字节的 fd 等超时。
    // shutdown 而不是 close——fd 的回收归 ~RelayState。
    shutdownFd(sock);

    // 被主动停掉（拆链/取消）是正常收尾，不算诊断信息；只有"自己断的"才留痕。
    if (!st->stop.load() && !why.isEmpty() && st->diag) {
        st->diag->append(QStringLiteral("%1 的转发中继已结束：%2").arg(st->label, why));
    }
    qCDebug(proxyLog).noquote() << st->label << "转发中继结束：" << why;
}

// 断开之前把 session 翻回阻塞并给一个短超时。
//
// 非阻塞 session 上 libssh2_session_disconnect 会返回 EAGAIN 就走，那句 "bye"
// 根本发不出去；更要紧的是 libssh2_session_free 同样会 EAGAIN 返回，而
// SshClient::disconnectFromHost 忽略返回值并把指针置空——于是每拆一跳漏一个
// session。翻回阻塞 + 给个上限，既能把 "bye" 发出去，也不会在拆链时卡住。
//
// 前提是这一跳的**出向**中继已经停掉：那条线程也要抢 sessionLock，还在跑的话
// 这里的阻塞调用会和它互相等。拆除顺序保证了这一点（见 ~JumpChainTransport）。
void prepareGracefulClose(SshClient *client)
{
    QMutexLocker lock(&client->sessionLock());
    if (LIBSSH2_SESSION *session = client->rawSession()) {
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, kGracefulCloseMs);
    }
}

// 一段「上一跳的通道 ↔ socket pair」链接。
struct RelayLink {
    std::shared_ptr<RelayState> state;
    QThread *thread = nullptr;
    // 通道挂在谁的 session 上（= 上一跳）。freeChannel 要经它调。
    SshClient *owner = nullptr;
    LIBSSH2_CHANNEL *channel = nullptr;

    bool isEmpty() const { return !state && !channel; }
};

// 链上的一跳。
struct ChainHop {
    SshClient *client = nullptr;
    QString label;
    // 用于连到本跳的那段链接。第 0 跳是直接 TCP 拨号，此处为空。
    RelayLink inbound;
};

// 「跳转服务器」的载体句柄。生命周期由目标 SshClient 持有（见 ProxyTransport）。
class JumpChainTransport : public ProxyTransport {
public:
    explicit JumpChainTransport(std::shared_ptr<ChainDiagnostics> diag)
        : m_diag(std::move(diag))
    {
    }

    ~JumpChainTransport() override
    {
        // 刻意**不**先调 shutdown()：那会把每一跳的 inbound 中继一起打断，
        // 而下面每一跳的 libssh2_session_disconnect（那句 "bye"）正要经它发出去。
        // 中继线程本身是非阻塞循环、每片最多睡 kSelectSliceMs，靠各自的 stop
        // 标志就能按时退出，不需要提前统一打断。
        //
        // 拆除顺序：从**最里面**往外。目标那一段先拆，然后每一跳「先断自己、
        // 再拆自己的 inbound」。这样每一跳发 "bye" 的时候，承载它的那条链接还活着
        // ——顺序反了就是在说话中间把话筒拔掉（同 SshClient::disconnectFromHost
        // 里"最后才拆载体"那条注释）。
        bool abandoned = !teardownLink(m_finalLink);

        for (int i = m_hops.size() - 1; i >= 0; --i) {
            ChainHop &hop = m_hops[i];
            if (abandoned) {
                // 前面有中继线程没能按时退出。它可能还在碰这一跳的 session 和
                // 通道，**宁可泄漏也不能删**（同 ProxyCommandDialer 那段 detach）：
                // 删一个还在被使用的 session 是 UAF，泄漏只是把一个线程 + 一条
                // 连接留到进程结束。
                qCWarning(proxyLog).noquote()
                    << "放弃回收跳板链剩余部分（泄漏一跳）：" << hop.label;
                continue;
            }
            if (hop.client) {
                prepareGracefulClose(hop.client);
                hop.client->disconnectFromHost();
                delete hop.client;
                hop.client = nullptr;
            }
            if (!teardownLink(hop.inbound))
                abandoned = true;
        }
        m_hops.clear();
    }

    void shutdown() override
    {
        // 从任意线程调、可重复调。只置标志 + shutdown fd，不 join：这条路径
        // 服务的是"关标签页"，它的全部意义就是不会堵。
        //
        // 每一跳的 socket 也要打断：中继线程醒了之后会去查 isTransportAlive()，
        // 而正卡在 libssh2 内部 select 上的那一跳不打断就不会醒。
        interruptLink(m_finalLink);
        for (ChainHop &hop : m_hops) {
            if (hop.client)
                hop.client->shutdownSocket();
            interruptLink(hop.inbound);
        }
    }

    QString diagnostics() const override
    {
        return m_diag ? m_diag->snapshot() : QString();
    }

    // 整条链有任何一跳用了 keyboard-interactive 动态码，这条载体就不能在无人
    // 交互的前提下原样重建。SftpTransferPool 靠它决定能不能克隆兄弟连接做并行
    // 传输——不把跳板算进来的话，"目标用密码 + 跳板用 OTP" 会被判成可克隆，
    // 然后每开一个克隆都去弹一次动态码框。
    bool isReplayable() const override
    {
        for (const ChainHop &hop : m_hops) {
            if (hop.client && !hop.client->isAuthReplayable())
                return false;
        }
        return true;
    }

    // --- 建链期用的写入口 ---
    void appendHop(ChainHop hop) { m_hops.append(std::move(hop)); }
    void setFinalLink(RelayLink link) { m_finalLink = std::move(link); }
    SshClient *lastClient() const
    {
        return m_hops.isEmpty() ? nullptr : m_hops.last().client;
    }
    // 最里面那一跳的可读名字。中继线程的诊断要报的是**承载它的那一跳**
    // （通道挂在谁的 session 上），而不是它连向的目标。
    QString lastHopLabel() const
    {
        return m_hops.isEmpty() ? QString() : m_hops.last().label;
    }

private:
    static void interruptLink(RelayLink &link)
    {
        if (!link.state)
            return;
        link.state->stop.store(true);
        shutdownFd(link.state->serverEnd);
    }

    // 停中继 → join → 放通道。返回 false 表示线程没在预算内退出，调用方
    // 必须放弃回收后面的一切（见析构里的 abandoned）。
    static bool teardownLink(RelayLink &link)
    {
        if (link.isEmpty())
            return true;
        interruptLink(link);
        if (link.thread) {
            if (!link.thread->wait(kRelayJoinMs)) {
                qCWarning(proxyLog) << "跳板链中继线程未在" << kRelayJoinMs
                                    << "ms 内退出，放弃回收（泄漏一个线程）";
                link.thread = nullptr;   // 删一个还在跑的 QThread 是 UB
                return false;
            }
            delete link.thread;
            link.thread = nullptr;
        }
        // 必须在线程 join 之后：中继线程一直在碰这个通道。
        if (link.channel && link.owner) {
            link.owner->freeChannel(link.channel);
            link.channel = nullptr;
        }
        return true;
    }

    QList<ChainHop> m_hops;
    RelayLink m_finalLink;
    std::shared_ptr<ChainDiagnostics> m_diag;
};

// 在 owner 上开一条到 destHost:destPort 的 direct-tcpip 通道，并把它接到一对
// 本地 socket 上。成功时返回给调用方用的那一端（clientEnd），链接句柄放进
// linkOut；失败返回 kInvalidSocket。
//
// budgetMs：开通道这一步的上限。session 在认证完成后被显式设成了"永不超时"
//（见 SshClient::finishHandshake 末尾——那是为了不让大文件传输被判超时），
// 所以这里得自己临时加一个，否则跳板机只应答 TCP 却不处理通道请求时会永久挂住。
qintptr openRelayedLink(SshClient *owner, const QString &destHost, quint16 destPort,
                        const QString &label,
                        const std::shared_ptr<ChainDiagnostics> &diag,
                        int budgetMs, RelayLink *linkOut, QString *errorOut)
{
    // --- 开通道。此刻 owner 的 session 还是阻塞模式（见文件头那条不变量）---
    {
        QMutexLocker lock(&owner->sessionLock());
        if (LIBSSH2_SESSION *session = owner->rawSession())
            libssh2_session_set_timeout(session, long(qMax(1, budgetMs)));
    }
    SshError chErr;
    // 源地址只是报给服务端记日志用的，填回环即可。
    LIBSSH2_CHANNEL *channel = owner->openDirectTcpip(destHost, destPort,
                                                      QStringLiteral("127.0.0.1"), 0, chErr);
    {
        QMutexLocker lock(&owner->sessionLock());
        if (LIBSSH2_SESSION *session = owner->rawSession())
            libssh2_session_set_timeout(session, 0);
    }
    if (!channel) {
        setErr(errorOut, QStringLiteral("在上一跳上打开到 %1:%2 的转发通道失败：%3")
                             .arg(destHost).arg(destPort)
                             .arg(chErr.message.isEmpty() ? QStringLiteral("未知错误")
                                                          : chErr.message));
        return kInvalidSocket;
    }

    auto st = std::make_shared<RelayState>();
    st->owner   = owner;
    st->channel = channel;
    st->label   = label;
    st->diag    = diag;

    qintptr clientEnd = kInvalidSocket;
    QString pairErr;
    if (!makeSocketPair(clientEnd, st->serverEnd, &pairErr)) {
        owner->freeChannel(channel);
        setErr(errorOut, QStringLiteral("为跳板链建立本地 socket 失败：%1").arg(pairErr));
        return kInvalidSocket;
    }
    // 中继这一端必须非阻塞（整个循环都建立在这个前提上）。交给 libssh2 的那一端
    // 保持阻塞——与直连产物的状态一致，后面的 handshake/openShell 才不用改。
    setNonBlocking(st->serverEnd, true);
    // 回环 TCP 的默认缓冲在 macOS 上偏小，SSH-over-SSH 跑 SFTP 时两个方向同时
    // 打满，缓冲太小会让中继线程频繁在 send 上打转。
    setSocketBuffers(st->serverEnd, kPairSocketBufferBytes);
    setSocketBuffers(clientEnd, kPairSocketBufferBytes);

    // 起中继**之前**把 owner 翻成非阻塞：顺序见文件头那条不变量。
    owner->setTransportNonBlocking();

    QThread *thread = QThread::create([st]() { runRelay(st); });
    thread->setObjectName(QStringLiteral("ssh-jump-relay"));
    thread->start();

    linkOut->state   = std::move(st);
    linkOut->thread  = thread;
    linkOut->owner   = owner;
    linkOut->channel = channel;
    return clientEnd;
}

// 跳板机自己那份代理配置里，真正还用得上的只有「HTTP / SOCKS5 / 代理命令」
// ——它们描述的是"怎么建立到这台跳板机的 TCP 连接"，只对**第一跳**有意义
//（第二跳往后的传输是上一跳的通道，压根没有拨号这一步）。
//
// 两种要摘掉：
//   * JumpHost —— 那条链已经被 flattenJumpChain 展平进本次的 hops 了，
//     留着会把同几台机器再连一遍；
//   * Global/System 解析出来又是 JumpHost —— 那是无限递归的入口：
//     A.proxy=Global、全局代理=JumpHost[X]、X.proxy=Global，于是连 X 要先连 X。
//     展平那一层看不到这种绕道（X.proxy.hopIds 是空的），所以必须在这里掐断。
//
// 摘掉之后按直连处理，并留一条 warning——静默改行为比报错更难查。
ProxyConfig effectiveHopProxy(const ProxyConfig &proxy, const QString &label)
{
    if (proxy.type == ProxyType::JumpHost) {
        // 这一种是正常情形（嵌套引用已展平），不必告警。
        return ProxyConfig{};
    }
    const ProxyConfig resolved =
        resolveGlobalProxy(proxy, GlobalState::instance().sshProxyConfig());
    if (resolved.type == ProxyType::JumpHost) {
        qCWarning(proxyLog).noquote()
            << label << "的代理指向了跳转服务器，会形成自引用，本次按直连处理";
        return ProxyConfig{};
    }
    // 原样交回去，让 SshClient 自己解析 Global/System——它还要顺带补上
    // 全局代理的口令（那份口令只在内存里，见 GlobalState::sshProxyPassword）。
    return proxy;
}

} // namespace

JumpDialer makeSshJumpDialer(const QList<DeviceEntry> &catalog,
                             const SshPromptCallback &prompt,
                             const HostKeyPromptCallback &hostKeyPrompt)
{
    // 按 id 建索引一次。lambda 按值捕获这份表：建链跑在工作线程上，而
    // DeviceConfigStore 没有锁（见 SshJumpChain.h 对 catalog 的说明）。
    QHash<QString, DeviceEntry> byId;
    byId.reserve(catalog.size());
    for (const DeviceEntry &e : catalog) {
        if (!e.id.isEmpty())
            byId.insert(e.id, e);
    }

    return [byId, prompt, hostKeyPrompt](const QString &targetHost, quint16 targetPort,
                          const QStringList &hops, int timeoutMs,
                          const std::atomic<bool> *cancelled,
                          ProxyTransportPtr *transportOut, QString *errorOut) -> qintptr {
        // 约定：0 = 预算已耗尽，不是"用默认值"（见 SocketUtil.h 那段）。
        if (timeoutMs == 0) {
            setErr(errorOut, QStringLiteral("跳板链没有可用的建连预算（超时已耗尽）"));
            return kInvalidSocket;
        }
        if (cancelled && cancelled->load()) {
            setErr(errorOut, QStringLiteral("连接已取消"));
            return kInvalidSocket;
        }
        if (!transportOut) {
            // 没人接手句柄，整条链会在本函数返回时立刻被拆掉，交出去的 fd 随即
            // 变成一根断管。这是调用方的编程错误——宁可在这里明确失败，也不要
            // 交出一个几毫秒后开始随机掉线的连接（同 makeProxyCommandDialer）。
            setErr(errorOut, QStringLiteral("内部错误：跳板链的载体句柄无人接管"));
            return kInvalidSocket;
        }

        // --- 展平嵌套引用 ---
        // startDeviceId 传空：JumpDialer 的签名里没有目标设备 id（它是 net/ 层
        // 定义的，那一层不认识设备）。少掉的只是"目标把自己列成跳板"这一种
        // 预置检测，而那种配置绕一圈之后照样会被 visited 判成环——目标出现在
        // hops 里，展开它自己的 hopIds 时又碰到自己。
        const JumpChainResult chain = flattenJumpChain(QString(), hops,
            [&byId](const QString &id) -> std::optional<JumpHopInfo> {
                const auto it = byId.constFind(id);
                if (it == byId.constEnd())
                    return std::nullopt;
                // 只有类型确实是「跳转服务器」时才算它有下级跳板。类型是 HTTP
                // 代理却填过 hopIds（用户切过类型）时，那份残留不该被当真。
                const QStringList nested = it->proxy.type == ProxyType::JumpHost
                                               ? it->proxy.hopIds
                                               : QStringList{};
                return JumpHopInfo{it->name, nested};
            });
        if (!chain.ok()) {
            setErr(errorOut, chain.error);
            return kInvalidSocket;
        }
        if (chain.hops.isEmpty()) {
            setErr(errorOut, QStringLiteral("未选择跳板机，请至少添加一台"));
            return kInvalidSocket;
        }

        // --- 分预算 ---
        // 展平后的每一跳一份，目标自己那一段（握手 + 认证）再留一份。
        // 上游传进来的 timeoutMs 已经按**直接**跳板数放大过（见
        // SshClient::effectiveConnectTimeoutMs），所以正常配置下每跳分到的
        // 仍是用户在设置里填的那个量级；嵌套引用多出来的跳会摊薄，那是
        // 那个函数刻意留下的下界。
        const int totalMs = timeoutMs > 0 ? timeoutMs : kDefaultConnectTimeoutMs;
        QDeadlineTimer budget(totalMs);
        const int perHopMs = qMax(1, totalMs / (int(chain.hops.size()) + 1));

        auto diag = std::make_shared<ChainDiagnostics>();
        auto transport = std::make_shared<JumpChainTransport>(diag);

        // --- 逐跳建链 ---
        for (int i = 0; i < chain.hops.size(); ++i) {
            // 取消只在跳与跳之间查得到：一跳的 connectToHost 一旦进去就没有
            // 外部打断口（那个对象的 shutdownSocket 我们没有交给任何人）。
            // 每跳有 perHopMs 的上限兜着，所以最坏多等一跳的预算。
            if (cancelled && cancelled->load()) {
                setErr(errorOut, QStringLiteral("连接已取消"));
                return kInvalidSocket;
            }
            if (budget.hasExpired()) {
                setErr(errorOut, QStringLiteral("建立跳板链超时（总预算 %1 ms）").arg(totalMs));
                return kInvalidSocket;
            }

            const QString hopId = chain.hops.at(i);
            const auto it = byId.constFind(hopId);
            if (it == byId.constEnd()) {
                // 展平那一层已经查过一遍了，走到这里说明表在两次查询之间变了。
                setErr(errorOut, QStringLiteral("跳板机 %1 已不存在（可能已被删除）").arg(hopId));
                return kInvalidSocket;
            }
            const DeviceEntry hop = *it;
            const QString label = QStringLiteral("跳板机 %1")
                                      .arg(hop.name.isEmpty() ? hopId : hop.name);

            // 跳板机必须能开 direct-tcpip 通道，串口/RDP/裸 TCP 条目做不到。
            // UI 的候选列表已经只放 SSH 设备，这里兜的是手改配置文件。
            if (!hop.isSsh()) {
                setErr(errorOut, QStringLiteral("%1 不是 SSH 设备，不能用作跳板机").arg(label));
                return kInvalidSocket;
            }
            const HostPort hp = hop.hostPort();
            if (hp.host.isEmpty()) {
                setErr(errorOut, QStringLiteral("%1 没有填主机地址").arg(label));
                return kInvalidSocket;
            }

            ChainHop entry;
            entry.label  = label;
            entry.client = new SshClient();
            entry.client->setHost(hp.host, hp.port);
            entry.client->setUsername(hop.username);
            // ssh-agent 的跳板：credentialKind 必须带上，否则 keyFile/password
            // 都为空时 authenticate() 会落到空密码认证（同 ConnectionTester）。
            // agent 句柄每跳各自 init，不跨 session 复用（方案 §2.6）。
            entry.client->setCredentialKind(hop.credentialKind);
            if (hop.usesKey())
                entry.client->setPrivateKey(hop.keyType, hop.keyFile);
            else
                entry.client->setPassword(hop.password);
            entry.client->setConnectTimeoutMs(
                int(qMin<qint64>(perHopMs, qMax<qint64>(1, budget.remainingTime()))));

            // 每跳的动态码提示都要带上是谁在问：原样复用目标的回调，用户看到的
            // 是一句光秃秃的 "Verification code:"，分不清是哪一跳。
            SshPromptCallback hopPrompt;
            if (prompt) {
                hopPrompt = [prompt, label](const QString &text, bool echo) {
                    return prompt(QStringLiteral("%1：%2").arg(label, text), echo);
                };
            }

            // 主机密钥校验同样要覆盖每一跳——但只在有人能应答弹窗时。
            //
            // 校验模式为「询问」时遇到未知主机必须问人；没有回调的调用方
            // （SFTP 并行克隆、隧道、proxy_integration_test 这类 headless 路径）
            // 问了没人答，fail-closed 会把整条链打死。这些路径退回接入本功能
            // 之前的行为（不校验），与「SshClient 未设 store 即跳过校验」一致：
            // 交互终端那条首连仍然走完整校验并把指纹写进 known_hosts。
            if (hostKeyPrompt) {
                entry.client->setKnownHostsStore(KnownHostsStore::defaultInstance());
                entry.client->setHostKeyVerification(static_cast<HostKeyVerification>(
                    GlobalState::instance().hostKeyVerification()));
                entry.client->setHostKeyPromptCallback(
                    [hostKeyPrompt, label](const QString &host, quint16 port,
                                           const QString &fingerprint,
                                           const QString &keyType, bool changed) {
                        return hostKeyPrompt(label, port, fingerprint, keyType, changed);
                    });
            }

            SshError hopErr;
            if (i == 0) {
                // 第一跳是真的拨号，尊重它自己那份代理配置（它可能在 HTTP
                // 代理后面）。JumpHost / 绕回 JumpHost 的要摘掉，见 effectiveHopProxy。
                entry.client->setProxyConfig(effectiveHopProxy(hop.proxy, label));
                if (!entry.client->connectToHost(hopPrompt, hopErr)) {
                    delete entry.client;
                    setErr(errorOut, QStringLiteral("连接%1（%2:%3）失败：%4")
                                         .arg(label, hp.host).arg(hp.port).arg(hopErr.message));
                    return kInvalidSocket;
                }
            } else {
                // 第二跳往后：在上一跳上开通道，经 socket pair 变成普通 fd。
                SshClient *const owner = transport->lastClient();
                QString linkErr;
                const qintptr fd = openRelayedLink(
                    owner, hp.host, hp.port, transport->lastHopLabel(), diag,
                    int(qMin<qint64>(perHopMs, qMax<qint64>(1, budget.remainingTime()))),
                    &entry.inbound, &linkErr);
                if (fd < 0) {
                    delete entry.client;
                    setErr(errorOut, QStringLiteral("经上一跳连接%1（%2:%3）失败：%4")
                                         .arg(label, hp.host).arg(hp.port).arg(linkErr));
                    return kInvalidSocket;
                }
                // 载体句柄留在 entry.inbound 里由 JumpChainTransport 统一管，
                // 不交给这一跳的 SshClient：整条链的拆除顺序必须集中决定
                //（见 ~JumpChainTransport），分散到每个 client 手里就没法保证。
                if (!entry.client->connectOverSocket(fd, nullptr, hopPrompt, hopErr)) {
                    // connectOverSocket 失败时也已接管 fd（会自己关掉）。
                    // entry.inbound 还得挂进 transport，否则这条中继线程没人拆。
                    const QString msg = hopErr.message;
                    delete entry.client;
                    entry.client = nullptr;
                    transport->appendHop(std::move(entry));
                    setErr(errorOut, QStringLiteral("经上一跳连接%1（%2:%3）失败：%4")
                                         .arg(label, hp.host).arg(hp.port).arg(msg));
                    return kInvalidSocket;
                }
            }

            qCDebug(proxyLog).noquote() << "跳板链第" << (i + 1) << "跳已就绪："
                                        << label << hp.host << hp.port;
            transport->appendHop(std::move(entry));
        }

        // --- 最后一段：从最里面那一跳开到真正的目标 ---
        if (cancelled && cancelled->load()) {
            setErr(errorOut, QStringLiteral("连接已取消"));
            return kInvalidSocket;
        }
        RelayLink finalLink;
        QString linkErr;
        const qintptr fd = openRelayedLink(
            transport->lastClient(), targetHost, targetPort, transport->lastHopLabel(), diag,
            int(qMax<qint64>(1, budget.remainingTime())), &finalLink, &linkErr);
        if (fd < 0) {
            setErr(errorOut, QStringLiteral("经跳板机连接目标 %1:%2 失败：%3")
                                 .arg(targetHost).arg(targetPort).arg(linkErr));
            return kInvalidSocket;
        }
        transport->setFinalLink(std::move(finalLink));

        *transportOut = std::move(transport);
        qCDebug(proxyLog).noquote() << "跳板链已就绪，共" << chain.hops.size()
                                    << "跳，目标" << targetHost << targetPort;
        return fd;
    };
}

} // namespace cubeshell
