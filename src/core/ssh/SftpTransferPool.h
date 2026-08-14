#pragma once

// SftpTransferPool.h — 并行 SFTP 传输的连接池。
//
// 为什么需要它：
// libssh2 把所有通道（交互 shell、端口转发、SFTP）复用在同一个
// LIBSSH2_SESSION 上，而 session 不是线程安全的 —— 本工程因此用
// SshClient::sessionLock() 把每一次 libssh2 调用串起来。结果是即便上层开了
// 线程池，SFTP 传输仍然全局串行：同一时刻只有一个分片在飞。而 SFTP 是
// 请求/应答协议，单流吞吐 ≈ chunk / RTT，跟带宽无关，加线程不加连接毫无收益。
//
// 本类的做法：为传输另开若干条独立 SSH 连接（各自一个 LIBSSH2_SESSION +
// 一个 SFTP 子系统通道），每条流独占一条，彼此无锁竞争，真正并行。凭据从主
// 连接克隆（SshClient::host()/username()/password()/keyFile() ...）。
//
// 克隆不了时的多路复用（jms 一次性 token / keyboard-interactive MFA）：
// SSH 协议允许一条连接上复用多条 channel，开新的 SFTP 通道不需要重新认证 ——
// 一次性/动态码凭据的限制天然碰不到。此时本池在主 session 上开若干条独立的
// SFTP 通道（通道槽），每条传输流各用一条通道 + 自己的远端 handle。所有通道
// 仍共享 sessionLock，但调用方按「逐调用加锁、EAGAIN 锁外等待」的纪律
// （见 SftpUploaderCore::sftpCallInt），多条通道的分片在一条 TCP 连接上真正
// 交错在飞 —— 吞吐比锁步串行高一个量级，且 UI 线程的终端写入不再被堵。
// 极限带宽受单连接拥塞窗口限制，不如独立连接，但远优于串行降级。
//
// 降级策略（功能不受影响，只是不加速）：
//  - 主连接用 keyboard-interactive/OTP 登录（isAuthReplayable() == false）：
//    无法静默重放认证，不克隆（绝不弹框骚扰用户再输动态码），走上面的通道
//    多路复用。
//  - 克隆连接失败（服务端 MaxSessions/MaxStartups 限制、网络抖动、
//    fail2ban 等）：记下已达上限，之后不再尝试，用已建成的连接继续。
//  - 通道也开不出来（MaxSessions 打满）：退回共享主 SFTP 通道串行。
//
// 线程模型：lease() 可从任意工作线程调用；连接/通道在首次被租用的那个线程上
// 惰性建立。每个租约都带一把锁（克隆槽是自己的锁，主连接槽/通道槽是主连接的
// sessionLock()）。独占克隆上这把锁无竞争，可整段持锁；共享 session 上必须
// 逐调用加锁、EAGAIN 锁外等待（SftpUploaderCore 的 sftpCall* 统一处理两种，
// 调用方不需要两套写法）。

#include <QMutex>
#include <QRecursiveMutex>
#include <QString>
#include <QThreadPool>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <memory>

struct _LIBSSH2_SFTP;

namespace cubeshell {

class SshClient;

class SftpTransferPool {
public:
    // 默认并行连接数。SFTP 单流受 RTT 限制，4 条在常见链路上已能把带宽吃满；
    // 再多主要是撞 sshd 的 MaxSessions(默认10)/MaxStartups，收益递减。
    static constexpr int kDefaultMaxConnections = 4;

    // primary: 已连接的主 SshClient（不拥有，必须比本对象活得久）。
    explicit SftpTransferPool(SshClient *primary);
    ~SftpTransferPool();

    SftpTransferPool(const SftpTransferPool &) = delete;
    SftpTransferPool &operator=(const SftpTransferPool &) = delete;

    void setPrimary(SshClient *primary);
    // 并行连接上限（含主连接）。<=1 时完全退回串行。
    void setMaxConnections(int n);
    int maxConnections() const;

    // 允许/禁止克隆独立连接（默认允许）。禁用后并行改走主 session 的通道
    // 多路复用 —— 适合对并发连接数敏感的服务端（MaxStartups / fail2ban /
    // 审计要求单连接），测试也用它确定性覆盖通道路径。对凭据不可重放的
    // 连接（jms token / MFA）本就不可能克隆，此开关无额外影响。
    void setCloningEnabled(bool enabled);
    bool cloningEnabled() const;

    // 本池当前实际能提供的并行度（已建成的连接/通道数，至少 1）。
    int activeConnections() const;
    // 是否能并行传输：可克隆（凭据可重放）→ 独立连接并行；不可克隆
    // （jms 一次性 token / MFA）→ 主 session 通道多路复用并行。两者皆不可
    // （无主连接 / maxConnections<=1 / 通道也开不出来）时为 false。
    bool canParallelize() const;

    // 对所有克隆连接的 socket 调 shutdown()，让阻塞在 libssh2_sftp_read/write
    // 里的传输线程立刻带错误返回。
    //
    // 克隆连接是**阻塞模式** session（没调过 openShell），阻塞中的 libssh2 调用
    // 不会回到我们的循环里去看取消标志，只会挂到 TCP 超时（可能几十秒）。而
    // SftpClient 析构时对 worker 的 join 是有超时的 —— 超时后析构继续往下走，
    // 后台线程就会访问已释放的对象。所以取消/析构路径必须先在这里把 socket
    // 打断，再去 join。与 SshClient::shutdownSocket() 在标签页关闭路径上的用法同源。
    //
    // 只 shutdown 不 close：fd 仍由各 SshClient 持有，稍后正常析构释放。
    // 通道槽与主连接槽跳过：它们共享主连接的 socket，shutdown 会误杀终端会话。
    void shutdownTransferSockets();

    // 主连接的 socket 已被外部关闭（标签页/应用退出）：此后不能再在主 session
    // 上做任何网络调用，否则往死 socket 写会崩。只丢弃本池在主连接上开的 SFTP
    // 通道指针，不做 shutdown 往返。对应 SftpClient::abandon() 的语义。
    void abandonPrimary();

    // 一条连接/通道。lock 必须在每次 libssh2 调用外持有；dedicated 为 false 表示
    // 这是与 shell 共享的主 session（此时 lock 就是主连接的 sessionLock，会与
    // 终端读写竞争，调用方必须按「逐调用加锁、EAGAIN 锁外等待」的纪律使用，
    // 见 SftpUploaderCore.cpp 的 sftpCallInt/sftpCallPtr）。
    struct Connection {
        SshClient *client = nullptr;
        _LIBSSH2_SFTP *sftp = nullptr;
        QRecursiveMutex *lock = nullptr;
        bool dedicated = false;
        bool isValid() const { return client && sftp && lock; }
    };

    // 租约（RAII）：析构自动归还。取不到可用连接时 isValid() 为 false。
    class Lease {
    public:
        Lease() = default;
        Lease(SftpTransferPool *pool, int slot, Connection conn)
            : m_pool(pool), m_slot(slot), m_conn(conn) {}
        ~Lease() { release(); }
        Lease(Lease &&other) noexcept { *this = std::move(other); }
        Lease &operator=(Lease &&other) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        void release();
        bool isValid() const { return m_conn.isValid(); }
        SshClient *client() const { return m_conn.client; }
        _LIBSSH2_SFTP *sftp() const { return m_conn.sftp; }
        QRecursiveMutex *lock() const { return m_conn.lock; }
        bool isDedicated() const { return m_conn.dedicated; }

    private:
        SftpTransferPool *m_pool = nullptr;
        int m_slot = -1;
        Connection m_conn;
    };

    // 租一条连接/通道。已建成的空闲克隆优先，其次空闲通道槽；没有空闲但未达
    // 上限则新建（克隆优先，主 session 通道兜底）；克隆连接在忙时可被共享复用
    // （阻塞 session 每次调用原子完成，共享安全）；主 session 通道与共享主通道
    // 则一律独占 —— 同一条 SFTP 通道绝不让两个工作线程交错发请求（libssh2 按
    // request_id 配对，跨线程交错会错乱挂死，实测如此），租不到独占通道就按
    // 「可取消的有限等待」等空闲槽，而不是共享。
    //
    // cancel：可选取消标志。等待空闲槽期间若置位，立即返回无效租约（调用方把它
    // 当取消处理，不当错误）。errorOut 仅在完全拿不到可用连接（主连接也失效）时填。
    Lease lease(QString *errorOut = nullptr, const std::atomic<bool> *cancel = nullptr);

    // 后台预建传输连接/通道，让首次传输不必现场付握手成本。
    //
    // 为什么必须有：每条克隆连接要走完整的 TCP + SSH 握手 + 认证，实测在
    // 25ms RTT 的链路上约 300ms。懒建连接会把这个成本直接压在首次传输的关键
    // 路径上 —— 实测 16MB 文件用 4 条连接反而比 1 条慢（901ms vs 691ms），
    // 慢的那部分正是握手。预热后这笔成本挪到用户点上传之前，不再计入传输。
    // 凭据不可克隆时改为预建主 session 的 SFTP 通道（一次 channel-open 往返，
    // 成本低但仍有）。
    //
    // 立即返回；连接在后台线程上建立。重复调用只补足缺口，不会重复建。
    // count <= 0 表示按 maxConnections() 补满。
    void prewarm(int count = 0);

    // 有界等待预热任务结束；false = 超时仍在跑。预热任务捕获本池 this，
    // 没退完就析构是 UAF——调用方（~SftpClient / ~SftpUploaderCore）应在
    // 拆池前调用，超时就泄漏整池而不是冻住 UI 线程无限等。
    bool waitPrewarmDone(int timeoutMs);

    // 关闭所有克隆连接与主 session 通道槽（主连接本身不动）。断连/换连接时调用。
    // 前置条件：调用时不得有未归还的租约（调用方需先停掉所有传输线程；
    // 传输线程可能阻塞在 socket 上，先调 shutdownTransferSockets() 打断）。
    void closeAll();

private:
    struct Slot {
        std::unique_ptr<SshClient> client; // 克隆连接，池拥有；主连接槽/通道槽为空
        _LIBSSH2_SFTP *sftp = nullptr;
        std::unique_ptr<QRecursiveMutex> lock; // 克隆连接自己的锁；通道槽为空（用主连接 sessionLock）
        bool inUse = false;
        bool isPrimary = false;        // 主连接槽（共享 m_primarySftp 通道）
        bool isPrimaryChannel = false; // 通道槽：主 session 上独立的 SFTP 通道
    };

    // 建一条克隆连接并开好 SFTP 通道；失败返回 false 并置 m_cloneExhausted。
    bool spawnClone(Slot &slot, QString *errorOut);
    // 在主 session 上开一条独立 SFTP 通道（通道槽）；失败返回 false 并置
    // m_channelExhausted。调用方已持 m_lock（内部做网络 I/O 时会短暂放锁）。
    bool spawnPrimaryChannel(Slot &slot);
    void releaseSlot(int slot);
    Connection primaryConnection(QString *errorOut);

    SshClient *m_primary = nullptr;        // 不拥有
    _LIBSSH2_SFTP *m_primarySftp = nullptr; // 主连接上本池自己的 SFTP 通道
    QVector<Slot *> m_slots;               // [0] 恒为主连接槽
    int m_maxConnections = kDefaultMaxConnections;
    // 主连接 socket 已死：所有涉及主连接的清理都只清指针（见 abandonPrimary）。
    bool m_primaryAbandoned = false;
    // 克隆已失败过（服务端限流/网络问题）：不再重复尝试，避免每次传输都
    // 白等一轮握手超时。closeAll() 会复位。
    bool m_cloneExhausted = false;
    // 主 session 上开通道已失败过（MaxSessions 打满等）：不再重复尝试，
    // 退回共享主 SFTP 通道。closeAll() 会复位。
    bool m_channelExhausted = false;
    // 是否允许克隆独立连接（见 setCloningEnabled）。
    bool m_cloningEnabled = true;
    // 预热任务的线程池（后台建连接，不阻塞调用方）。析构时等它跑完，
    // 避免预热线程摸到正在释放的池。
    QThreadPool m_prewarmPool;
    mutable QMutex m_lock; // 保护 m_slots / m_primarySftp / 计数
    // 有空闲槽（含共享主通道槽 [0]）被归还时唤醒 lease() 里等槽的线程。
    QWaitCondition m_slotFreed;

    friend class Lease;
};

} // namespace cubeshell
