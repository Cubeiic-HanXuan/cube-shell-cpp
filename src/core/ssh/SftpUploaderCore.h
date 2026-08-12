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
// 并发模型（两级并行）：
// libssh2 的 SFTP handle 与其底层 LIBSSH2_SESSION 都非线程安全，同一个
// session 上的所有调用必须经 SshClient::sessionLock() 串行。
//
// 第一级（凭据可重放：密码/密钥直连）：SftpTransferPool 按主连接凭据另开
// 若干条独立 SSH 连接（各自一个 session + 一个 SFTP 通道），每条流独占一条，
// 无锁竞争，真正并行。主连接不参与传输，终端 shell 不被大文件传输拖住。
//
// 第二级（凭据不可重放：jms 一次性 token / keyboard-interactive MFA）：
// 克隆不了连接，但 SSH 协议允许一条连接复用多条 channel —— 连接池改在主
// session 上开多条独立 SFTP 通道，每条流各用一条。所有 channel 仍共享
// sessionLock，但本文件按「逐调用加锁、EAGAIN 锁外等待」的纪律
// （sftpCallInt/sftpCallPtr），多条通道的分片在一条 TCP 连接上交错在飞，
// 且 UI 线程的终端写入不再被持锁空等堵死（曾因此卡 UI）。
//
// 并行发生在两个维度上：
//   - 多文件：QThreadPool 每文件一个任务，各任务租用不同连接/通道同时传。
//   - 单个大文件：切成 4MB 分片后由 kMaxStreamsPerFile 条流按"工作窃取"
//     并行抢传（每条流各开一个远端 handle，seek 到自己的分片位置写）。
//
// 最终降级：连主 session 的通道也开不出来（MaxSessions 打满）时，退回
// "主连接共享 SFTP 通道 + sessionLock 串行"，功能不变只是不加速。
// 详见 SftpTransferPool.h。
//
// 断点续传与并行的关系：元数据只有 uploaded_size 一个标量（Python 侧要按
// 字段互认，不能加字段），标量天然只能表达"前缀已完成"。因此乱序完成的分片
// 不直接进 uploaded_size，而是只把**连续前缀**的水位写进去 —— 中断后重传会
// 把水位之后已传的分片再传一遍（保守但绝不出错），Python 侧读到的仍是合法值。

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QRecursiveMutex>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <memory>

struct _LIBSSH2_SFTP;
struct _LIBSSH2_SFTP_HANDLE;

namespace cubeshell {

class SshClient;
class SftpTransferPool;
struct SshError;

// 对应Python: class SFTPUploaderCore(QObject)
class SftpUploaderCore : public QObject {
    Q_OBJECT
public:
    // 分片大小: 4MB。对应Python: SFTPUploaderCore.CHUNK_SIZE
    static constexpr qint64 kChunkSize = 4 * 1024 * 1024;
    // 最大重试次数。对应Python: SFTPUploaderCore.MAX_RETRIES
    static constexpr int kMaxRetries = 3;
    // 单个文件最多用几条并行流。再多的话 SFTP 服务端的磁盘写入通常先成为
    // 瓶颈，且每条流都占一条 SSH 连接（撞 sshd MaxSessions 默认 10）。
    static constexpr int kMaxStreamsPerFile = 4;
    // 小于此大小的文件不拆流并行（多开连接的握手成本超过收益）。
    static constexpr qint64 kMinSizeForMultiStream = 8 * 1024 * 1024;

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

    // 同时上传的文件数（QThreadPool 工作线程数）。每个任务会从连接池租一条
    // 独立连接，所以这里的并发是真并发。
    void setMaxConcurrentUploads(int n) { m_pool->setMaxThreadCount(qMax(1, n)); }

    // 并行传输连接数上限（含降级用的主连接）。1 = 完全串行。
    void setMaxTransferConnections(int n);
    // 允许/禁止克隆独立连接（见 SftpTransferPool::setCloningEnabled）。
    // 禁用后并行走主 session 通道多路复用（jms token / MFA 的默认形态）；
    // 对之后因换连接新建的连接池同样生效。
    void setCloningEnabled(bool enabled);
    // 实际可用的并行度（已建成的传输连接数）。传输开始后才有意义。
    int activeTransferConnections() const;
    // 当前连接是否支持并行传输（可克隆 → 独立连接；jms token / MFA → 主
    // session 通道多路复用；皆不可时为 false）。
    bool canParallelize() const;

    // 后台预建传输连接。挂接连接后尽早调用：每条连接要走完整 SSH 握手
    // （25ms RTT 链路上约 300ms），不预热的话这笔成本会落在首次上传上，
    // 反而比串行更慢。详见 SftpTransferPool::prewarm。
    void prewarmConnections();

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

    // 一个文件的并行上传共享状态：分片游标（工作窃取）、完成集合、
    // 连续前缀水位、错误短路。多条流并发访问，全部由 mutex 保护。
    struct FileTransferState;

    // 工作线程与本对象之间的共享状态，生命周期独立于 SftpUploaderCore。
    //
    // 为什么必须独立：析构跑在 UI 线程上（关闭标签页 / 退出程序），等不到
    // 工作线程时必须放手走人，否则窗口就冻死了（见 ~SftpUploaderCore）。
    // 那一刻工作线程还在跑，如果它经由 this 去取取消标志、连接池或元数据
    // 目录，就是 use-after-free —— 成员按声明逆序销毁，m_cancelFlags 甚至
    // 早于 m_pool 就没了，工作线程收尾时必崩（实测 SIGSEGV in QHash）。
    // 所以工作线程用到的一切都放在这个由 shared_ptr 共同持有的块里，
    // 全程不解引用 this。
    struct WorkerContext {
        QMutex lock;                            // 保护 owner / cancelFlags
        // 宿主对象；置空表示已析构，此后不得再向它投递任何东西。
        SftpUploaderCore *owner = nullptr;
        QHash<QString, CancelFlag> cancelFlags; // 对应Python: stop_events
        std::shared_ptr<SftpTransferPool> pool; // 并行传输连接池
        QString metadataDir;                    // 快照，工作线程只读
    };
    using WorkerContextPtr = std::shared_ptr<WorkerContext>;

    // 以下工作线程侧函数一律以 ctx 取状态，不碰 this（原因见 WorkerContext）。
    // 声明为 static 是硬约束：编译器保证它们没有 this 可用。

    // 工作线程主体。对应Python: _upload_file_worker
    static void workerUpload(const WorkerContextPtr &ctx, const QString &fileId,
                             const QString &localPath, const QString &remotePath,
                             CancelFlag cancel);
    // 一条并行流：租一条连接，开一次远端 handle，循环领取分片直到取完。
    // 对应Python: _upload_chunk 的循环体（Python 侧是单线程串行）
    static void runUploadStream(const WorkerContextPtr &ctx, FileTransferState &state,
                                const QString &fileId, const QString &localPath,
                                const QString &remotePath);
    // 单分片写入（含 kMaxRetries 次重试），在已开好的 handle 上做。
    // cancel: 取消标志（可为空）。置位时立即放弃重试与 EAGAIN 等待，
    // 让关闭路径上的工作线程尽快退出。
    static bool writeChunk(struct _LIBSSH2_SFTP_HANDLE *handle, QRecursiveMutex *lock,
                           SshClient *client, const QByteArray &data, qint64 offset,
                           QString &errorOut, const std::atomic<bool> *cancel);
    // 递归创建远程目录。对应Python: _mkdir_p
    static bool remoteMkdirP(const WorkerContextPtr &ctx, const QString &remotePath);
    static bool remoteExists(const WorkerContextPtr &ctx, const QString &remotePath);
    // 建立空文件（totalSize == 0 的情形）。
    static bool createEmptyRemoteFile(const WorkerContextPtr &ctx, const QString &remotePath,
                                      QString &errorOut);

    // 元数据读写的实现体：不依赖成员，供工作线程用 ctx->metadataDir 调用；
    // 同名的公开成员函数是它们在 m_metadataDir 上的薄包装。
    static QString metadataPathIn(const QString &dir, const QString &fileId);
    static bool saveMetadataIn(const QString &dir, const QString &fileId,
                               const QString &localPath, const QString &remotePath,
                               qint64 uploadedSize);
    static bool loadMetadataIn(const QString &dir, const QString &fileId, QJsonObject &out);
    static void deleteMetadataIn(const QString &dir, const QString &fileId);

    // 把信号发射回宿主对象线程（工作线程中调用）。宿主已析构则整个丢弃。
    template <typename Fn> static void postToOwner(const WorkerContextPtr &ctx, Fn &&fn);

    SshClient *m_client = nullptr;      // 不拥有
    // 连接池的另一份持有在 m_ctx->pool 上：析构等不到工作线程时，本对象先走，
    // 池由最后一个退出的工作线程释放，不再需要"故意泄漏"。
    std::shared_ptr<SftpTransferPool> m_transferPool;
    bool m_cloningEnabled = true;       // 新建连接池时应用（见 setCloningEnabled）
    QString m_metadataDir;
    // 指针持有：析构超时时要能把它整个丢下（release），否则 ~QThreadPool 会
    // 无条件 waitForDone()，UI 线程照样被钉死 —— 那正是逃生口要避免的事。
    std::unique_ptr<QThreadPool> m_pool;
    WorkerContextPtr m_ctx;             // 与工作线程共享，见 WorkerContext
    mutable QMutex m_stateLock;         // 保护 m_client
};

} // namespace cubeshell
