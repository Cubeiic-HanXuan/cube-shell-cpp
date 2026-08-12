// SftpClient.cpp — libssh2-backed SFTP client. See SftpClient.h.

#include "SftpClient.h"
#include "SftpTransferPool.h"
#include "SshClient.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <utility>

Q_LOGGING_CATEGORY(sftpLog, "cubeshell.sftp")

namespace cubeshell {

// 元数据/小操作的分块大小 —— Python 在 download_with_resume /
// resume_upload 里用 32768(32KB)，保持一致以对齐进度粒度。
static constexpr int kChunkSize = 32768;

// 并行传输的分片大小。SFTP 是请求/应答协议，单流吞吐 ≈ 分片 / RTT，
// 32KB 在 30ms RTT 上只有约 1MB/s；传输走独立连接后不必再迁就进度粒度，
// 放大到 256KB 让单流本身就快一个量级，再叠加多流并行。
static constexpr qint64 kTransferChunkSize = 256 * 1024;

// 单个文件最多用几条并行流（与 SftpUploaderCore::kMaxStreamsPerFile 同量级）。
static constexpr int kMaxStreamsPerFile = 4;

// 小于此大小不拆流并行（多开连接的握手成本超过收益）。
static constexpr qint64 kMinSizeForMultiStream = 8 * 1024 * 1024;

// 析构时对每个存活传输 worker 的最长等待（cancelTransfer 后分块循环与
// EAGAIN 重试循环都会尽快退出，一般远小于此值）。
static constexpr int kWorkerJoinTimeoutMs = 5000;

bool SftpFileInfo::isDirectory() const { return LIBSSH2_SFTP_S_ISDIR(mode) != 0; }
bool SftpFileInfo::isSymlink() const { return LIBSSH2_SFTP_S_ISLNK(mode) != 0; }
bool SftpFileInfo::isRegular() const { return LIBSSH2_SFTP_S_ISREG(mode) != 0; }

SftpClient::SftpClient(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_transferPool(std::make_unique<SftpTransferPool>(client))
{
    qRegisterMetaType<cubeshell::SftpFileInfo>("cubeshell::SftpFileInfo");
    qRegisterMetaType<cubeshell::SftpFileInfoList>("cubeshell::SftpFileInfoList");
}

SftpClient::~SftpClient()
{
    // 取消协作：置 m_cancel 使 doDownload/doUpload 的分块循环与各同步操作的
    // EAGAIN 重试循环尽快退出，再有限等待自建传输线程结束，
    // 避免后台线程访问已删除的 this/m_sftp（与 SftpBrowserWidget 同款模式）。
    cancelTransfer();
    for (const QPointer<QThread> &worker : std::as_const(m_workers)) {
        if (worker) {
            worker->quit();
            // cancel 生效快（每个分块/重试迭代都检查），一般不会超时；
            // 超时兑底从简仅告警：m_sftp 是裸句柄非子 QObject，无父子关系可脱钩。
            if (!worker->wait(kWorkerJoinTimeoutMs))
                qCWarning(sftpLog) << "transfer worker did not finish within"
                                   << kWorkerJoinTimeoutMs << "ms";
        }
    }
    m_workers.clear();
    // 传输连接池必须在 close() 之前拆掉：它持有自己的克隆连接和 SFTP 通道，
    // 需要在 m_client 仍有效时正常 shutdown。abandon() 情形下 closeAll()
    // 已经先被调用过，这里只剩释放对象。
    m_transferPool.reset();
    close();
}

// Fill the last-error for the current SFTP subsystem. Falls back to the
// session error message. Must be called with sessionLock() held.
static void fillSftpError(_LIBSSH2_SFTP *sftp, SshClient *client, SshError &error,
                          const QString &context)
{
    unsigned long sftpErr = sftp ? libssh2_sftp_last_error(sftp) : 0;
    error.code = int(sftpErr);
    error.message = context;
    if (client && client->rawSession()) {
        char *msg = nullptr;
        int len = 0;
        libssh2_session_last_error(client->rawSession(), &msg, &len, 0);
        if (msg && len > 0)
            error.message += QStringLiteral(": ") + QString::fromLatin1(msg, len);
    }
}

// True when the most recent libssh2 call on this session would block (EAGAIN).
// After openShell() the session is non-blocking, so every SFTP call must retry
// on EAGAIN. Must be called with sessionLock() held.
static bool sftpWouldBlock(SshClient *client)
{
    return libssh2_session_last_errno(client->rawSession()) == LIBSSH2_ERROR_EAGAIN;
}

// Run an integer-returning libssh2 SFTP call, retrying on EAGAIN. When cancel
// is set (SftpClient::m_cancel via cancelTransfer) the retry loop aborts and
// the pending EAGAIN is returned; callers treat it as a failure and bail out.
// Must be called with sessionLock() held.
template <typename Fn>
static auto sftpRetryInt(SshClient *client, Fn &&fn,
                         const std::atomic<bool> *cancel = nullptr) -> decltype(fn())
{
    for (;;) {
        auto rc = fn();
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (cancel && cancel->load())
                return rc;
            client->waitReadable(5000);
            continue;
        }
        return rc;
    }
}

// Run a handle-returning libssh2 SFTP call, retrying while it returns NULL with
// the session in EAGAIN state; aborts (returning NULL) when cancel is set.
// Must be called with sessionLock() held.
template <typename Fn>
static auto sftpRetryPtr(SshClient *client, Fn &&fn,
                         const std::atomic<bool> *cancel = nullptr) -> decltype(fn())
{
    for (;;) {
        auto *h = fn();
        if (!h && sftpWouldBlock(client)) {
            if (cancel && cancel->load())
                return h;
            client->waitReadable(5000);
            continue;
        }
        return h;
    }
}

// 大文件传输流（downloadStream/uploadStream 的热循环）专用的调用纪律：
// 逐次 libssh2 调用加锁、EAGAIN 锁外等待。共享 session（主连接 / 主 session
// 通道槽）上持锁 select 空等会堵死终端读循环与 UI 线程的 writeChannel
// （jms/MFA 会话传输卡 UI 的根因）；锁一放，多条 SFTP 通道才能在一条 TCP
// 连接上真正交错。独占克隆连接（阻塞 session）上没有 EAGAIN，退化为
// 「拿锁一次调用」。cancel 置位时带 EAGAIN 哨兵返回（调用方当失败退出）。
// 与 sftpRetryInt 的区别仅在于 EAGAIN 等待发生在锁外 —— 上面的同步元数据
// 操作都是短调用，仍用 sftpRetry*；传输热循环必须用这个。
template <typename Fn>
static auto sftpStreamCall(QRecursiveMutex *lock, SshClient *client, Fn &&fn,
                           const std::atomic<bool> *cancel) -> decltype(fn())
{
    for (;;) {
        decltype(fn()) rc;
        {
            QMutexLocker<QRecursiveMutex> guard(lock);
            rc = fn();
        } // 锁在此释放 —— EAGAIN 等待发生在锁外
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (cancel && cancel->load())
                return rc;
            client->waitReadable(5000);
            continue;
        }
        return rc;
    }
}

bool SftpClient::open(SshError &error)
{
    if (m_sftp)
        return true;
    if (!m_client || !m_client->rawSession()) {
        error.message = QStringLiteral("SFTP: not connected");
        return false;
    }
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    _LIBSSH2_SFTP *sftp = sftpRetryPtr(m_client, [&] {
        return libssh2_sftp_init(m_client->rawSession());
    });
    if (!sftp) {
        fillSftpError(nullptr, m_client, error, QStringLiteral("libssh2_sftp_init failed"));
        return false;
    }
    m_sftp = sftp;
    return true;
}

bool SftpClient::ensureOpen(SshError &error)
{
    return m_sftp ? true : open(error);
}

void SftpClient::close()
{
    if (!m_sftp)
        return;
    if (m_abandoned) {
        // socket 已被关闭（标签页/应用退出）：不能再做任何 libssh2 网络调用，
        // 否则 libssh2_sftp_shutdown 往死 socket 写会崩溃。只清本地指针。
        m_sftp = nullptr;
        return;
    }
    if (m_client) {
        QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
        sftpRetryInt(m_client, [&] { return libssh2_sftp_shutdown(m_sftp); });
    }
    m_sftp = nullptr;
}

void SftpClient::abandon()
{
    // 标记后由 close()/析构走「只清指针」路径，避免在死 socket 上做网络往返。
    m_abandoned = true;
    if (m_transferPool) {
        // 主连接的 socket 已死 -> 只清指针；克隆连接各有自己的活 socket，
        // 先打断可能仍阻塞在传输里的线程，再按正常路径关掉
        //（否则会在服务端留下悬挂会话）。
        m_transferPool->abandonPrimary();
        m_transferPool->shutdownTransferSockets();
        m_transferPool->closeAll();
    }
}

// Convert libssh2 attributes to our info struct.
static void fillInfo(const QString &name, const QString &longname,
                     const LIBSSH2_SFTP_ATTRIBUTES &a, SftpFileInfo &info)
{
    info.filename = name;
    info.longname = longname;
    if (a.flags & LIBSSH2_SFTP_ATTR_SIZE)
        info.size = qint64(a.filesize);
    if (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        info.mode = a.permissions;
    if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
        info.mtime = qint64(a.mtime);
        info.atime = qint64(a.atime);
    }
    if (a.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info.uid = a.uid;
        info.gid = a.gid;
    }

    // 回退检测：部分 SFTP 服务器 readdir 返回的 attrs.permissions 不含 symlink
    // 类型位（返回了 stat() 跟随后的目标 mode 而非 lstat() 的 link mode）。
    // 此时从 longname 首字符 'l'（ls -l 格式的文件类型标识）来判断。
    // 对应Python: n[0][0] == 'l' 判断 symlink。
    //
    // longname 权限串 = 前 8 列的第 1 列（与 Python del_more_space 的 parts[0]
    // 对齐：re.split(r'\s+') 后前 8 列固定）。先按列切分，列数 <9 说明服务端
    // 返回了非标准/空 longname（部分服务端不生成 longname），不能用；列数 ≥9
    // 才可信。不能按"首个空格位置"猜——文件名字段不定长、且各列间可能有多空格，
    // 那样既会误认（含空格的名字把起点推偏）也会漏认（列间多空格把首个空格挤后）。
    QString permToken;
    if (!longname.isEmpty()) {
        const QStringList cols = longname.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (cols.size() >= 9)
            permToken = cols.first();
    }
    if (!info.isSymlink() && permToken.size() >= 10 &&
        permToken.at(0) == QLatin1Char('l')) {
        // longname 表明是 symlink，手动修正 mode 的类型位
        info.mode = (info.mode & ~quint32(LIBSSH2_SFTP_S_IFMT))
                  | quint32(LIBSSH2_SFTP_S_IFLNK);
    }

    // OpenSSH sftp-server appends " -> <target>" in longname for symlinks.
    // longname 形如：lrwxrwxrwx 1 root root 4 Jan 1 00:00 name -> target
    // 解析必须照搬 Python 版 del_more_space 的思路（function/ssh_func.py:479）：
    //   前 8 列固定（权限/链接数/属主/组/大小/月/日/时间），第 9 列(索引8)起
    //   的【整段】就是 "name -> target"。这样无论名字/目标含空格、含 "->"、
    //   或与前面某列同字，都不会被截断或定位错。
    // 旧实现用 longname.lastIndexOf(name) 猜名字位置——一旦目标路径里出现与
    // 文件名相同的子串、或名字被转义，起点就偏，导致 symlinkTarget 解析为空，
    // UI 只显示前半段名字、丢 "-> target"。
    if (info.isSymlink() && !longname.isEmpty()) {
        const QString simplified = longname.simplified();          // 连续空白→单空格
        const QStringList parts = simplified.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 9) {
            // 第 9 列起整段 = "name -> target"（保留内部所有空格与箭头）
            const QString nameAndTarget = parts.mid(8).join(QLatin1Char(' '));
            const int arrow = nameAndTarget.indexOf(QStringLiteral(" -> "));
            if (arrow >= 0)
                info.symlinkTarget = nameAndTarget.mid(arrow + 4).trimmed();
            // 若没有箭头（极少数服务端不附 target），symlinkTarget 留空，
            // UI 回退只显示 name——与 Python n[8] 行为一致。
        }
    }
}

QStringList SftpClient::listdir(const QString &path, SshError &error)
{
    QStringList names;
    const SftpFileInfoList entries = listdirAttr(path, error);
    if (!error.message.isEmpty() && entries.isEmpty())
        return names;
    for (const SftpFileInfo &e : entries)
        names << e.filename;
    return names;
}

SftpFileInfoList SftpClient::listdirAttr(const QString &path, SshError &error)
{
    SftpFileInfoList out;
    if (!ensureOpen(error))
        return out;

    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = path.toUtf8();

    _LIBSSH2_SFTP_HANDLE *dir = sftpRetryPtr(m_client, [&] {
        return libssh2_sftp_opendir(m_sftp, p.constData());
    }, &m_cancel);
    if (!dir) {
        fillSftpError(m_sftp, m_client, error, QStringLiteral("opendir failed: %1").arg(path));
        return out;
    }

    for (;;) {
        char nameBuf[512];
        char longBuf[4096];
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        int n = sftpRetryInt(m_client, [&] {
            return libssh2_sftp_readdir_ex(dir, nameBuf, sizeof(nameBuf),
                                           longBuf, sizeof(longBuf), &attrs);
        }, &m_cancel);
        if (n <= 0)
            break; // 0 == end of directory, <0 == error
        const QString name = QString::fromUtf8(nameBuf, n);
        if (name == QLatin1String(".") || name == QLatin1String(".."))
            continue;
        SftpFileInfo info;
        fillInfo(name, QString::fromUtf8(longBuf), attrs, info);
        out.append(info);
    }

    sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(dir); }, &m_cancel);
    return out;
}

static bool doStat(SshClient *client, _LIBSSH2_SFTP *sftp,
                   const QString &path, int statType, SftpFileInfo &info, SshError &error,
                   const std::atomic<bool> *cancel)
{
    QMutexLocker<QRecursiveMutex> lock(&client->sessionLock());
    const QByteArray p = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    int rc = sftpRetryInt(client, [&] {
        return libssh2_sftp_stat_ex(sftp, p.constData(), p.size(), statType, &attrs);
    }, cancel);
    if (rc != 0) {
        fillSftpError(sftp, client, error, QStringLiteral("stat failed: %1").arg(path));
        return false;
    }
    fillInfo(QFileInfo(path).fileName(), QString(), attrs, info);
    return true;
}

bool SftpClient::stat(const QString &path, SftpFileInfo &info, SshError &error)
{
    if (!ensureOpen(error))
        return false;
    return doStat(m_client, m_sftp, path, LIBSSH2_SFTP_STAT, info, error, &m_cancel);
}

bool SftpClient::lstat(const QString &path, SftpFileInfo &info, SshError &error)
{
    if (!ensureOpen(error))
        return false;
    return doStat(m_client, m_sftp, path, LIBSSH2_SFTP_LSTAT, info, error, &m_cancel);
}

bool SftpClient::exists(const QString &path)
{
    SftpFileInfo info;
    SshError error;
    return stat(path, info, error);
}

bool SftpClient::mkdir(const QString &path, int mode, SshError *error)
{
    SshError dummy;
    SshError &err = error ? *error : dummy;
    if (!ensureOpen(err))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = path.toUtf8();
    int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_mkdir_ex(m_sftp, p.constData(), p.size(), mode);
    });
    if (rc != 0) {
        fillSftpError(m_sftp, m_client, err, QStringLiteral("mkdir failed: %1").arg(path));
        return false;
    }
    return true;
}

bool SftpClient::mkdirP(const QString &path, int mode, SshError *error)
{
    // Recursive mkdir -p, mirroring sftp_uploader_core._mkdir_p.
    if (path.isEmpty() || path == QLatin1String("/"))
        return true;
    if (exists(path))
        return true;
    const QString parent = QFileInfo(path).path();
    if (!parent.isEmpty() && parent != path && parent != QLatin1String("."))
        mkdirP(parent, mode, error);
    return mkdir(path, mode, error);
}

bool SftpClient::rmdir(const QString &path, SshError *error)
{
    SshError dummy;
    SshError &err = error ? *error : dummy;
    if (!ensureOpen(err))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = path.toUtf8();
    int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_rmdir_ex(m_sftp, p.constData(), p.size());
    }, &m_cancel);
    if (rc != 0) {
        fillSftpError(m_sftp, m_client, err, QStringLiteral("rmdir failed: %1").arg(path));
        return false;
    }
    return true;
}

bool SftpClient::remove(const QString &path, SshError *error)
{
    SshError dummy;
    SshError &err = error ? *error : dummy;
    if (!ensureOpen(err))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = path.toUtf8();
    int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_unlink_ex(m_sftp, p.constData(), p.size());
    }, &m_cancel);
    if (rc != 0) {
        fillSftpError(m_sftp, m_client, err, QStringLiteral("remove failed: %1").arg(path));
        return false;
    }
    return true;
}

bool SftpClient::removeRecursive(const QString &path, SshError *error)
{
    // Mirrors util.deleteFolder: try to remove each child as a file; on failure
    // recurse into it as a directory; finally rmdir the (now empty) directory.
    SftpFileInfo info;
    SshError statErr;
    if (!lstat(path, info, statErr)) {
        if (error)
            *error = statErr;
        return false;
    }
    if (!info.isDirectory() || info.isSymlink())
        return remove(path, error);

    SshError listErr;
    const SftpFileInfoList children = listdirAttr(path, listErr);
    for (const SftpFileInfo &child : children) {
        const QString childPath = path + QLatin1Char('/') + child.filename;
        if (child.isDirectory() && !child.isSymlink())
            removeRecursive(childPath, error);
        else
            remove(childPath, nullptr);
    }
    return rmdir(path, error);
}

bool SftpClient::rename(const QString &oldPath, const QString &newPath, SshError *error)
{
    SshError dummy;
    SshError &err = error ? *error : dummy;
    if (!ensureOpen(err))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray op = oldPath.toUtf8();
    const QByteArray np = newPath.toUtf8();
    int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_rename_ex(m_sftp, op.constData(), op.size(),
                                      np.constData(), np.size(),
                                      LIBSSH2_SFTP_RENAME_OVERWRITE |
                                      LIBSSH2_SFTP_RENAME_ATOMIC |
                                      LIBSSH2_SFTP_RENAME_NATIVE);
    });
    if (rc != 0) {
        fillSftpError(m_sftp, m_client, err,
                      QStringLiteral("rename failed: %1 -> %2").arg(oldPath, newPath));
        return false;
    }
    return true;
}

bool SftpClient::chmod(const QString &path, int mode, SshError *error)
{
    SshError dummy;
    SshError &err = error ? *error : dummy;
    if (!ensureOpen(err))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = mode;
    int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_stat_ex(m_sftp, p.constData(), p.size(),
                                    LIBSSH2_SFTP_SETSTAT, &attrs);
    });
    if (rc != 0) {
        fillSftpError(m_sftp, m_client, err, QStringLiteral("chmod failed: %1").arg(path));
        return false;
    }
    return true;
}

QByteArray SftpClient::readFile(const QString &remotePath, SshError &error)
{
    QByteArray result;
    if (!ensureOpen(error))
        return result;

    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = remotePath.toUtf8();
    _LIBSSH2_SFTP_HANDLE *fh = sftpRetryPtr(m_client, [&] {
        return libssh2_sftp_open_ex(m_sftp, p.constData(), p.size(),
                                    LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    }, &m_cancel);
    if (!fh) {
        fillSftpError(m_sftp, m_client, error, QStringLiteral("open for read failed: %1").arg(remotePath));
        return result;
    }
    for (;;) {
        char buf[kChunkSize];
        ssize_t n = sftpRetryInt(m_client, [&] {
            return libssh2_sftp_read(fh, buf, sizeof(buf));
        }, &m_cancel);
        if (n <= 0)
            break;
        result.append(buf, int(n));
    }
    sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(fh); }, &m_cancel);
    return result;
}

bool SftpClient::writeFile(const QString &remotePath, const QByteArray &data, SshError &error)
{
    if (!ensureOpen(error))
        return false;
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = remotePath.toUtf8();
    _LIBSSH2_SFTP_HANDLE *fh = sftpRetryPtr(m_client, [&] {
        return libssh2_sftp_open_ex(m_sftp, p.constData(), p.size(),
                                    LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                    0644, LIBSSH2_SFTP_OPENFILE);
    }, &m_cancel);
    if (!fh) {
        fillSftpError(m_sftp, m_client, error, QStringLiteral("open for write failed: %1").arg(remotePath));
        return false;
    }
    ssize_t written = 0;
    bool ok = true;
    while (written < data.size()) {
        ssize_t n = sftpRetryInt(m_client, [&] {
            return libssh2_sftp_write(fh, data.constData() + written, size_t(data.size() - written));
        }, &m_cancel);
        if (n < 0) {
            fillSftpError(m_sftp, m_client, error, QStringLiteral("write failed: %1").arg(remotePath));
            ok = false;
            break;
        }
        written += n;
    }
    sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(fh); }, &m_cancel);
    return ok;
}

void SftpClient::fail(const QString &op, const QString &path, SshError &error)
{
    emit operationFailed(op, path, error.message);
}

// --- asynchronous transfers ------------------------------------------------

void SftpClient::cancelTransfer()
{
    m_cancel = true;
    // 传输走的是连接池的克隆连接，那是**阻塞模式** session：正卡在
    // libssh2_sftp_read/write 里的流不会回到我们的循环去看 m_cancel，只会挂到
    // TCP 超时。打断它们的 socket，让调用立刻带错误返回，取消才是即时的。
    if (m_transferPool)
        m_transferPool->shutdownTransferSockets();
}

// 登记并启动传输 worker 线程：finished 后 deleteLater，QPointer 随删除自动置空；
// 启动前顺手清理登记表中已置空的条目（与 SftpBrowserWidget::startWorker 同款）。
void SftpClient::startWorker(QThread *worker)
{
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_workers.removeAll(QPointer<QThread>());
    m_workers.append(QPointer<QThread>(worker));
    worker->start();
}

void SftpClient::download(const QString &remotePath, const QString &localPath)
{
    m_cancel = false;
    // Run the blocking transfer on a worker thread; report via queued signals.
    QThread *worker = QThread::create([this, remotePath, localPath] {
        doDownload(remotePath, localPath);
    });
    startWorker(worker);
}

void SftpClient::upload(const QString &localPath, const QString &remotePath)
{
    m_cancel = false;
    QThread *worker = QThread::create([this, localPath, remotePath] {
        doUpload(localPath, remotePath);
    });
    startWorker(worker);
}

// 一条并行传输流的共享状态（下载/上传通用）。工作窃取式分片分配：各条
// 连接实际速度可能差几倍，静态均分会被最慢的一条拖到底；游标领取让快的
// 流多干活，整体收敛到"最慢流只多干一个分片"的时间。
struct SftpClient::TransferState {
    QMutex lock;
    qint64 nextOffset = 0;   // 下一个待领分片的起点
    qint64 endOffset = 0;    // 传输区间末尾（不含）
    qint64 total = 0;        // 进度分母
    bool failed = false;
    QString error;
    std::atomic<qint64> done{0};

    // 领取下一个分片；返回 false 表示领完了（或已失败）。
    bool takeNext(qint64 &offsetOut, qint64 &sizeOut)
    {
        QMutexLocker locker(&lock);
        if (failed || nextOffset >= endOffset)
            return false;
        offsetOut = nextOffset;
        sizeOut = qMin(kTransferChunkSize, endOffset - nextOffset);
        nextOffset += sizeOut;
        return true;
    }

    void setFailed(const QString &message)
    {
        QMutexLocker locker(&lock);
        if (!failed) {
            failed = true;
            error = message;
        }
    }

    bool hasFailed()
    {
        QMutexLocker locker(&lock);
        return failed;
    }
};

// 决定一次传输开几条流。小文件不拆（握手成本盖过收益）；连接池拿不到
// 独立连接时恒为 1（此时退回主连接串行，与旧行为一致）。
int SftpClient::planStreamCount(qint64 size) const
{
    if (!m_transferPool || size < kMinSizeForMultiStream || !m_transferPool->canParallelize())
        return 1;
    const int byChunks = int(qMin<qint64>(kMaxStreamsPerFile,
                                          (size + kTransferChunkSize - 1) / kTransferChunkSize));
    return qBound(1, qMin(byChunks, m_transferPool->maxConnections()), kMaxStreamsPerFile);
}

// 跑 streams 条流并等它们全部结束。state 是调用方的栈对象，这里必须等干净。
void SftpClient::runStreams(int streams, const std::function<void()> &body)
{
    if (streams <= 1) {
        body();
        return;
    }
    QVector<QThread *> helpers;
    helpers.reserve(streams - 1);
    for (int i = 1; i < streams; ++i) {
        QThread *t = QThread::create(body);
        helpers.append(t);
        t->start();
    }
    body(); // 当前线程也干活，少开一个线程
    for (QThread *t : helpers) {
        t->wait();
        delete t;
    }
}

// 下载的一条流：租一条独立连接，开一次读 handle，循环领分片。
// 每条流各开一个本地 QFile 句柄写自己的区间（QFile 非线程安全不能共享；
// 区间互不重叠，并发 pwrite 安全）。
void SftpClient::downloadStream(TransferState &state, const QString &remotePath,
                                const QString &localPath)
{
    QString leaseErr;
    auto lease = m_transferPool->lease(&leaseErr, &m_cancel);
    if (!lease.isValid()) {
        if (m_cancel.load())
            return; // 已取消：等空闲槽被打断，不算错误
        state.setFailed(leaseErr.isEmpty() ? QStringLiteral("no SFTP connection available")
                                           : leaseErr);
        return;
    }

    QFile out(localPath);
    if (!out.open(QIODevice::ReadWrite)) {
        state.setFailed(QStringLiteral("cannot open local file: %1").arg(localPath));
        return;
    }

    const QByteArray p = remotePath.toUtf8();
    _LIBSSH2_SFTP_HANDLE *fh = nullptr;
    {
        QMutexLocker<QRecursiveMutex> guard(lease.lock());
        fh = sftpRetryPtr(lease.client(), [&] {
            return libssh2_sftp_open_ex(lease.sftp(), p.constData(), p.size(),
                                        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        }, &m_cancel);
    }
    if (!fh) {
        state.setFailed(QStringLiteral("open for read failed: %1").arg(remotePath));
        return;
    }

    QByteArray buf;
    buf.resize(int(kTransferChunkSize));
    qint64 offset = 0, size = 0;
    while (!m_cancel && state.takeNext(offset, size)) {
        // 一个分片可能要多次 read 才读满（服务端每次返回量有上限）。
        qint64 got = 0;
        bool ok = true;
        while (got < size && !m_cancel) {
            // seek+read 合成一次持锁调用：seek 只改 handle 本地 offset，read
            // 才可能 EAGAIN。fh 是本条流独占的，调用之间放锁不会被串改位置。
            const ssize_t n = sftpStreamCall(lease.lock(), lease.client(), [&] {
                libssh2_sftp_seek64(fh, libssh2_uint64_t(offset + got));
                return libssh2_sftp_read(fh, buf.data() + got, size_t(size - got));
            }, &m_cancel);
            if (n < 0) {
                state.setFailed(QStringLiteral("read failed at offset %1: %2")
                                    .arg(offset + got).arg(remotePath));
                ok = false;
                break;
            }
            if (n == 0)
                break; // 提前 EOF（远端文件在传输中被截短）
            got += n;
        }
        if (!ok)
            break;
        if (got > 0) {
            if (!out.seek(offset) || out.write(buf.constData(), got) != got) {
                state.setFailed(QStringLiteral("local write failed at offset %1").arg(offset));
                break;
            }
            state.done.fetch_add(got);
            emit transferProgress(remotePath, state.done.load(), state.total);
        }
        if (got < size)
            break; // EOF
    }
    out.flush();
    out.close();
    QMutexLocker<QRecursiveMutex> guard(lease.lock());
    sftpRetryInt(lease.client(), [&] { return libssh2_sftp_close_handle(fh); });
}

// 上传的一条流：与 downloadStream 对称。CREAT|WRITE 不带 TRUNC，
// 各流 seek 到自己的分片位置写（越过 EOF 的写由服务端补零）。
void SftpClient::uploadStream(TransferState &state, const QString &localPath,
                              const QString &remotePath)
{
    QString leaseErr;
    auto lease = m_transferPool->lease(&leaseErr, &m_cancel);
    if (!lease.isValid()) {
        if (m_cancel.load())
            return; // 已取消：等空闲槽被打断，不算错误
        state.setFailed(leaseErr.isEmpty() ? QStringLiteral("no SFTP connection available")
                                           : leaseErr);
        return;
    }

    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        state.setFailed(QStringLiteral("cannot open local file: %1").arg(localPath));
        return;
    }

    const QByteArray p = remotePath.toUtf8();
    _LIBSSH2_SFTP_HANDLE *fh = nullptr;
    {
        QMutexLocker<QRecursiveMutex> guard(lease.lock());
        fh = sftpRetryPtr(lease.client(), [&] {
            return libssh2_sftp_open_ex(lease.sftp(), p.constData(), p.size(),
                                        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                        0644, LIBSSH2_SFTP_OPENFILE);
        }, &m_cancel);
    }
    if (!fh) {
        state.setFailed(QStringLiteral("open for write failed: %1").arg(remotePath));
        return;
    }

    qint64 offset = 0, size = 0;
    while (!m_cancel && state.takeNext(offset, size)) {
        if (!in.seek(offset)) {
            state.setFailed(QStringLiteral("local read failed at offset %1").arg(offset));
            break;
        }
        const QByteArray chunk = in.read(size);
        if (chunk.isEmpty())
            break;

        bool ok = true;
        // seek 只改 handle 本地 offset（无网络 I/O）；fh 是本条流独占的，
        // 与后续写调用之间放锁不会被串改位置。
        {
            QMutexLocker<QRecursiveMutex> guard(lease.lock());
            libssh2_sftp_seek64(fh, libssh2_uint64_t(offset));
        }
        qint64 written = 0;
        while (written < chunk.size() && !m_cancel) {
            const ssize_t n = sftpStreamCall(lease.lock(), lease.client(), [&] {
                return libssh2_sftp_write(fh, chunk.constData() + written,
                                          size_t(chunk.size() - written));
            }, &m_cancel);
            if (n < 0) {
                state.setFailed(QStringLiteral("write failed at offset %1: %2")
                                    .arg(offset).arg(remotePath));
                ok = false;
                break;
            }
            written += n;
        }
        if (!ok)
            break;
        state.done.fetch_add(chunk.size());
        emit transferProgress(remotePath, state.done.load(), state.total);
    }

    QMutexLocker<QRecursiveMutex> guard(lease.lock());
    sftpRetryInt(lease.client(), [&] { return libssh2_sftp_close_handle(fh); });
}

// 对应Python: util.download_with_resume
//
// 相对旧实现的三处改动：
//  1. 不再把 sessionLock 锁在整个读循环外 —— 旧代码下载一个大文件期间会
//     全程霸占主 session，交互终端直接卡死到传完为止。现在传输走连接池的
//     独立连接，锁粒度收缩到单次 libssh2 调用。
//  2. 分片 32KB -> 256KB，单流吞吐直接上一个量级（SFTP 受 RTT 限制）。
//  3. 大文件按区间拆成多条流并行下载。
void SftpClient::doDownload(const QString &remotePath, const QString &localPath)
{
    SshError error;
    SftpFileInfo info;
    if (!stat(remotePath, info, error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }
    const qint64 remoteSize = info.size;

    qint64 localSize = 0;
    if (QFile::exists(localPath))
        localSize = QFileInfo(localPath).size();
    if (localSize >= remoteSize) {
        qCDebug(sftpLog) << "download: already complete" << remotePath;
        emit transferFinished(remotePath, true, QStringLiteral("already downloaded"));
        return;
    }

    if (!ensureOpen(error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }

    // 并行写要求本地文件先有完整长度（各流 seek 到自己的区间写）。
    {
        QFile out(localPath);
        if (!out.open(QIODevice::ReadWrite)) {
            emit transferFinished(remotePath, false,
                                  QStringLiteral("cannot open local file: %1").arg(localPath));
            return;
        }
        if (!out.resize(remoteSize)) {
            emit transferFinished(remotePath, false,
                                  QStringLiteral("cannot preallocate local file: %1").arg(localPath));
            return;
        }
    }

    TransferState state;
    state.nextOffset = localSize; // 断点续传：从本地已有长度接着下
    state.endOffset = remoteSize;
    state.total = remoteSize;
    state.done.store(localSize);

    const int streams = planStreamCount(remoteSize - localSize);
    qCDebug(sftpLog) << "download" << remotePath << "size" << remoteSize
                     << "streams" << streams;
    runStreams(streams, [this, &state, remotePath, localPath] {
        downloadStream(state, remotePath, localPath);
    });

    if (m_cancel) {
        emit transferFinished(remotePath, false, QStringLiteral("cancelled"));
    } else if (state.hasFailed()) {
        emit transferFinished(remotePath, false, state.error);
    } else {
        emit transferFinished(remotePath, true, QString());
    }
}

// 对应Python: util.resume_upload
// 并行化同 doDownload。注意断点续传语义的变化：旧实现用 FXF_APPEND 追加，
// 并行下多流 append 会互相覆盖，因此改成 seek 定位写（远端已有的前
// remoteSize 字节保持不动，效果一致）。
void SftpClient::doUpload(const QString &localPath, const QString &remotePath)
{
    SshError error;
    QFileInfo localInfo(localPath);
    if (!localInfo.exists()) {
        emit transferFinished(remotePath, false,
                              QStringLiteral("cannot open local file: %1").arg(localPath));
        return;
    }
    const qint64 fileSize = localInfo.size();

    // 远端已有多少（不存在则 0）——断点续传起点。
    qint64 remoteSize = 0;
    {
        SftpFileInfo info;
        SshError statErr;
        if (stat(remotePath, info, statErr))
            remoteSize = qMin(info.size, fileSize);
    }
    if (remoteSize >= fileSize) {
        emit transferFinished(remotePath, true, QStringLiteral("already uploaded"));
        return;
    }

    if (!ensureOpen(error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }

    TransferState state;
    state.nextOffset = remoteSize;
    state.endOffset = fileSize;
    state.total = fileSize;
    state.done.store(remoteSize);

    const int streams = planStreamCount(fileSize - remoteSize);
    qCDebug(sftpLog) << "upload" << remotePath << "size" << fileSize
                     << "streams" << streams;
    runStreams(streams, [this, &state, localPath, remotePath] {
        uploadStream(state, localPath, remotePath);
    });

    if (m_cancel) {
        emit transferFinished(remotePath, false, QStringLiteral("cancelled"));
    } else if (state.hasFailed()) {
        emit transferFinished(remotePath, false, state.error);
    } else {
        emit transferFinished(remotePath, true, QString());
    }
}

} // namespace cubeshell
