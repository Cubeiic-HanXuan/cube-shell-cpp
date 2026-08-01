#pragma once

// SftpUploaderCore.h — SFTP 分片上传核心（4MB 分片 / 失败重试 / 断点续传）。
//
// 对应Python: core/uploader/sftp_uploader_core.py::SFTPUploaderCore
//
// 与 Python 版的对应关系：
//  - 4MB 分片（CHUNK_SIZE）、失败分片重试 3 次（MAX_RETRIES）、
//    断点续传元数据 ~/.sftp_uploader/{file_id}.json —— 逐字段兼容，双向互认：
//        { "file_id", "local_path", "remote_path",
//          "uploaded_size", "last_modified", "timestamp" }
//    last_modified 按 CPython os.path.getmtime 的算法计算
//    （st_mtime_ns * 1e-9 的 double），与 Python 侧做精确相等比较。
//  - Python 用 threading.Thread（每文件一线程）+ op_lock 互斥串行化所有
//    SFTP 操作；C++ 用 QThreadPool + 任务队列（每文件一个任务），所有
//    libssh2 SFTP 调用经 SshClient::sessionLock() 串行（队列并发、写入串行，
//    与 Python 的互斥串行模型语义一致 —— 见下方"并发模型"）。
//  - 取消标志：Python 的 threading.Event -> 每文件一个 std::atomic<bool>。
//  - 进度信号从工作线程经 Qt::QueuedConnection 回投到本对象线程后发射。
//
// 并发模型（为什么是"队列串行写"而不是每 worker 独立 SFTP 会话）：
// libssh2 的 SFTP handle 与其底层 LIBSSH2_SESSION 都非线程安全；本工程的
// SshClient 一个实例只承载一条 SSH 连接（一个 LIBSSH2_SESSION），SftpClient
// 也复用同一个 session。要做真正并行的多会话上传就必须为每个 worker 各建一条
// SSH 连接（需要凭据、握手成本高），而 Python 原实现本来就是 op_lock 全局
// 串行。因此这里选择：QThreadPool 提供任务级并发（多个文件排队、进度独立、
// 可单独取消），而对同一 SshClient 的每次 SFTP 调用都在
// SshClient::sessionLock()（递归锁）内完成 —— 传输吞吐与 Python 持平，
// 且与共存的 shell 通道/转发通道安全共享会话。
//
// 注意：本类只调用 SshClient 的公开接口（rawSession()/sessionLock()/
// waitReadable()），自己 libssh2_sftp_init 一个独立 SFTP 子系统通道，
// 不触碰 SftpClient（其 LIBSSH2_SFTP 句柄是私有的）。

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <memory>

struct _LIBSSH2_SFTP;

namespace cubeshell {

class SshClient;
struct SshError;

// 对应Python: class SFTPUploaderCore(QObject)
class SftpUploaderCore : public QObject {
    Q_OBJECT
public:
    // 分片大小: 4MB。对应Python: SFTPUploaderCore.CHUNK_SIZE
    static constexpr qint64 kChunkSize = 4 * 1024 * 1024;
    // 最大重试次数。对应Python: SFTPUploaderCore.MAX_RETRIES
    static constexpr int kMaxRetries = 3;

    // client: 已连接的 SshClient（外部系统创建和管理，不拥有；可为空，
    // 之后用 setSshClient 注入 —— 对应 Python 的 sftp_client 参数语义）。
    explicit SftpUploaderCore(SshClient *client = nullptr, QObject *parent = nullptr);
    ~SftpUploaderCore() override;

    // 对应Python: set_sftp_client
    void setSshClient(SshClient *client);

    // 断点续传元数据目录（默认 ~/.sftp_uploader，与 Python 侧共用；
    // 测试可重定向到临时目录）。
    void setMetadataDir(const QString &dir);
    QString metadataDir() const;

    // 并发任务数（QThreadPool 工作线程数；默认 2 —— 传输本身经 sessionLock
    // 串行，更多线程只增加排队并发度）。
    void setMaxConcurrentUploads(int n) { m_pool.setMaxThreadCount(qMax(1, n)); }

    // --- 上传 API ---
    // 对应Python: upload_file(file_id, local_path, remote_path)
    // 立即返回；任务进入 QThreadPool 队列。
    void uploadFile(const QString &fileId, const QString &localPath, const QString &remotePath);
    // 对应Python: batch_upload({file_id: (local, remote)})
    void batchUpload(const QHash<QString, QPair<QString, QString>> &fileMappings);
    // 对应Python: cancel_upload(file_id)。设置取消标志；已入队未开始的任务
    // 会直接跳过，进行中的任务在当前分片完成后停止（与 Python 语义一致）。
    void cancelUpload(const QString &fileId);
    // 取消标志查询（供测试/上层 UI 使用）。
    bool isCancelRequested(const QString &fileId) const;
    // 阻塞等待所有上传任务结束（测试用）。msecs<0 表示无限等。
    bool waitForFinished(int msecs = -1);

    // --- 纯逻辑（静态，可单测，不依赖网络） ---
    // 分片切分：从 startOffset 起把 totalSize 切成 (offset, size) 序列。
    // 对应Python: _upload_file_worker 的 while offset < total_size 循环
    static QVector<QPair<qint64, qint64>> planChunks(qint64 totalSize, qint64 startOffset = 0);
    // CPython os.path.getmtime 兼容的 double mtime（st_mtime_ns * 1e-9）；
    // 文件不存在返回 -1。对应Python: os.path.getmtime
    static double pythonMtime(const QString &path);

    // --- 断点续传元数据（公开以便测试格式往返；与 Python 侧完全互认） ---
    // 对应Python: _get_metadata_path
    QString metadataPath(const QString &fileId) const;
    // 对应Python: _save_metadata
    bool saveMetadata(const QString &fileId, const QString &localPath,
                      const QString &remotePath, qint64 uploadedSize) const;
    // 对应Python: _load_metadata —— 校验本地文件存在且 last_modified 精确
    // 相等，否则视为无效返回 false（文件被改过要重传）。
    bool loadMetadata(const QString &fileId, QJsonObject &out) const;
    // 对应Python: _delete_metadata
    void deleteMetadata(const QString &fileId) const;

signals:
    // 对应Python: upload_started = Signal(str, str, int)
    void uploadStarted(const QString &fileId, const QString &filename, qint64 totalSize);
    // 对应Python: progress_updated = Signal(str, int, str)（百分比）
    void progressUpdated(const QString &fileId, int progress, const QString &filename);
    // 字节级进度（任务要求的 progressChanged(file, done, total)）。
    void progressChanged(const QString &fileId, qint64 done, qint64 total);
    // 对应Python: upload_completed = Signal(str, str)
    void uploadCompleted(const QString &fileId, const QString &filename);
    // 对应Python: upload_failed = Signal(str, str, str)
    void uploadFailed(const QString &fileId, const QString &filename, const QString &error);

private:
    using CancelFlag = std::shared_ptr<std::atomic<bool>>;

    // 工作线程主体。对应Python: _upload_file_worker
    void workerUpload(const QString &fileId, const QString &localPath,
                      const QString &remotePath, CancelFlag cancel);
    // 单分片上传（含 3 次重试）。对应Python: _upload_chunk
    bool uploadChunk(const QString &fileId, const QString &localPath,
                     const QString &remotePath, qint64 offset, qint64 chunkSize,
                     qint64 totalSize, QString &errorOut);
    // 递归创建远程目录。对应Python: _mkdir_p
    bool remoteMkdirP(const QString &remotePath);
    bool remoteExists(const QString &remotePath);
    // 懒初始化本类专用的 LIBSSH2_SFTP 子系统通道（sessionLock 内）。
    bool ensureSftp(QString &errorOut);

    // 把信号发射回本对象线程（工作线程中调用）。
    template <typename Fn> void postToSelf(Fn &&fn);

    SshClient *m_client = nullptr;      // 不拥有
    _LIBSSH2_SFTP *m_sftp = nullptr;    // 本类自己的 SFTP 通道
    QString m_metadataDir;
    QThreadPool m_pool;
    QHash<QString, CancelFlag> m_cancelFlags; // 对应Python: stop_events
    mutable QMutex m_stateLock;               // 保护 m_cancelFlags / m_sftp 指针
};

} // namespace cubeshell
