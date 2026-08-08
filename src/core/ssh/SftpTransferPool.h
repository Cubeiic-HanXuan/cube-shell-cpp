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
// 降级策略（都退回"用主连接 + sessionLock 串行"，功能不受影响，只是不加速）：
//  - 主连接用 keyboard-interactive/OTP 登录（isAuthReplayable() == false）：
//    无法静默重放认证，不克隆，绝不弹框骚扰用户再输动态码。
//  - 克隆连接失败（服务端 MaxSessions/MaxStartups 限制、网络抖动、
//    fail2ban 等）：记下已达上限，之后不再尝试，用已建成的连接继续。
//
// 线程模型：lease() 可从任意工作线程调用；连接在首次被租用的那个线程上惰性
// 建立。每个租约都带一把该连接自己的递归锁（主连接租约带的就是主连接的
// sessionLock()），调用方统一在锁内做 libssh2 调用 —— 独占连接上这把锁不会
// 有竞争，代码路径与降级路径完全一致，不需要两套写法。

#include <QMutex>
#include <QRecursiveMutex>
#include <QString>
#include <QThreadPool>
#include <QVector>

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

    // 本池当前实际能提供的并行度（已建成的连接数，至少 1）。
    int activeConnections() const;
    // 主连接的认证方式是否允许克隆（false 时并行度恒为 1）。
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
    void shutdownTransferSockets();

    // 主连接的 socket 已被外部关闭（标签页/应用退出）：此后不能再在主 session
    // 上做任何网络调用，否则往死 socket 写会崩。只丢弃本池在主连接上开的 SFTP
    // 通道指针，不做 shutdown 往返。对应 SftpClient::abandon() 的语义。
    void abandonPrimary();

    // 一条连接。lock 必须在每次 libssh2 调用外持有；dedicated 为 false 表示
    // 这是与 shell 共享的主连接（此时 lock 就是主连接的 sessionLock，会与
    // 终端读写竞争，调用方应尽量缩小持锁粒度）。
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

    // 租一条连接。已建成的空闲连接优先；没有空闲但未达上限则新建一条；
    // 无法新建时返回主连接的租约（串行降级）。errorOut 仅在完全拿不到
    // 可用连接（主连接也失效）时填写。
    Lease lease(QString *errorOut = nullptr);

    // 后台预建传输连接，让首次传输不必现场付握手成本。
    //
    // 为什么必须有：每条克隆连接要走完整的 TCP + SSH 握手 + 认证，实测在
    // 25ms RTT 的链路上约 300ms。懒建连接会把这个成本直接压在首次传输的关键
    // 路径上 —— 实测 16MB 文件用 4 条连接反而比 1 条慢（901ms vs 691ms），
    // 慢的那部分正是握手。预热后这笔成本挪到用户点上传之前，不再计入传输。
    //
    // 立即返回；连接在后台线程上建立。重复调用只补足缺口，不会重复建。
    // count <= 0 表示按 maxConnections() 补满。
    void prewarm(int count = 0);

    // 关闭所有克隆连接（主连接不动）。断连/换连接时调用。
    // 前置条件：调用时不得有未归还的租约（调用方需先停掉所有传输线程；
    // 传输线程可能阻塞在 socket 上，先调 shutdownTransferSockets() 打断）。
    void closeAll();

private:
    struct Slot {
        std::unique_ptr<SshClient> client; // 克隆连接，池拥有；主连接槽为空
        _LIBSSH2_SFTP *sftp = nullptr;
        std::unique_ptr<QRecursiveMutex> lock; // 克隆连接自己的锁
        bool inUse = false;
        bool isPrimary = false;
    };

    // 建一条克隆连接并开好 SFTP 通道；失败返回 false 并置 m_cloneExhausted。
    bool spawnClone(Slot &slot, QString *errorOut);
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
    // 预热任务的线程池（后台建连接，不阻塞调用方）。析构时等它跑完，
    // 避免预热线程摸到正在释放的池。
    QThreadPool m_prewarmPool;
    mutable QMutex m_lock; // 保护 m_slots / m_primarySftp / 计数

    friend class Lease;
};

} // namespace cubeshell
