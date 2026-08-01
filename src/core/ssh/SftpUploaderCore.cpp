// SftpUploaderCore.cpp — SFTP 分片上传核心。见 SftpUploaderCore.h 的
// 并发模型与元数据兼容性说明。

#include "SftpUploaderCore.h"
#include "SshClient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QThread>

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifndef Q_OS_WIN
#  include <sys/stat.h>
#endif

Q_LOGGING_CATEGORY(uploaderLog, "cubeshell.uploader")

namespace cubeshell {

// 元数据字段名，必须与 Python 侧逐字一致。
// 对应Python: sftp_uploader_core.py::_save_metadata 的 metadata dict
namespace {
const QString kMetaFileId       = QStringLiteral("file_id");
const QString kMetaLocalPath    = QStringLiteral("local_path");
const QString kMetaRemotePath   = QStringLiteral("remote_path");
const QString kMetaUploadedSize = QStringLiteral("uploaded_size");
const QString kMetaLastModified = QStringLiteral("last_modified");
const QString kMetaTimestamp    = QStringLiteral("timestamp");

// libssh2 SFTP 调用的 EAGAIN 重试（sessionLock 内调用）。
// 与 SftpClient.cpp 的同名帮助函数保持一致的行为。
template <typename Fn>
auto sftpRetryInt(SshClient *client, Fn &&fn) -> decltype(fn())
{
    for (;;) {
        auto rc = fn();
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            client->waitReadable(5000);
            continue;
        }
        return rc;
    }
}

template <typename Fn>
auto sftpRetryPtr(SshClient *client, Fn &&fn) -> decltype(fn())
{
    for (;;) {
        auto *h = fn();
        if (!h && libssh2_session_last_errno(client->rawSession()) == LIBSSH2_ERROR_EAGAIN) {
            client->waitReadable(5000);
            continue;
        }
        return h;
    }
}

// 远程路径的父目录（远端固定为 POSIX 路径语义）。
// 对应Python: os.path.dirname(remote_path)
QString remoteDirname(const QString &path)
{
    const int idx = path.lastIndexOf(QLatin1Char('/'));
    if (idx < 0)
        return QString();
    if (idx == 0)
        return QStringLiteral("/");
    return path.left(idx);
}

QString baseName(const QString &path)
{
    return QFileInfo(path).fileName(); // 对应Python: os.path.basename
}

} // namespace

SftpUploaderCore::SftpUploaderCore(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    // 对应Python: __init__ 的 metadata_dir = ~/.sftp_uploader + makedirs
    m_metadataDir = QDir::home().filePath(QStringLiteral(".sftp_uploader"));
    QDir().mkpath(m_metadataDir);
    m_pool.setMaxThreadCount(2);
}

SftpUploaderCore::~SftpUploaderCore()
{
    {
        QMutexLocker locker(&m_stateLock);
        for (auto &flag : m_cancelFlags)
            flag->store(true);
    }
    m_pool.waitForDone();
    if (m_sftp && m_client && m_client->rawSession()) {
        QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
        sftpRetryInt(m_client, [&] { return libssh2_sftp_shutdown(m_sftp); });
        m_sftp = nullptr;
    }
}

void SftpUploaderCore::setSshClient(SshClient *client)
{
    QMutexLocker locker(&m_stateLock);
    m_client = client;
    m_sftp = nullptr; // 换连接后需重开 SFTP 通道
}

void SftpUploaderCore::setMetadataDir(const QString &dir)
{
    m_metadataDir = dir;
    QDir().mkpath(m_metadataDir);
}

QString SftpUploaderCore::metadataDir() const
{
    return m_metadataDir;
}

// ---------------------------------------------------------------------------
// 纯逻辑（可单测）
// ---------------------------------------------------------------------------

// 对应Python: _upload_file_worker 的 while offset < total_size 分片循环
QVector<QPair<qint64, qint64>> SftpUploaderCore::planChunks(qint64 totalSize, qint64 startOffset)
{
    QVector<QPair<qint64, qint64>> chunks;
    if (startOffset < 0)
        startOffset = 0;
    for (qint64 offset = startOffset; offset < totalSize; ) {
        const qint64 size = qMin(kChunkSize, totalSize - offset);
        chunks.append(qMakePair(offset, size));
        offset += size;
    }
    return chunks;
}

// 对应Python: os.path.getmtime —— CPython 在 posixmodule.c::fill_time 里用
// `PyFloat_FromDouble(sec + 1e-9*nsec)` 生成 st_mtime，必须逐位复刻这个
// 表达式（写成 (sec*1e9+nsec)*1e-9 会在末位 bit 上产生差异，导致 Python
// 侧对 last_modified 的精确 != 比较判定“文件已改”，断点续传失效）。
double SftpUploaderCore::pythonMtime(const QString &path)
{
#ifndef Q_OS_WIN
    struct stat st{};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return -1.0;
#  ifdef Q_OS_DARWIN
    const auto &ts = st.st_mtimespec;
#  else
    const auto &ts = st.st_mtim;
#  endif
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
#else
    // Windows: QFileInfo 只有毫秒精度；CPython 在 Windows 上也是从 100ns
    // FILETIME 换算，毫秒级误差在这里可接受（跨平台互认场景是 POSIX）。
    const QFileInfo fi(path);
    if (!fi.exists())
        return -1.0;
    return double(fi.lastModified().toMSecsSinceEpoch()) / 1000.0;
#endif
}

// ---------------------------------------------------------------------------
// 断点续传元数据（与 Python 侧互认）
// ---------------------------------------------------------------------------

// 对应Python: _get_metadata_path
QString SftpUploaderCore::metadataPath(const QString &fileId) const
{
    return QDir(m_metadataDir).filePath(fileId + QStringLiteral(".json"));
}

// 对应Python: _save_metadata
bool SftpUploaderCore::saveMetadata(const QString &fileId, const QString &localPath,
                                    const QString &remotePath, qint64 uploadedSize) const
{
    QJsonObject metadata;
    metadata.insert(kMetaFileId, fileId);
    metadata.insert(kMetaLocalPath, localPath);
    metadata.insert(kMetaRemotePath, remotePath);
    metadata.insert(kMetaUploadedSize, double(uploadedSize));
    metadata.insert(kMetaLastModified, pythonMtime(localPath));
    metadata.insert(kMetaTimestamp, double(QDateTime::currentMSecsSinceEpoch()) / 1000.0);

    QFile f(metadataPath(fileId));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    // Compact JSON —— Python json.load 可读；字段名/类型与 json.dump 一致。
    f.write(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    return true;
}

// 对应Python: _load_metadata
bool SftpUploaderCore::loadMetadata(const QString &fileId, QJsonObject &out) const
{
    QFile f(metadataPath(fileId));
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        return false; // 对应Python: except: pass -> None
    const QJsonObject metadata = doc.object();

    const QString localPath = metadata.value(kMetaLocalPath).toString();
    if (localPath.isEmpty() || !QFile::exists(localPath))
        return false;
    // 文件被修改过 -> 元数据作废，重新上传（与 Python 的精确 != 比较一致）。
    const double currentMtime = pythonMtime(localPath);
    if (currentMtime != metadata.value(kMetaLastModified).toDouble())
        return false;
    out = metadata;
    return true;
}

// 对应Python: _delete_metadata
void SftpUploaderCore::deleteMetadata(const QString &fileId) const
{
    QFile::remove(metadataPath(fileId));
}

// ---------------------------------------------------------------------------
// 上传 API
// ---------------------------------------------------------------------------

template <typename Fn>
void SftpUploaderCore::postToSelf(Fn &&fn)
{
    // 工作线程 -> 本对象线程；显式 QueuedConnection，信号最终在本线程发射。
    QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
}

// 对应Python: upload_file
void SftpUploaderCore::uploadFile(const QString &fileId, const QString &localPath,
                                  const QString &remotePath)
{
    CancelFlag cancel = std::make_shared<std::atomic<bool>>(false);
    {
        QMutexLocker locker(&m_stateLock);
        m_cancelFlags.insert(fileId, cancel);
    }
    // QThreadPool 任务队列替代 Python 的每文件 threading.Thread。
    m_pool.start([this, fileId, localPath, remotePath, cancel]() {
        workerUpload(fileId, localPath, remotePath, cancel);
    });
}

// 对应Python: batch_upload
void SftpUploaderCore::batchUpload(const QHash<QString, QPair<QString, QString>> &fileMappings)
{
    for (auto it = fileMappings.begin(); it != fileMappings.end(); ++it)
        uploadFile(it.key(), it.value().first, it.value().second);
}

// 对应Python: cancel_upload
void SftpUploaderCore::cancelUpload(const QString &fileId)
{
    QMutexLocker locker(&m_stateLock);
    auto it = m_cancelFlags.find(fileId);
    if (it != m_cancelFlags.end())
        it.value()->store(true);
}

bool SftpUploaderCore::isCancelRequested(const QString &fileId) const
{
    QMutexLocker locker(&m_stateLock);
    auto it = m_cancelFlags.constFind(fileId);
    return it != m_cancelFlags.constEnd() && it.value()->load();
}

bool SftpUploaderCore::waitForFinished(int msecs)
{
    return m_pool.waitForDone(msecs);
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------

bool SftpUploaderCore::ensureSftp(QString &errorOut)
{
    QMutexLocker locker(&m_stateLock);
    if (m_sftp)
        return true;
    if (!m_client || !m_client->rawSession()) {
        errorOut = QStringLiteral("SFTP客户端未设置"); // 与 Python 错误文案一致
        return false;
    }
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    m_sftp = sftpRetryPtr(m_client, [&] { return libssh2_sftp_init(m_client->rawSession()); });
    if (!m_sftp) {
        errorOut = QStringLiteral("libssh2_sftp_init failed");
        return false;
    }
    return true;
}

bool SftpUploaderCore::remoteExists(const QString &remotePath)
{
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    const QByteArray p = remotePath.toUtf8();
    const int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_stat_ex(m_sftp, p.constData(), quint32(p.size()),
                                    LIBSSH2_SFTP_STAT, &attrs);
    });
    return rc == 0;
}

// 对应Python: _mkdir_p
bool SftpUploaderCore::remoteMkdirP(const QString &remotePath)
{
    if (remotePath.isEmpty() || remotePath == QStringLiteral("/"))
        return true;
    if (remoteExists(remotePath))
        return true;
    const QString parent = remoteDirname(remotePath);
    if (!parent.isEmpty() && parent != remotePath) {
        if (!remoteMkdirP(parent))
            return false;
    }
    QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
    const QByteArray p = remotePath.toUtf8();
    const int rc = sftpRetryInt(m_client, [&] {
        return libssh2_sftp_mkdir_ex(m_sftp, p.constData(), quint32(p.size()), 0755);
    });
    return rc == 0 || remoteExists(remotePath); // 并发下目录可能已被建出
}

// 对应Python: _upload_chunk（顺序上传模式 + 3 次重试）
// Python 对不存在的文件用 'wb' 创建并补零、已存在的用 'rb+' 定位写；
// libssh2 用 CREAT|WRITE（不带 TRUNC）+ seek64 一步覆盖两种情形：
// 越过 EOF 的写由 SFTP 服务端补零/稀疏化，语义一致。
bool SftpUploaderCore::uploadChunk(const QString &fileId, const QString &localPath,
                                   const QString &remotePath, qint64 offset,
                                   qint64 chunkSize, qint64 totalSize, QString &errorOut)
{
    for (int retry = 0; retry < kMaxRetries; ++retry) {
        QString err;

        // 读取本地分片。
        QFile local(localPath);
        QByteArray chunkData;
        if (local.open(QIODevice::ReadOnly) && local.seek(offset))
            chunkData = local.read(chunkSize);
        if (chunkData.isEmpty()) {
            err = QStringLiteral("read local chunk failed at offset %1").arg(offset);
        } else {
            // 写远端（sessionLock 串行，粒度与 Python 的 op_lock 相同：整个分片）。
            QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
            const QByteArray p = remotePath.toUtf8();
            LIBSSH2_SFTP_HANDLE *handle = sftpRetryPtr(m_client, [&] {
                return libssh2_sftp_open_ex(m_sftp, p.constData(), quint32(p.size()),
                                            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                            0644, LIBSSH2_SFTP_OPENFILE);
            });
            if (!handle) {
                // 父目录可能不存在（Python 在此路径下也会补建目录）。
                lock.unlock();
                remoteMkdirP(remoteDirname(remotePath));
                lock.relock();
                handle = sftpRetryPtr(m_client, [&] {
                    return libssh2_sftp_open_ex(m_sftp, p.constData(), quint32(p.size()),
                                                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                                0644, LIBSSH2_SFTP_OPENFILE);
                });
            }
            if (!handle) {
                err = QStringLiteral("open remote file failed: %1").arg(remotePath);
            } else {
                libssh2_sftp_seek64(handle, quint64(offset));
                qint64 written = 0;
                while (written < chunkData.size()) {
                    const ssize_t n = sftpRetryInt(m_client, [&] {
                        return libssh2_sftp_write(handle,
                                                  chunkData.constData() + written,
                                                  size_t(chunkData.size() - written));
                    });
                    if (n < 0) {
                        err = QStringLiteral("sftp write failed (rc=%1)").arg(qint64(n));
                        break;
                    }
                    written += n;
                }
                sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(handle); });
            }
        }

        if (err.isEmpty()) {
            const qint64 done = offset + chunkData.size();
            // 更新元数据（对应Python: _save_metadata(offset + len(chunk_data))）。
            saveMetadata(fileId, localPath, remotePath, done);

            // 更新进度（百分比与字节级双信号，经 QueuedConnection 回本线程）。
            const int progress = int(qMin<qint64>(100, done * 100 / qMax<qint64>(1, totalSize)));
            const QString filename = baseName(localPath);
            postToSelf([this, fileId, progress, filename, done, totalSize]() {
                emit progressUpdated(fileId, progress, filename);
                emit progressChanged(fileId, done, totalSize);
            });

            // 判断是否完成。
            if (done >= totalSize) {
                deleteMetadata(fileId);
                postToSelf([this, fileId, filename]() {
                    emit uploadCompleted(fileId, filename);
                });
            }
            return true;
        }

        if (retry < kMaxRetries - 1) {
            QThread::sleep(1); // 对应Python: time.sleep(1) 后重试
        } else {
            errorOut = QStringLiteral("上传失败(重试%1次): %2").arg(kMaxRetries).arg(err);
            return false;
        }
    }
    return false;
}

// 对应Python: _upload_file_worker
void SftpUploaderCore::workerUpload(const QString &fileId, const QString &localPath,
                                    const QString &remotePath, CancelFlag cancel)
{
    const QString filename = baseName(localPath);
    QString err;

    auto emitFailed = [this, fileId, filename](const QString &message) {
        postToSelf([this, fileId, filename, message]() {
            emit uploadFailed(fileId, filename, message);
        });
    };

    do {
        if (cancel->load())
            break; // 入队后未开始就被取消

        if (!ensureSftp(err)) {
            emitFailed(err);
            break;
        }
        if (!QFile::exists(localPath)) {
            emitFailed(QStringLiteral("本地文件不存在: %1").arg(localPath));
            break;
        }

        // 确保远程目录存在。
        const QString remoteDir = remoteDirname(remotePath);
        if (!remoteDir.isEmpty() && !remoteExists(remoteDir)) {
            if (!remoteMkdirP(remoteDir)) {
                emitFailed(QStringLiteral("创建远程目录失败: %1").arg(remoteDir));
                break;
            }
        }

        const qint64 totalSize = QFileInfo(localPath).size();

        // 断点续传：元数据有效且 local/remote 一致时从 uploaded_size 续传。
        qint64 offset = 0;
        QJsonObject metadata;
        if (loadMetadata(fileId, metadata)
            && metadata.value(QStringLiteral("local_path")).toString() == localPath
            && metadata.value(QStringLiteral("remote_path")).toString() == remotePath
            && metadata.contains(QStringLiteral("uploaded_size"))) {
            offset = qint64(metadata.value(QStringLiteral("uploaded_size")).toDouble());
        }

        postToSelf([this, fileId, filename, totalSize]() {
            emit uploadStarted(fileId, filename, totalSize);
        });

        // 上传文件分片。
        const auto chunks = planChunks(totalSize, offset);
        for (const auto &chunk : chunks) {
            if (cancel->load())
                break; // 对应Python: stop_events[file_id].is_set()
            QString chunkErr;
            if (!uploadChunk(fileId, localPath, remotePath,
                             chunk.first, chunk.second, totalSize, chunkErr)) {
                emitFailed(chunkErr);
                break;
            }
        }

        // 空文件：无分片可传，直接创建远端空文件并报完成。
        if (totalSize == 0 && !cancel->load()) {
            QString chunkErr;
            QMutexLocker<QRecursiveMutex> lock(&m_client->sessionLock());
            const QByteArray p = remotePath.toUtf8();
            LIBSSH2_SFTP_HANDLE *handle = sftpRetryPtr(m_client, [&] {
                return libssh2_sftp_open_ex(m_sftp, p.constData(), quint32(p.size()),
                                            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                            0644, LIBSSH2_SFTP_OPENFILE);
            });
            if (handle)
                sftpRetryInt(m_client, [&] { return libssh2_sftp_close_handle(handle); });
            Q_UNUSED(chunkErr);
            postToSelf([this, fileId, filename]() {
                emit uploadCompleted(fileId, filename);
            });
        }
    } while (false);

    // 清理线程资源。对应Python: finally 中删除 upload_threads/stop_events。
    QMutexLocker locker(&m_stateLock);
    m_cancelFlags.remove(fileId);
}

} // namespace cubeshell
