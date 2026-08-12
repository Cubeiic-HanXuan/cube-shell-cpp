// SftpUploaderCore.cpp — SFTP 分片上传核心。见 SftpUploaderCore.h 的
// 并发模型与元数据兼容性说明。

#include "SftpUploaderCore.h"
#include "SftpTransferPool.h"
#include "SshClient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifndef Q_OS_WIN
#  include <sys/stat.h>
#endif

Q_LOGGING_CATEGORY(uploaderLog, "cubeshell.uploader")

// 析构/换连接时等待上传工作线程退出的上限。取消标志置位 + socket 打断后，
// 各流会在当前分片边界立刻退出，正常远小于此值；超时则放弃回收连接池
// （见 ~SftpUploaderCore），绝不让 UI 线程无限等。与 SftpClient 的
// kWorkerJoinTimeoutMs 同值同理。
static constexpr int kWorkerJoinTimeoutMs = 5000;

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

// 取消感知的加锁。共享 session（主连接 / 通道槽）用的是 SshClient::sessionLock()，
// 终端读循环也在争它 —— 无限等锁的话取消标志根本没机会被检查，工作线程退不出，
// 关闭标签页就只能靠"泄漏连接池"兜底。这里改成轮询式 tryLock，两者之间
// 检查取消标志，让关闭路径总能及时收敛。
// 独占的克隆连接上这把锁没人竞争，首次 tryLock 即成功，无额外开销。
class CancelableLock {
public:
    CancelableLock(QRecursiveMutex *lock, const std::atomic<bool> *cancel)
        : m_lock(lock)
    {
        for (;;) {
            if (cancel && cancel->load()) {
                m_lock = nullptr; // 已取消：不再争锁
                return;
            }
            if (m_lock->tryLock(100))
                return;
        }
    }
    ~CancelableLock()
    {
        if (m_lock)
            m_lock->unlock();
    }
    CancelableLock(const CancelableLock &) = delete;
    CancelableLock &operator=(const CancelableLock &) = delete;
    bool held() const { return m_lock != nullptr; }

private:
    QRecursiveMutex *m_lock;
};

// 在租约连接上执行一次 int 返回的 libssh2 SFTP 调用，EAGAIN 自动重试。
//
// 锁纪律（本文件的硬规矩）：锁粒度收缩到单次调用，EAGAIN 的等待在锁外。
// 共享 session（主连接 / 主 session 通道槽）上这是刚需 —— 持锁 select 空等
// 会把终端读循环和 UI 线程的 writeChannel 全部堵死（jms/MFA 会话批量上传
// 卡 UI 的根因）；锁一放，多条 SFTP 通道的分片才能在一条 TCP 连接上交错
// 在飞。独占克隆连接（阻塞 session）上没有 EAGAIN，同一条路径退化为
// 「拿锁一次调用」，两种租约不需要两套写法。
//
// cancel 置位时立即带 EAGAIN 哨兵返回（调用方把它当失败并走取消分支）：
// 关闭路径上如果不看取消标志，非阻塞的主连接会一直 EAGAIN → select →
// EAGAIN 地空转，工作线程永不退出，析构就卡住了。
template <typename Fn>
auto sftpCallInt(QRecursiveMutex *lock, SshClient *client, Fn &&fn,
                 const std::atomic<bool> *cancel = nullptr) -> decltype(fn())
{
    for (;;) {
        decltype(fn()) rc;
        {
            CancelableLock guard(lock, cancel);
            if (!guard.held())
                return static_cast<decltype(fn())>(LIBSSH2_ERROR_EAGAIN); // 已取消
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

// 句柄返回版。EAGAIN 判定（session last_errno）必须在锁内完成 —— 放锁后
// 其它线程的 libssh2 调用会改写 last_errno，锁外判定会把真失败误判成 EAGAIN。
template <typename Fn>
auto sftpCallPtr(QRecursiveMutex *lock, SshClient *client, Fn &&fn,
                 const std::atomic<bool> *cancel = nullptr) -> decltype(fn())
{
    for (;;) {
        decltype(fn()) h;
        bool again = false;
        {
            CancelableLock guard(lock, cancel);
            if (!guard.held())
                return nullptr; // 已取消
            h = fn();
            if (!h)
                again = libssh2_session_last_errno(client->rawSession())
                        == LIBSSH2_ERROR_EAGAIN;
        }
        if (again) {
            if (cancel && cancel->load())
                return h;
            client->waitReadable(5000);
            continue;
        }
        return h;
    }
}

// 读取 session 最近错误文本（用于失败文案；调用时最好已持锁，诊断用途容错）。
QString sessionErrorText(SshClient *client)
{
    if (!client || !client->rawSession())
        return QString();
    char *msg = nullptr;
    int len = 0;
    libssh2_session_last_error(client->rawSession(), &msg, &len, 0);
    if (msg && len > 0)
        return QString::fromLatin1(msg, len);
    return QString();
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

// 在【已有租约】上判断远端路径是否存在。不新租连接 —— 供已持有租约的流复用
// 同一条通道（见 runUploadStream 的目录补建），避免「持租约再租」的嵌套：
// 连接池对主 session 通道是一律独占、租不到就等的（见 SftpTransferPool::lease），
// 嵌套再租会让一个工作线程等自己占着的槽，直接死锁。
bool remoteExistsOn(SftpTransferPool::Lease &lease, const QString &remotePath)
{
    if (!lease.isValid())
        return false;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    const QByteArray p = remotePath.toUtf8();
    const int rc = sftpCallInt(lease.lock(), lease.client(), [&] {
        return libssh2_sftp_stat_ex(lease.sftp(), p.constData(), quint32(p.size()),
                                    LIBSSH2_SFTP_STAT, &attrs);
    });
    return rc == 0;
}

// 在【已有租约】上递归创建远程目录（对应Python: _mkdir_p）。
bool remoteMkdirPOn(SftpTransferPool::Lease &lease, const QString &remotePath)
{
    if (remotePath.isEmpty() || remotePath == QStringLiteral("/"))
        return true;
    if (remoteExistsOn(lease, remotePath))
        return true;
    const QString parent = remoteDirname(remotePath);
    if (!parent.isEmpty() && parent != remotePath) {
        if (!remoteMkdirPOn(lease, parent))
            return false;
    }
    const QByteArray p = remotePath.toUtf8();
    const int rc = sftpCallInt(lease.lock(), lease.client(), [&] {
        return libssh2_sftp_mkdir_ex(lease.sftp(), p.constData(), quint32(p.size()), 0755);
    });
    return rc == 0 || remoteExistsOn(lease, remotePath); // 并发下目录可能已被别的流建出
}

} // namespace

// 单个文件并行上传的共享状态。多条流并发访问，全部字段由 lock 保护
// （progressDone 除外，它是 atomic，供进度信号无锁读取）。
//
// 分片分配用"工作窃取"而不是静态均分：各条连接的实际速度可能差好几倍
// （服务端调度、丢包重传），静态均分会被最慢的一条拖到底。游标式领取
// 让快的流多干活，整体收敛到最慢流只多干一个分片的时间。
struct SftpUploaderCore::FileTransferState {
    QMutex lock;
    QVector<QPair<qint64, qint64>> chunks; // planChunks 的结果（有序）
    int nextChunk = 0;                     // 工作窃取游标
    QVector<bool> done;                    // 各分片是否完成（下标同 chunks）
    int completedCount = 0;
    qint64 baseOffset = 0;                 // 断点续传起点（此前的字节视为已传）
    qint64 watermark = 0;                  // 连续前缀水位，写进元数据的值
    qint64 totalSize = 0;
    bool failed = false;
    QString error;
    std::atomic<qint64> progressDone{0};   // 已传字节数（含乱序完成的）
    std::atomic<bool> *cancel = nullptr;

    // 领取下一个待传分片；返回 false 表示没有了（或已失败/已取消）。
    bool takeNext(int &indexOut, qint64 &offsetOut, qint64 &sizeOut)
    {
        QMutexLocker locker(&lock);
        if (failed || (cancel && cancel->load()))
            return false;
        if (nextChunk >= chunks.size())
            return false;
        indexOut = nextChunk++;
        offsetOut = chunks[indexOut].first;
        sizeOut = chunks[indexOut].second;
        return true;
    }

    // 标记分片完成，并把连续前缀水位往前推。返回推进后的水位。
    // 乱序完成的分片不会抬高水位——水位只表示"这之前全部落盘了"，
    // 这正是 Python 侧 uploaded_size 标量字段的语义。
    qint64 markDone(int index, qint64 chunkSize)
    {
        QMutexLocker locker(&lock);
        if (index >= 0 && index < done.size() && !done[index]) {
            done[index] = true;
            ++completedCount;
        }
        int i = 0;
        while (i < done.size() && done[i])
            ++i;
        // 前 i 个分片连续完成 -> 水位 = 起点 + 这些分片的总长。
        watermark = baseOffset;
        for (int k = 0; k < i; ++k)
            watermark += chunks[k].second;
        progressDone.fetch_add(chunkSize);
        return watermark;
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

SftpUploaderCore::SftpUploaderCore(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_transferPool(std::make_shared<SftpTransferPool>(client))
    , m_pool(std::make_unique<QThreadPool>())
    , m_ctx(std::make_shared<WorkerContext>())
{
    // 对应Python: __init__ 的 metadata_dir = ~/.sftp_uploader + makedirs
    m_metadataDir = QDir::home().filePath(QStringLiteral(".sftp_uploader"));
    QDir().mkpath(m_metadataDir);
    m_ctx->owner = this;
    m_ctx->pool = m_transferPool;
    m_ctx->metadataDir = m_metadataDir;
    // 每个任务租一条独立连接，线程数与连接数上限对齐才有意义。
    m_pool->setMaxThreadCount(SftpTransferPool::kDefaultMaxConnections);
}

SftpUploaderCore::~SftpUploaderCore()
{
    // 先与工作线程断开联系：置空 owner 之后它们不再向本对象投递任何东西，
    // 也不再读本对象的任何成员（它们要的都在 ctx 里）。必须在等线程之前做，
    // 因为下面可能等不到就走人。
    {
        QMutexLocker locker(&m_ctx->lock);
        m_ctx->owner = nullptr;
        for (auto &flag : m_ctx->cancelFlags)
            flag->store(true);
    }
    // 取消标志只在分片边界被检查；正卡在 libssh2_sftp_write 里的流（克隆连接
    // 是阻塞模式 session）看不到它，会一直挂到 TCP 超时。先打断 socket，
    // waitForDone 才能及时返回，否则析构会长时间卡住 UI 线程。
    m_transferPool->shutdownTransferSockets();
    if (!m_pool->waitForDone(kWorkerJoinTimeoutMs)) {
        // 兜底：宁可泄漏也不要冻住 UI 线程。析构跑在 UI 线程上（关闭标签页 /
        // 退出程序），此处若无限等，窗口就再也不响应了。
        //
        // 线程池必须 release() 而不是留给成员析构：~QThreadPool 会无条件
        // waitForDone()，那等于白设超时，UI 一样被钉死。
        //
        // 连接池也再泄漏一份持有，让它永不析构。不交给最后退出的工作线程去
        // 析构，是因为那时 SshClient 可能已经没了（标签页的析构顺序是先释放
        // client 的 shared_ptr，再由 ~QObject 删本对象），关闭往返会往死
        // session 上写而崩。泄漏的 fd 随进程退出由内核回收 —— 这条路径本身
        // 就是异常兜底，不是常态。
        //
        // 工作线程此后只用 ctx（取消标志 / 连接池 / 元数据目录），不碰本对象，
        // 所以它们跑完为止都是安全的。
        qCWarning(uploaderLog) << "upload workers did not finish within"
                               << kWorkerJoinTimeoutMs
                               << "ms; detaching thread pool and transfer pool"
                                  " to avoid freezing the UI";
        (void)m_pool.release();
        (void)new std::shared_ptr<SftpTransferPool>(m_transferPool);
        return;
    }
    // 正常路径：线程都退干净了，在本线程（UI 线程）关闭所有传输连接。
    m_ctx->pool.reset();
    m_transferPool.reset();
}

void SftpUploaderCore::setSshClient(SshClient *client)
{
    // 换连接前必须让在途任务全部退出：它们持有旧连接的租约，
    // setPrimary 会把那些连接关掉、释放掉。
    {
        QMutexLocker locker(&m_ctx->lock);
        for (auto &flag : m_ctx->cancelFlags)
            flag->store(true);
    }
    m_transferPool->shutdownTransferSockets();
    const bool drained = m_pool->waitForDone(kWorkerJoinTimeoutMs);

    QMutexLocker locker(&m_stateLock);
    m_client = client;
    if (!drained) {
        // 还有流在用旧连接，不能 setPrimary（会把它们脚下的连接关掉）。
        // 弃掉旧池另起一个：旧池由还在跑的工作线程经 ctx 持有，等它们退完
        // 自然释放；新任务用新池。
        qCWarning(uploaderLog) << "upload workers still running while switching client; "
                                  "abandoning old transfer pool";
        m_transferPool = std::make_shared<SftpTransferPool>(client);
        m_transferPool->setCloningEnabled(m_cloningEnabled);
        // 注意不换 ctx：在途任务还要靠它读自己的取消标志。它们各自的租约
        // 指向旧池，旧池的持有留在它们的栈上，不受这里替换的影响。
        QMutexLocker ctxLock(&m_ctx->lock);
        m_ctx->pool = m_transferPool;
        return;
    }
    m_transferPool->setPrimary(client); // 旧的传输连接全部作废，按新凭据重建
}

void SftpUploaderCore::setMaxTransferConnections(int n)
{
    m_transferPool->setMaxConnections(n);
    m_pool->setMaxThreadCount(qMax(1, n));
}

void SftpUploaderCore::setCloningEnabled(bool enabled)
{
    m_cloningEnabled = enabled;
    m_transferPool->setCloningEnabled(enabled);
}

int SftpUploaderCore::activeTransferConnections() const
{
    return m_transferPool->activeConnections();
}

bool SftpUploaderCore::canParallelize() const
{
    return m_transferPool->canParallelize();
}

void SftpUploaderCore::prewarmConnections()
{
    m_transferPool->prewarm();
}

void SftpUploaderCore::setMetadataDir(const QString &dir)
{
    m_metadataDir = dir;
    QDir().mkpath(m_metadataDir);
    QMutexLocker locker(&m_ctx->lock);
    m_ctx->metadataDir = m_metadataDir; // 工作线程读的是这一份
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

// 元数据读写的实现体都是静态的：工作线程要在本对象可能已析构的情况下调用
// 它们（见 WorkerContext），只能靠传进来的目录，不能读成员。
// 公开的同名成员函数是它们在 m_metadataDir 上的薄包装，签名保持不变。

// 对应Python: _get_metadata_path
QString SftpUploaderCore::metadataPathIn(const QString &dir, const QString &fileId)
{
    return QDir(dir).filePath(fileId + QStringLiteral(".json"));
}

QString SftpUploaderCore::metadataPath(const QString &fileId) const
{
    return metadataPathIn(m_metadataDir, fileId);
}

// 对应Python: _save_metadata
bool SftpUploaderCore::saveMetadataIn(const QString &dir, const QString &fileId,
                                      const QString &localPath, const QString &remotePath,
                                      qint64 uploadedSize)
{
    QJsonObject metadata;
    metadata.insert(kMetaFileId, fileId);
    metadata.insert(kMetaLocalPath, localPath);
    metadata.insert(kMetaRemotePath, remotePath);
    metadata.insert(kMetaUploadedSize, double(uploadedSize));
    metadata.insert(kMetaLastModified, pythonMtime(localPath));
    metadata.insert(kMetaTimestamp, double(QDateTime::currentMSecsSinceEpoch()) / 1000.0);

    QFile f(metadataPathIn(dir, fileId));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    // Compact JSON —— Python json.load 可读；字段名/类型与 json.dump 一致。
    f.write(QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    return true;
}

bool SftpUploaderCore::saveMetadata(const QString &fileId, const QString &localPath,
                                    const QString &remotePath, qint64 uploadedSize) const
{
    return saveMetadataIn(m_metadataDir, fileId, localPath, remotePath, uploadedSize);
}

// 对应Python: _load_metadata
bool SftpUploaderCore::loadMetadataIn(const QString &dir, const QString &fileId, QJsonObject &out)
{
    QFile f(metadataPathIn(dir, fileId));
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

bool SftpUploaderCore::loadMetadata(const QString &fileId, QJsonObject &out) const
{
    return loadMetadataIn(m_metadataDir, fileId, out);
}

// 对应Python: _delete_metadata
void SftpUploaderCore::deleteMetadataIn(const QString &metadataDir, const QString &fileId)
{
    QFile::remove(metadataPathIn(metadataDir, fileId));
}

void SftpUploaderCore::deleteMetadata(const QString &fileId) const
{
    deleteMetadataIn(m_metadataDir, fileId);
}

// ---------------------------------------------------------------------------
// 上传 API
// ---------------------------------------------------------------------------

// 对应Python: upload_file
void SftpUploaderCore::uploadFile(const QString &fileId, const QString &localPath,
                                  const QString &remotePath)
{
    CancelFlag cancel = std::make_shared<std::atomic<bool>>(false);
    {
        QMutexLocker locker(&m_ctx->lock);
        m_ctx->cancelFlags.insert(fileId, cancel);
    }
    // QThreadPool 任务队列替代 Python 的每文件 threading.Thread。
    // 按值捕获 ctx（shared_ptr）而不是 this：本对象可能在任务跑完前就析构，
    // 任务全程只用 ctx。
    WorkerContextPtr ctx = m_ctx;
    m_pool->start([ctx, fileId, localPath, remotePath, cancel]() {
        workerUpload(ctx, fileId, localPath, remotePath, cancel);
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
    QMutexLocker locker(&m_ctx->lock);
    auto it = m_ctx->cancelFlags.find(fileId);
    if (it != m_ctx->cancelFlags.end())
        it.value()->store(true);
}

bool SftpUploaderCore::isCancelRequested(const QString &fileId) const
{
    QMutexLocker locker(&m_ctx->lock);
    auto it = m_ctx->cancelFlags.constFind(fileId);
    return it != m_ctx->cancelFlags.constEnd() && it.value()->load();
}

bool SftpUploaderCore::waitForFinished(int msecs)
{
    return m_pool->waitForDone(msecs);
}

// ---------------------------------------------------------------------------
// 工作线程
//
// 以下函数全部是 static：本对象随时可能在它们跑到一半时析构，凡是它们要用的
// 东西都经 ctx 取（见 WorkerContext）。编译器替我们保证这里没有 this 可碰。
// ---------------------------------------------------------------------------

// 把 fn 投回宿主对象线程执行，并把宿主指针交给它（信号得由宿主发）。
// 宿主已析构（owner 为空）就整个丢弃 —— 关闭标签页后本来也没人再关心进度。
//
// 两层保护缺一不可：
//  - 取 owner 时持 ctx->lock，而 ~SftpUploaderCore 置空 owner 也要拿同一把锁，
//    所以拿到的 owner 在 invokeMethod 那一刻必然还活着。
//  - 投递之后宿主若被析构，~QObject 会清掉发给它的待处理事件，排队的调用
//    不会再执行。
template <typename Fn>
void SftpUploaderCore::postToOwner(const WorkerContextPtr &ctx, Fn &&fn)
{
    QMutexLocker locker(&ctx->lock);
    SftpUploaderCore *owner = ctx->owner;
    if (!owner)
        return;
    // 显式 QueuedConnection，信号最终在宿主线程发射。
    QMetaObject::invokeMethod(
        owner, [owner, fn = std::forward<Fn>(fn)]() { fn(owner); }, Qt::QueuedConnection);
}

// 远端路径是否存在。从连接池租一条连接做（元数据操作很轻，随便哪条都行）。
bool SftpUploaderCore::remoteExists(const WorkerContextPtr &ctx, const QString &remotePath)
{
    auto lease = ctx->pool->lease();
    if (!lease.isValid())
        return false;
    return remoteExistsOn(lease, remotePath);
}

// 对应Python: _mkdir_p
bool SftpUploaderCore::remoteMkdirP(const WorkerContextPtr &ctx, const QString &remotePath)
{
    auto lease = ctx->pool->lease();
    if (!lease.isValid())
        return false;
    return remoteMkdirPOn(lease, remotePath);
}

// 建立远端空文件（totalSize == 0）。
bool SftpUploaderCore::createEmptyRemoteFile(const WorkerContextPtr &ctx,
                                             const QString &remotePath, QString &errorOut)
{
    auto lease = ctx->pool->lease(&errorOut);
    if (!lease.isValid())
        return false;
    const QByteArray p = remotePath.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle = sftpCallPtr(lease.lock(), lease.client(), [&] {
        return libssh2_sftp_open_ex(lease.sftp(), p.constData(), quint32(p.size()),
                                    LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                    0644, LIBSSH2_SFTP_OPENFILE);
    });
    if (!handle) {
        errorOut = QStringLiteral("创建远程文件失败: %1").arg(remotePath);
        return false;
    }
    sftpCallInt(lease.lock(), lease.client(),
                [&] { return libssh2_sftp_close_handle(handle); });
    return true;
}

// 在已开好的 handle 上写一个分片（含 kMaxRetries 次重试）。
// 每次重试都重新 seek —— 上一次可能已经写进去一部分，位置是脏的。
// 锁纪律：逐次 libssh2 调用加锁、EAGAIN 锁外等待（见 sftpCallInt）；handle 是
// 本条流独占的（每条流各开一个），调用之间的放锁不会被别人串改文件位置。
bool SftpUploaderCore::writeChunk(LIBSSH2_SFTP_HANDLE *handle, QRecursiveMutex *lock,
                                  SshClient *client, const QByteArray &data,
                                  qint64 offset, QString &errorOut,
                                  const std::atomic<bool> *cancel)
{
    for (int retry = 0; retry < kMaxRetries; ++retry) {
        QString err;
        // seek 只改 handle 本地 offset（libssh2_sftp_seek64 无网络 I/O），
        // 但 session 非线程安全，仍按统一纪律进锁。
        sftpCallInt(lock, client, [&] {
            libssh2_sftp_seek64(handle, quint64(offset));
            return 0;
        }, cancel);
        qint64 written = 0;
        while (written < data.size()) {
            const ssize_t n = sftpCallInt(lock, client, [&] {
                return libssh2_sftp_write(handle, data.constData() + written,
                                          size_t(data.size() - written));
            }, cancel);
            if (n < 0) {
                err = QStringLiteral("sftp write failed (rc=%1) at offset %2")
                          .arg(qint64(n)).arg(offset);
                break;
            }
            written += n;
        }
        if (err.isEmpty())
            return true;
        // 已取消（关闭标签页/退出程序）就不再重试，更不能睡 1 秒：每条流都
        // 白等一遍会让析构多卡好几秒。
        if (cancel && cancel->load()) {
            errorOut = QStringLiteral("上传已取消");
            return false;
        }
        if (retry < kMaxRetries - 1)
            QThread::sleep(1); // 对应Python: time.sleep(1) 后重试
        else
            errorOut = QStringLiteral("上传失败(重试%1次): %2").arg(kMaxRetries).arg(err);
    }
    return false;
}

// 一条并行上传流：租一条独立连接，开一次远端 handle，然后循环领分片。
// 关键点（相对旧实现的提速来源）：
//  - handle 只开一次，不再每个 4MB 分片都 open/close 一轮（1GB 文件省掉 256
//    次多余的协议往返）。
//  - 独占连接上没有别的流竞争这把锁，多条流的 write 真正同时在网络上飞。
//  - 元数据只在连续前缀水位推进时才落盘，不是每片一次 JSON 写。
void SftpUploaderCore::runUploadStream(const WorkerContextPtr &ctx, FileTransferState &state,
                                       const QString &fileId, const QString &localPath,
                                       const QString &remotePath)
{
    const std::atomic<bool> *cancel = state.cancel;
    QString leaseErr;
    // 等空闲槽期间若被取消，lease 带无效租约返回（不算错误，走取消分支）。
    auto lease = ctx->pool->lease(&leaseErr, cancel);
    if (!lease.isValid()) {
        if (cancel && cancel->load())
            return; // 已取消：保留续传元数据，由 workerUpload 走取消分支
        state.setFailed(leaseErr.isEmpty() ? QStringLiteral("SFTP客户端未设置") : leaseErr);
        return;
    }

    // 本地文件每条流各开一个只读句柄（QFile 非线程安全，不能共享）。
    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        state.setFailed(QStringLiteral("本地文件无法读取: %1").arg(localPath));
        return;
    }

    // 打开远端文件：CREAT|WRITE 不带 TRUNC，各流 seek 到自己的分片位置写。
    // 越过 EOF 的写由 SFTP 服务端补零/稀疏化，与 Python 'rb+' 定位写语义一致。
    const QByteArray p = remotePath.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle = sftpCallPtr(lease.lock(), lease.client(), [&] {
        return libssh2_sftp_open_ex(lease.sftp(), p.constData(), quint32(p.size()),
                                    LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                    0644, LIBSSH2_SFTP_OPENFILE);
    }, cancel);
    if (!handle && !(cancel && cancel->load())) {
        // 父目录可能不存在（Python 在此路径下也会补建目录）。复用本条流的租约
        // （而不是再租一条）：连接池对主 session 通道租不到就等，再租会等自己。
        remoteMkdirPOn(lease, remoteDirname(remotePath));
        handle = sftpCallPtr(lease.lock(), lease.client(), [&] {
            return libssh2_sftp_open_ex(lease.sftp(), p.constData(), quint32(p.size()),
                                        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT,
                                        0644, LIBSSH2_SFTP_OPENFILE);
        }, cancel);
    }
    if (!handle) {
        // 取消导致的打开失败不算错误：由 workerUpload 走取消分支保留续传元数据。
        if (!(cancel && cancel->load())) {
            const QString detail = sessionErrorText(lease.client());
            state.setFailed(QStringLiteral("open remote file failed: %1%2")
                                .arg(remotePath, detail.isEmpty()
                                         ? QString() : QStringLiteral(" — ") + detail));
        }
        return;
    }

    const QString filename = baseName(localPath);
    int index = 0;
    qint64 offset = 0, size = 0;
    while (state.takeNext(index, offset, size)) {
        if (!local.seek(offset)) {
            state.setFailed(QStringLiteral("read local chunk failed at offset %1").arg(offset));
            break;
        }
        const QByteArray data = local.read(size);
        if (data.size() != size) {
            state.setFailed(QStringLiteral("read local chunk failed at offset %1").arg(offset));
            break;
        }

        QString chunkErr;
        if (!writeChunk(handle, lease.lock(), lease.client(), data, offset, chunkErr, cancel)) {
            state.setFailed(chunkErr);
            break;
        }

        // 分片完成：推进水位并落一次元数据（水位没动就不写盘）。
        const qint64 before = state.watermark;
        const qint64 watermark = state.markDone(index, data.size());
        if (watermark > before)
            saveMetadataIn(ctx->metadataDir, fileId, localPath, remotePath, watermark);

        // 进度按实际已传字节算（含乱序完成的分片），不是水位。
        const qint64 done = state.progressDone.load();
        const qint64 total = state.totalSize;
        const int progress = int(qMin<qint64>(100, done * 100 / qMax<qint64>(1, total)));
        postToOwner(ctx, [fileId, progress, filename, done, total](SftpUploaderCore *self) {
            emit self->progressUpdated(fileId, progress, filename);
            emit self->progressChanged(fileId, done, total);
        });
    }

    // 收尾关句柄：取消时 sftpCall* 抢锁即带哨兵返回（不再争锁），句柄随连接/
    // 通道关闭一并回收 —— 与原「取消时放弃优雅关闭」语义一致。
    sftpCallInt(lease.lock(), lease.client(),
                [&] { return libssh2_sftp_close_handle(handle); }, cancel);
}

// 对应Python: _upload_file_worker（Python 侧单线程串行；这里并行拆流）
void SftpUploaderCore::workerUpload(const WorkerContextPtr &ctx, const QString &fileId,
                                    const QString &localPath, const QString &remotePath,
                                    CancelFlag cancel)
{
    const QString filename = baseName(localPath);
    const QString metaDir = [&ctx] {
        QMutexLocker locker(&ctx->lock);
        return ctx->metadataDir;
    }();

    auto emitFailed = [&ctx, fileId, filename](const QString &message) {
        postToOwner(ctx, [fileId, filename, message](SftpUploaderCore *self) {
            emit self->uploadFailed(fileId, filename, message);
        });
    };

    do {
        if (cancel->load())
            break; // 入队后未开始就被取消

        if (!QFile::exists(localPath)) {
            emitFailed(QStringLiteral("本地文件不存在: %1").arg(localPath));
            break;
        }

        // 确保远程目录存在。
        const QString remoteDir = remoteDirname(remotePath);
        if (!remoteDir.isEmpty() && !remoteExists(ctx, remoteDir)) {
            if (!remoteMkdirP(ctx, remoteDir)) {
                emitFailed(QStringLiteral("创建远程目录失败: %1").arg(remoteDir));
                break;
            }
        }

        const qint64 totalSize = QFileInfo(localPath).size();

        postToOwner(ctx, [fileId, filename, totalSize](SftpUploaderCore *self) {
            emit self->uploadStarted(fileId, filename, totalSize);
        });

        // 空文件：无分片可传，直接创建远端空文件并报完成。
        if (totalSize == 0) {
            if (cancel->load())
                break;
            QString err;
            if (!createEmptyRemoteFile(ctx, remotePath, err)) {
                emitFailed(err);
                break;
            }
            deleteMetadataIn(metaDir, fileId);
            postToOwner(ctx, [fileId, filename](SftpUploaderCore *self) {
                emit self->uploadCompleted(fileId, filename);
            });
            break;
        }

        // 断点续传：元数据有效且 local/remote 一致时从 uploaded_size 续传。
        qint64 startOffset = 0;
        QJsonObject metadata;
        if (loadMetadataIn(metaDir, fileId, metadata)
            && metadata.value(kMetaLocalPath).toString() == localPath
            && metadata.value(kMetaRemotePath).toString() == remotePath
            && metadata.contains(kMetaUploadedSize)) {
            startOffset = qint64(metadata.value(kMetaUploadedSize).toDouble());
        }

        FileTransferState state;
        state.chunks = planChunks(totalSize, startOffset);
        state.done.fill(false, state.chunks.size());
        state.baseOffset = startOffset;
        state.watermark = startOffset;
        state.totalSize = totalSize;
        state.progressDone.store(startOffset);
        state.cancel = cancel.get();

        // 并行度：分片数、单文件流上限、连接池实际能给的连接数，取最小。
        // 小文件（< kMinSizeForMultiStream）不拆流——握手成本盖过收益。
        int streams = 1;
        if (totalSize >= kMinSizeForMultiStream && ctx->pool->canParallelize()) {
            streams = qMin(kMaxStreamsPerFile, int(state.chunks.size()));
            streams = qMax(1, qMin(streams, ctx->pool->maxConnections()));
        }

        if (streams <= 1) {
            runUploadStream(ctx, state, fileId, localPath, remotePath);
        } else {
            // 额外流放到独立线程；当前线程也干活（少开一个线程）。
            QVector<QThread *> helpers;
            helpers.reserve(streams - 1);
            for (int i = 1; i < streams; ++i) {
                QThread *t = QThread::create([&ctx, &state, fileId, localPath, remotePath] {
                    runUploadStream(ctx, state, fileId, localPath, remotePath);
                });
                helpers.append(t);
                t->start();
            }
            runUploadStream(ctx, state, fileId, localPath, remotePath);
            // state 是栈对象，必须等所有流退出后才能离开作用域。
            for (QThread *t : helpers) {
                t->wait();
                delete t;
            }
        }

        if (cancel->load())
            break; // 取消：保留元数据供下次续传

        if (state.hasFailed()) {
            emitFailed(state.error);
            break;
        }

        deleteMetadataIn(metaDir, fileId);
        postToOwner(ctx, [fileId, filename](SftpUploaderCore *self) {
            emit self->uploadCompleted(fileId, filename);
        });
    } while (false);

    // 清理线程资源。对应Python: finally 中删除 upload_threads/stop_events。
    // 注意这里动的是 ctx 而不是本对象的成员：本对象可能早就析构了。
    QMutexLocker locker(&ctx->lock);
    ctx->cancelFlags.remove(fileId);
}

} // namespace cubeshell
