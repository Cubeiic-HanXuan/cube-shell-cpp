// SftpClient.cpp — libssh2-backed SFTP client. See SftpClient.h.

#include "SftpClient.h"
#include "SshClient.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <utility>

Q_LOGGING_CATEGORY(sftpLog, "cubeshell.sftp")

namespace cubeshell {

// Transfer chunk size — Python uses 32768 (32KB) in download_with_resume /
// resume_upload; keep the same so progress granularity matches.
static constexpr int kChunkSize = 32768;

// 析构时对每个存活传输 worker 的最长等待（cancelTransfer 后分块循环与
// EAGAIN 重试循环都会尽快退出，一般远小于此值）。
static constexpr int kWorkerJoinTimeoutMs = 5000;

bool SftpFileInfo::isDirectory() const { return LIBSSH2_SFTP_S_ISDIR(mode) != 0; }
bool SftpFileInfo::isSymlink() const { return LIBSSH2_SFTP_S_ISLNK(mode) != 0; }
bool SftpFileInfo::isRegular() const { return LIBSSH2_SFTP_S_ISREG(mode) != 0; }

SftpClient::SftpClient(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
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
    if (m_client) {
        QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
        sftpRetryInt(m_client, [&] { return libssh2_sftp_shutdown(m_sftp); });
    }
    m_sftp = nullptr;
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
        char longBuf[512];
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

void SftpClient::doDownload(const QString &remotePath, const QString &localPath)
{
    // Mirrors util.download_with_resume.
    SshError error;
    SftpFileInfo info;
    if (!stat(remotePath, info, error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }
    const qint64 remoteSize = info.size;

    qint64 localSize = 0;
    {
        QFile existing(localPath);
        if (existing.exists())
            localSize = QFileInfo(localPath).size();
    }
    if (localSize >= remoteSize) {
        qCDebug(sftpLog) << "download: already complete" << remotePath;
        emit transferFinished(remotePath, true, QStringLiteral("already downloaded"));
        return;
    }

    if (!ensureOpen(error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }

    {
        QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
        const QByteArray p = remotePath.toUtf8();
        _LIBSSH2_SFTP_HANDLE *fh = sftpRetryPtr(m_client, [&] {
            return libssh2_sftp_open_ex(m_sftp, p.constData(), p.size(),
                                        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        }, &m_cancel);
        if (!fh) {
            fillSftpError(m_sftp, m_client, error, QStringLiteral("open for read failed: %1").arg(remotePath));
            emit transferFinished(remotePath, false, error.message);
            return;
        }

        // Seek to the resume offset.
        if (localSize > 0)
            libssh2_sftp_seek64(fh, libssh2_uint64_t(localSize));

        QFile out(localPath);
        if (!out.open(QIODevice::Append)) {
            sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(fh); });
            emit transferFinished(remotePath, false,
                                  QStringLiteral("cannot open local file: %1").arg(localPath));
            return;
        }

        bool ok = true;
        while (!m_cancel) {
            char buf[kChunkSize];
            ssize_t n = sftpRetryInt(m_client, [&] {
                return libssh2_sftp_read(fh, buf, sizeof(buf));
            }, &m_cancel);
            if (n < 0) {
                fillSftpError(m_sftp, m_client, error, QStringLiteral("read failed: %1").arg(remotePath));
                ok = false;
                break;
            }
            if (n == 0)
                break; // EOF
            out.write(buf, n);
            localSize += n;
            emit transferProgress(remotePath, localSize, remoteSize);
        }
        out.flush();
        out.close();
        sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(fh); });

        if (m_cancel) {
            emit transferFinished(remotePath, false, QStringLiteral("cancelled"));
        } else {
            emit transferFinished(remotePath, ok, ok ? QString() : error.message);
        }
    }
}

void SftpClient::doUpload(const QString &localPath, const QString &remotePath)
{
    // Mirrors util.resume_upload.
    SshError error;
    QFile in(localPath);
    if (!in.open(QIODevice::ReadOnly)) {
        emit transferFinished(remotePath, false,
                              QStringLiteral("cannot open local file: %1").arg(localPath));
        return;
    }
    const qint64 fileSize = in.size();

    // Determine remote resume offset (0 when the remote file does not exist).
    qint64 remoteSize = 0;
    {
        SftpFileInfo info;
        SshError statErr;
        if (stat(remotePath, info, statErr))
            remoteSize = info.size;
    }

    if (!ensureOpen(error)) {
        emit transferFinished(remotePath, false, error.message);
        return;
    }

    {
        QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
        const QByteArray p = remotePath.toUtf8();
        const unsigned long flags = remoteSize > 0
            ? (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_APPEND)
            : (LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC);
        _LIBSSH2_SFTP_HANDLE *fh = sftpRetryPtr(m_client, [&] {
            return libssh2_sftp_open_ex(m_sftp, p.constData(), p.size(), flags,
                                        0644, LIBSSH2_SFTP_OPENFILE);
        }, &m_cancel);
        if (!fh) {
            fillSftpError(m_sftp, m_client, error, QStringLiteral("open for write failed: %1").arg(remotePath));
            emit transferFinished(remotePath, false, error.message);
            return;
        }

        in.seek(remoteSize);
        bool ok = true;
        while (!m_cancel) {
            const QByteArray chunk = in.read(kChunkSize);
            if (chunk.isEmpty())
                break;
            ssize_t written = 0;
            while (written < chunk.size()) {
                ssize_t n = sftpRetryInt(m_client, [&] {
                    return libssh2_sftp_write(fh, chunk.constData() + written,
                                              size_t(chunk.size() - written));
                }, &m_cancel);
                if (n < 0) {
                    fillSftpError(m_sftp, m_client, error, QStringLiteral("write failed: %1").arg(remotePath));
                    ok = false;
                    break;
                }
                written += n;
            }
            if (!ok)
                break;
            remoteSize += chunk.size();
            emit transferProgress(remotePath, remoteSize, fileSize);
        }
        sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(fh); });

        if (m_cancel) {
            emit transferFinished(remotePath, false, QStringLiteral("cancelled"));
        } else {
            emit transferFinished(remotePath, ok, ok ? QString() : error.message);
        }
    }
}

} // namespace cubeshell
