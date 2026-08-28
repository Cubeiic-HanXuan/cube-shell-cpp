// SftpTransferPool.cpp — 并行 SFTP 传输连接池。见 SftpTransferPool.h。

#include "SftpTransferPool.h"
#include "SshClient.h"

#include <QLoggingCategory>
#include <QMutexLocker>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

Q_LOGGING_CATEGORY(sftpPoolLog, "cubeshell.sftp.pool")

namespace cubeshell {

namespace {

// 关闭 SFTP 通道时的有界重试：对端正常时头一两次就成功；socket 已断时全部
// 返回 EAGAIN，最多花 retries * retryMs 便放弃。
constexpr int kCloseChannelRetries = 20;
constexpr int kCloseChannelRetryMs = 10;

// lease() 等空闲槽的唤醒间隔：每过这么长就醒一次，重查取消标志并重扫一遍槽
// （防止错过唤醒，也让取消能在有界时间内被看到）。
constexpr int kLeaseWaitSliceMs = 500;

// 析构路径上抢锁（sessionLock / 槽锁）的最长等待。
//
// 抢这把锁只是为了"优雅关闭"SFTP 通道，而优雅关闭本身是可选的：通道内存随
// SshClient 析构时的 libssh2_session_free 一并回收，跳过它只是少一次协议层
// shutdown 往返，不泄漏任何东西。收益这么小，就不值得为它堵住 UI 线程 ——
// 关闭标签页时终端读循环/远程监控极可能正持有 sessionLock 并阻塞在网络 IO 上
// （SshClient.cpp 的 kCloseLockTimeoutMs 注释记录了同款 gdb 现场），那种情形
// 下等多久都是白等，等满超时纯粹变成用户看得见的卡顿：实测上传中关标签页
// （终端读循环占着锁）时，这里独占了析构 1.6 秒里的 1.5 秒。
//
// 因此不与 SshClient::kCloseLockTimeoutMs(1500) 对齐，改为与
// closeSftpChannelBounded 拿到锁之后的总预算（kCloseChannelRetries *
// kCloseChannelRetryMs = 200ms）对齐：等锁的时间不该超过拿到锁要干的活。
constexpr int kCloseLockTimeoutMs = kCloseChannelRetries * kCloseChannelRetryMs;

// 在指定连接上开一个 SFTP 子系统通道。克隆连接是阻塞式 session（没调过
// openShell，connectToHost 里 set_blocking(1) 一直有效），不会返回 EAGAIN；
// 主连接是非阻塞的，需要 EAGAIN 重试。两种都用这一个函数处理。
_LIBSSH2_SFTP *openSftpChannel(SshClient *client)
{
    if (!client || !client->rawSession())
        return nullptr;
    for (;;) {
        _LIBSSH2_SFTP *sftp = libssh2_sftp_init(client->rawSession());
        if (sftp)
            return sftp;
        if (libssh2_session_last_errno(client->rawSession()) != LIBSSH2_ERROR_EAGAIN)
            return nullptr;
        client->waitReadable(5000);
    }
}

// 关闭一条连接上的 SFTP 通道，保证有界返回。
//
// 绝不能在阻塞模式下直接 libssh2_sftp_shutdown：它内部要等服务端回
// SSH_MSG_CHANNEL_CLOSE，而克隆连接的 session 是阻塞模式（只 connectToHost，
// 没走 openShell 的 set_blocking(0)），libssh2 默认超时又是"永不超时"——
// socket 已被 shutdownTransferSockets 打断时 select() 永远不返回。
// macOS 上 sample 实测：主线程卡在
//   ~SftpTransferPool → closeAll → libssh2_sftp_shutdown
//     → _libssh2_wait_socket → __select
// 这正是上传中关闭标签页卡死、退出程序卡在程序坞的原因。
//
// 处理方式与 SshClient::closeChannelLocked 一致：切非阻塞 + 有界重试，
// 拿到优雅关闭最好，拿不到就放弃——通道内存随 libssh2_session_free 一起回收
// （disconnectFromHost 负责），本地释放本就不依赖对端确认。
void closeSftpChannelBounded(SshClient *client, _LIBSSH2_SFTP *sftp)
{
    if (!sftp || !client)
        return;
    LIBSSH2_SESSION *session = client->rawSession();
    if (!session)
        return; // session 已释放，通道随之失效，不能再碰
    libssh2_session_set_blocking(session, 0);
    for (int i = 0; i < kCloseChannelRetries; ++i) {
        if (libssh2_sftp_shutdown(sftp) != LIBSSH2_ERROR_EAGAIN)
            return;
        QThread::msleep(kCloseChannelRetryMs);
    }
    qCDebug(sftpPoolLog) << "sftp channel close gave up after"
                         << kCloseChannelRetries * kCloseChannelRetryMs << "ms; "
                            "leaving it to session_free";
}

} // namespace

SftpTransferPool::Lease &SftpTransferPool::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        release();
        m_pool = other.m_pool;
        m_slot = other.m_slot;
        m_conn = other.m_conn;
        other.m_pool = nullptr;
        other.m_slot = -1;
        other.m_conn = Connection{};
    }
    return *this;
}

void SftpTransferPool::Lease::release()
{
    if (m_pool && m_slot >= 0)
        m_pool->releaseSlot(m_slot);
    m_pool = nullptr;
    m_slot = -1;
    m_conn = Connection{};
}

SftpTransferPool::SftpTransferPool(SshClient *primary)
    : m_primary(primary)
{
    // [0] 永远是主连接槽（不拥有 client，锁用主连接的 sessionLock）。
    auto *primarySlot = new Slot();
    primarySlot->isPrimary = true;
    m_slots.append(primarySlot);
}

SftpTransferPool::~SftpTransferPool()
{
    // 预热任务会往 m_slots 里塞连接，必须先等它们跑完再拆池，
    // 否则它们会摸到正在释放的成员。
    m_prewarmPool.waitForDone();
    // 先打断阻塞中的传输线程，再关连接。调用方（SftpClient/SftpUploaderCore
    // 的析构）应当已经 join 过传输线程；这里是兜底，避免超时逃逸的线程
    // 继续在正被释放的连接上做 I/O。
    shutdownTransferSockets();
    closeAll();
    QMutexLocker locker(&m_lock);
    // 主连接上本池自己开的 SFTP 通道：主 session 仍可能被终端使用，
    // 必须持 sessionLock 才能安全 shutdown。socket 已死时只清指针
    // （abandonPrimary 已置位），否则往死 socket 写会崩溃。
    //
    // 锁用 tryLock 而非无限等：关闭标签页时终端读循环/远程监控极可能正持有
    // sessionLock 并阻塞在网络 IO 上，无限等会把 UI 线程钉死在 futex 上
    // （SshClient.cpp 的 kCloseLockTimeoutMs 注释记录了同款 gdb 现场）。
    // 抢不到就放弃优雅关闭，通道随 session_free 回收。
    if (m_primarySftp && !m_primaryAbandoned && m_primary && m_primary->rawSession()) {
        if (m_primary->sessionLock().tryLock(kCloseLockTimeoutMs)) {
            closeSftpChannelBounded(m_primary, m_primarySftp);
            m_primary->sessionLock().unlock();
        } else {
            qCWarning(sftpPoolLog) << "sessionLock busy on teardown; skipping graceful "
                                      "shutdown of primary SFTP channel";
        }
    }
    m_primarySftp = nullptr;
    qDeleteAll(m_slots);
    m_slots.clear();
}

void SftpTransferPool::setPrimary(SshClient *primary)
{
    m_prewarmPool.waitForDone();  // 别让预热任务往旧凭据上继续建连接
    shutdownTransferSockets();    // 打断可能仍在传输的线程
    closeAll();                   // 换连接：旧的克隆连接全部作废
    QMutexLocker locker(&m_lock);
    m_primary = primary;
    m_primaryAbandoned = false; // 新连接是活的
    m_primarySftp = nullptr; // 主连接换了，SFTP 通道要重开
}

void SftpTransferPool::setMaxConnections(int n)
{
    QMutexLocker locker(&m_lock);
    m_maxConnections = qBound(1, n, 16);
}

int SftpTransferPool::maxConnections() const
{
    QMutexLocker locker(&m_lock);
    return m_maxConnections;
}

int SftpTransferPool::activeConnections() const
{
    QMutexLocker locker(&m_lock);
    int n = 0;
    for (const Slot *slot : m_slots) {
        if (slot->isPrimary || slot->sftp)
            ++n;
    }
    return qMax(1, n);
}

void SftpTransferPool::setCloningEnabled(bool enabled)
{
    QMutexLocker locker(&m_lock);
    m_cloningEnabled = enabled;
}

bool SftpTransferPool::cloningEnabled() const
{
    QMutexLocker locker(&m_lock);
    return m_cloningEnabled;
}

bool SftpTransferPool::canParallelize() const
{
    QMutexLocker locker(&m_lock);
    if (m_maxConnections <= 1 || !m_primary || !m_primary->rawSession()
        || m_primaryAbandoned)
        return false;
    // 凭据可重放 → 克隆独立连接并行；不可重放（jms 一次性 token / MFA）→
    // 主 session 通道多路复用并行。两条路只要一条通就算可并行。
    const bool canClone = m_cloningEnabled && m_primary->isAuthReplayable()
        && !m_cloneExhausted;
    return canClone || !m_channelExhausted;
}

// 主连接槽的连接信息。主 session 与终端 shell 共享，锁是 sessionLock。
SftpTransferPool::Connection SftpTransferPool::primaryConnection(QString *errorOut)
{
    // 调用方已持 m_lock。
    if (!m_primary || !m_primary->rawSession() || m_primaryAbandoned) {
        if (errorOut)
            *errorOut = QStringLiteral("SFTP客户端未设置");
        return Connection{};
    }
    if (!m_primarySftp) {
        QMutexLocker<QRecursiveMutex> lock(&m_primary->sessionLock());
        m_primarySftp = openSftpChannel(m_primary);
        if (!m_primarySftp) {
            if (errorOut)
                *errorOut = QStringLiteral("libssh2_sftp_init failed");
            return Connection{};
        }
    }
    Connection conn;
    conn.client = m_primary;
    conn.sftp = m_primarySftp;
    conn.lock = &m_primary->sessionLock();
    conn.dedicated = false;
    return conn;
}

// 克隆一条传输专用连接。凭据全部取自主连接；不调 openShell，所以 session
// 保持阻塞模式（connectToHost 里 set_blocking(1)），SFTP 调用不会 EAGAIN。
bool SftpTransferPool::spawnClone(Slot &slot, QString *errorOut)
{
    // 调用方已持 m_lock；握手/认证会阻塞若干百毫秒，这里先取出凭据再解锁，
    // 避免长时间占着池锁把其它工作线程的 lease() 全堵住。
    SshClient *primary = m_primary;
    if (!primary)
        return false;
    const QString host = primary->host();
    const quint16 port = primary->port();
    const QString user = primary->username();
    const QString pass = primary->password();
    const QString keyType = primary->keyType();
    const QString keyFile = primary->keyFile();
    const QString passphrase = primary->passphrase();
    // 代理配置也算凭据的一部分，必须一起复制：漏了的话克隆连接会绕过代理直连，
    // 在内网里就是"主连接好着、并行传输却连不上"这种极难定位的现象。
    const ProxyConfig proxy = primary->proxyConfig();
    const ProxyConfig globalProxy = primary->globalProxyConfig();
    const int connectTimeoutMs = primary->connectTimeoutMs();

    auto client = std::make_unique<SshClient>();
    client->setHost(host, port);
    client->setUsername(user);
    // 从主连接复制而不是各自去读配置：主连接建立时用的是哪份代理，克隆就该用
    // 哪份。（唯一例外是"全局代理"且主连接当初也没显式给出全局那份的情况——
    // 那时两边都会各自去设置里取，见 SshClient::connectToHost。）
    // 超时同理：这里复制到的 0 表示"主连接也没显式设"，克隆照样去设置里取。
    client->setProxyConfig(proxy, globalProxy);
    client->setConnectTimeoutMs(connectTimeoutMs);
    if (!keyFile.isEmpty())
        client->setPrivateKey(keyType, keyFile, passphrase);
    else
        client->setPassword(pass);

    SshError err;
    bool ok = false;
    {
        // 解锁做网络 I/O。m_slots 里这个槽已被标记 inUse，不会被别人抢。
        m_lock.unlock();
        // promptCallback 传 null：克隆连接绝不弹 MFA 对话框（canParallelize()
        // 已经排除了 keyboard-interactive 的主连接，这里是双保险）。
        ok = client->connectToHost(nullptr, err);
        m_lock.lock();
    }
    if (!ok) {
        qCInfo(sftpPoolLog) << "clone connection failed, falling back to serial:" << err.message;
        m_cloneExhausted = true; // 大概率是 MaxSessions/限流，别再重试
        if (errorOut)
            *errorOut = err.message;
        return false;
    }

    _LIBSSH2_SFTP *sftp = openSftpChannel(client.get());
    if (!sftp) {
        qCInfo(sftpPoolLog) << "clone sftp_init failed, falling back to serial";
        m_cloneExhausted = true;
        client->disconnectFromHost();
        if (errorOut)
            *errorOut = QStringLiteral("libssh2_sftp_init failed on transfer connection");
        return false;
    }

    slot.client = std::move(client);
    slot.sftp = sftp;
    slot.lock = std::make_unique<QRecursiveMutex>();
    qCDebug(sftpPoolLog) << "transfer connection established, pool size now"
                         << m_slots.size();
    return true;
}

// 在主 session 上多路复用一条独立 SFTP 通道（通道槽）。
//
// 凭据不可重放（jms 一次性 token / keyboard-interactive MFA）时克隆不了连接，
// 但 SSH 协议允许一条连接上复用多条 channel —— 开新通道不需要重新认证，
// 一次性/动态码凭据的限制天然绕开。通道与 shell、与其它 SFTP 通道并存；
// 所有 channel 共享 sessionLock，由调用方按逐调用锁纪律交错（见头文件注释）。
bool SftpTransferPool::spawnPrimaryChannel(Slot &slot)
{
    // 调用方已持 m_lock；槽已 append 且占住 inUse，sftp 为空时不会被租走。
    SshClient *primary = m_primary;
    if (!primary || !primary->rawSession() || m_primaryAbandoned)
        return false;

    _LIBSSH2_SFTP *sftp = nullptr;
    {
        // openSftpChannel 内有网络往返（EAGAIN 时 waitReadable），先放池锁，
        // 避免堵住其它线程的 lease()。锁序与全类一致：m_lock → sessionLock。
        m_lock.unlock();
        {
            QMutexLocker<QRecursiveMutex> slock(&primary->sessionLock());
            sftp = openSftpChannel(primary);
        }
        m_lock.lock();
    }
    if (!sftp) {
        qCInfo(sftpPoolLog) << "primary-channel spawn failed (MaxSessions?), "
                               "falling back to shared channel";
        m_channelExhausted = true; // 别再每次 lease 都白试一轮
        return false;
    }

    slot.isPrimaryChannel = true;
    slot.sftp = sftp;
    qCDebug(sftpPoolLog) << "multiplexed SFTP channel on primary session, "
                            "pool size now" << m_slots.size();
    return true;
}

SftpTransferPool::Lease SftpTransferPool::lease(QString *errorOut,
                                                const std::atomic<bool> *cancel)
{
    QMutexLocker locker(&m_lock);

    for (;;) {
        const bool primaryUsable = m_primary && m_primary->rawSession()
            && !m_primaryAbandoned;

        // 1) 已建成且空闲的克隆连接优先（零握手成本，独立 TCP 最优）。
        for (int i = 0; i < m_slots.size(); ++i) {
            Slot *slot = m_slots[i];
            if (slot->isPrimary || slot->isPrimaryChannel || slot->inUse || !slot->sftp)
                continue;
            // 连接可能在空闲期间被服务端断开，用则先验。
            if (!slot->client || !slot->client->isTransportAlive()) {
                if (slot->sftp) {
                    closeSftpChannelBounded(slot->client.get(), slot->sftp);
                    slot->sftp = nullptr;
                }
                if (slot->client)
                    slot->client->disconnectFromHost();
                slot->client.reset();
                slot->lock.reset();
                continue;
            }
            slot->inUse = true;
            Connection conn;
            conn.client = slot->client.get();
            conn.sftp = slot->sftp;
            conn.lock = slot->lock.get();
            conn.dedicated = true;
            return Lease(this, i, conn);
        }

        // 2) 空闲的主 session 通道槽（多路复用，无需重认证）。
        if (primaryUsable) {
            for (int i = 0; i < m_slots.size(); ++i) {
                Slot *slot = m_slots[i];
                if (!slot->isPrimaryChannel || slot->inUse || !slot->sftp)
                    continue;
                slot->inUse = true;
                Connection conn;
                conn.client = m_primary;
                conn.sftp = slot->sftp;
                conn.lock = &m_primary->sessionLock();
                conn.dedicated = false;
                return Lease(this, i, conn);
            }
        }

        // 3) 未达上限 -> 新建：克隆连接优先，主 session 通道兜底。
        //    优先回收步骤 1 清理出来的空槽（死连接只清空不摘除——摘除会让
        //    在野租约持有的槽位下标错位）。否则取消传输把全部克隆 socket
        //    打断后，池子会被空槽占满、再也建不出新克隆。
        const bool cloneAllowed = primaryUsable && m_cloningEnabled
            && m_primary->isAuthReplayable()
            && m_maxConnections > 1 && !m_cloneExhausted;
        const bool channelAllowed = primaryUsable && m_maxConnections > 1
            && !m_channelExhausted;
        Slot *emptySlot = nullptr;
        for (Slot *s : m_slots) {
            if (!s->isPrimary && !s->isPrimaryChannel && !s->inUse
                && !s->sftp && !s->client) {
                emptySlot = s;
                break;
            }
        }
        if ((cloneAllowed || channelAllowed)
            && (emptySlot || m_slots.size() < m_maxConnections)) {
            Slot *slot = emptySlot;
            int index = -1;
            if (slot) {
                slot->inUse = true; // 先占住，spawn* 会短暂解锁池锁
                index = int(m_slots.indexOf(slot));
            } else {
                slot = new Slot();
                slot->inUse = true; // 先占住，spawn* 会短暂解锁池锁
                m_slots.append(slot);
                index = m_slots.size() - 1;
            }
            bool ok = false;
            if (cloneAllowed)
                ok = spawnClone(*slot, nullptr);
            // 克隆被拒（一次性 token / MaxSessions）→ 落到主 session 通道。
            if (!ok && channelAllowed)
                ok = spawnPrimaryChannel(*slot);
            if (ok) {
                Connection conn;
                if (slot->isPrimaryChannel) {
                    conn.client = m_primary;
                    conn.sftp = slot->sftp;
                    conn.lock = &m_primary->sessionLock();
                    conn.dedicated = false;
                } else {
                    conn.client = slot->client.get();
                    conn.sftp = slot->sftp;
                    conn.lock = slot->lock.get();
                    conn.dedicated = true;
                }
                return Lease(this, index, conn);
            }
            // 建不起来：撤掉这个槽，落到下面的等待/共享主通道。注意 spawn* 期间
            // 解过锁，槽位下标可能已被其它线程的 append 挤动，按指针精确移除。
            // 回收槽不 delete（它是既有空槽）：只还原占用标志。
            const int actual = m_slots.indexOf(slot);
            if (slot == emptySlot) {
                if (actual >= 0)
                    slot->inUse = false;
            } else {
                if (actual >= 0)
                    m_slots.remove(actual);
                delete slot;
            }
        }

        // 4) 共享已建成的克隆连接（多于连接数的任务在此排队）。克隆连接是阻塞
        //    session：每次 libssh2 调用在槽锁内原子完成、不会有 EAGAIN 半途态，
        //    两个线程共享同一条通道也不会把 request_id 配对打乱 —— 共享是安全的。
        for (int i = 0; i < m_slots.size(); ++i) {
            Slot *slot = m_slots[i];
            if (slot->isPrimary || slot->isPrimaryChannel || !slot->sftp)
                continue;
            // 被 shutdownTransferSockets 打断的连接（取消传输后）不能共享出租，
            // 租出去 open 必失败——留给步骤 1 清理、步骤 3 回收。
            if (!slot->client || !slot->client->isTransportAlive())
                continue;
            Connection conn;
            conn.client = slot->client.get();
            conn.sftp = slot->sftp;
            conn.lock = slot->lock.get();
            conn.dedicated = true;
            // 不置 inUse：这是共享复用，归还时也不清标志（见 releaseSlot）。
            return Lease(this, -1, conn);
        }

        // 走到这里说明没有可共享的克隆，只能走主 session 的通道。主 session 是
        // 非阻塞的，逐调用锁纪律下两个线程若共享同一条 SFTP 通道，EAGAIN 半途态
        // 会把 libssh2 的 request_id 配对打乱而挂死（实测如此）。所以主 session
        // 的通道一律独占，绝不共享：租不到就等空闲槽。

        // 5) 主连接自己的共享 SFTP 通道（m_primarySftp）作为最后一条可独占的
        //    通道：借用主连接槽 [0] 的 inUse 标志做互斥，一次只租给一条流。
        if (primaryUsable && !m_slots.isEmpty() && !m_slots[0]->inUse) {
            Connection conn = primaryConnection(errorOut);
            if (!conn.isValid())
                return Lease(); // 主连接通道也开不出来，errorOut 已填
            m_slots[0]->inUse = true;
            return Lease(this, 0, conn);
        }

        // 6) 所有独占通道都忙：可取消的有限等待，醒了重扫。持锁等
        //    m_slotFreed（wait 期间放锁），releaseSlot() 归还槽时唤醒。
        if (cancel && cancel->load())
            return Lease();
        if (!primaryUsable) {
            // 主连接不可用且没有可共享的克隆：无计可施，别死等。
            if (errorOut)
                *errorOut = QStringLiteral("SFTP客户端未设置");
            return Lease();
        }
        m_slotFreed.wait(&m_lock, kLeaseWaitSliceMs);
    }
}

void SftpTransferPool::releaseSlot(int slot)
{
    if (slot < 0)
        return; // 共享克隆复用租约：无独占标志可清
    QMutexLocker locker(&m_lock);
    if (slot < m_slots.size())
        m_slots[slot]->inUse = false;
    m_slotFreed.wakeAll(); // 叫醒 lease() 里等空闲槽的线程
}

// 后台补足空闲连接/通道。每个任务建一条：spawn* 内部会短暂释放池锁做网络
// I/O，所以多个预热任务可以真正并行握手（4 条连接的预热 ≈ 1 条的耗时）。
// 凭据不可克隆时改为预建主 session 的 SFTP 通道（jms 一次性 token / MFA）。
void SftpTransferPool::prewarm(int count)
{
    int toSpawn = 0;
    {
        QMutexLocker locker(&m_lock);
        const bool primaryUsable = m_primary && m_primary->rawSession()
            && !m_primaryAbandoned;
        const bool canClone = primaryUsable && m_cloningEnabled
            && m_primary->isAuthReplayable() && !m_cloneExhausted;
        const bool canChannel = primaryUsable && !m_channelExhausted;
        if (m_maxConnections <= 1 || (!canClone && !canChannel))
            return; // 既不能克隆也不能开通道：预热没有意义
        const int target = count > 0 ? qMin(count, m_maxConnections) : m_maxConnections;
        // m_slots 含主连接槽，传输槽数 = size() - 1。
        toSpawn = target - m_slots.size();
    }
    if (toSpawn <= 0)
        return;

    m_prewarmPool.setMaxThreadCount(qMax(1, toSpawn));
    for (int i = 0; i < toSpawn; ++i) {
        m_prewarmPool.start([this] {
            Slot *slot = nullptr;
            {
                QMutexLocker locker(&m_lock);
                if (m_slots.size() >= m_maxConnections)
                    return;
                const bool canClone = m_primary && m_primary->rawSession()
                    && !m_primaryAbandoned && m_cloningEnabled
                    && m_primary->isAuthReplayable() && !m_cloneExhausted;
                const bool canChannel = m_primary && m_primary->rawSession()
                    && !m_primaryAbandoned && !m_channelExhausted;
                if (!canClone && !canChannel)
                    return;
                slot = new Slot();
                // 不置 inUse：预热建好的连接/通道应当是空闲可租的。
                m_slots.append(slot);
                bool ok = false;
                if (canClone)
                    ok = spawnClone(*slot, nullptr);
                if (!ok && canChannel)
                    ok = spawnPrimaryChannel(*slot);
                if (!ok) {
                    const int actual = m_slots.indexOf(slot);
                    if (actual >= 0)
                        m_slots.remove(actual);
                    delete slot;
                }
            }
        });
    }
}

bool SftpTransferPool::waitPrewarmDone(int timeoutMs)
{
    return m_prewarmPool.waitForDone(timeoutMs);
}

void SftpTransferPool::shutdownTransferSockets()
{
    QMutexLocker locker(&m_lock);
    for (Slot *slot : m_slots) {
        // 只打断池自己拥有的克隆连接。通道槽（!slot->client）与主连接槽共享
        // 主连接的 socket —— shutdown 它会误杀终端会话；且主 session 是非阻塞
        // 模式，通道槽上的传输线程走 EAGAIN 循环看取消标志即可退出，无需打断。
        if (slot->isPrimary || !slot->client)
            continue;
        // 不取槽锁：正阻塞在 libssh2 调用里的线程正握着它，取锁会死等。
        // shutdownSocket() 只做 ::shutdown(fd)，不触碰 libssh2 状态，可并发调用。
        slot->client->shutdownSocket();
    }
}

void SftpTransferPool::abandonPrimary()
{
    QMutexLocker locker(&m_lock);
    m_primaryAbandoned = true;
    m_primarySftp = nullptr; // 只丢指针，绝不在死 socket 上做 sftp_shutdown
    m_slotFreed.wakeAll();   // 让等槽的线程重扫，按 !primaryUsable 退出
}

void SftpTransferPool::closeAll()
{
    QMutexLocker locker(&m_lock);
    for (Slot *slot : m_slots) {
        if (slot->isPrimary)
            continue;
        if (slot->isPrimaryChannel) {
            // 通道槽：只关自己在主 session 上的 SFTP 通道，绝不动主连接本身。
            // 主连接已死（abandon）时只清指针，不做网络往返（往死 socket 写会崩）。
            if (slot->sftp && !m_primaryAbandoned && m_primary
                && m_primary->rawSession()) {
                // 与克隆槽同理用 tryLock：万一还有线程卡在主 session 的
                // libssh2 调用里，无限等锁就等于把 UI 线程一起赔进去。
                if (m_primary->sessionLock().tryLock(kCloseLockTimeoutMs)) {
                    closeSftpChannelBounded(m_primary, slot->sftp);
                    m_primary->sessionLock().unlock();
                } else {
                    qCWarning(sftpPoolLog) << "sessionLock busy on teardown; "
                                              "skipping graceful shutdown of "
                                              "multiplexed SFTP channel";
                }
            }
            slot->sftp = nullptr;
            slot->inUse = false;
            continue;
        }
        // 持槽锁再关：正常路径下调用方已停掉所有传输线程（无未归还租约），
        // 但共享复用租约（lease 第 3 步）不置 inUse，拿锁是廉价的双保险。
        // 同样用 tryLock：万一还有线程卡在这条连接的 libssh2 调用里，
        // 无限等锁就等于把 UI 线程一起赔进去。
        if (slot->lock) {
            if (slot->lock->tryLock(kCloseLockTimeoutMs)) {
                if (slot->sftp)
                    closeSftpChannelBounded(slot->client.get(), slot->sftp);
                slot->lock->unlock();
            } else {
                qCWarning(sftpPoolLog) << "transfer slot lock busy on teardown; "
                                          "skipping graceful SFTP channel shutdown";
            }
        }
        slot->sftp = nullptr;
        if (slot->client)
            slot->client->disconnectFromHost();
        slot->client.reset();
        slot->lock.reset();
        slot->inUse = false;
    }
    // 只留主连接槽。
    for (int i = m_slots.size() - 1; i >= 1; --i) {
        delete m_slots[i];
        m_slots.remove(i);
    }
    if (!m_slots.isEmpty())
        m_slots[0]->inUse = false; // 共享主通道槽复位（前置条件：无未归还租约）
    m_cloneExhausted = false;   // 重连后允许重新尝试克隆
    m_channelExhausted = false; // 重连后允许重新尝试通道
    m_slotFreed.wakeAll();      // 状态变了，让等槽的线程重扫（并按取消/无效退出）
}

} // namespace cubeshell
