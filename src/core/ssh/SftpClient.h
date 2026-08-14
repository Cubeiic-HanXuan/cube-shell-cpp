#pragma once

// SftpClient.h — libssh2-backed SFTP client.
//
// C++ replacement for the SFTP subsystem cube-shell uses from
// function/ssh_func.py (open_sftp) plus the file operations the file-tree,
// upload, download, docker-compose-editor and hermes features call:
//
//   listdir / listdir_attr / stat / lstat / mkdir / remove(unlink) / rmdir /
//   rename / chmod / open+read+write (get/put) / read whole remote file.
//
// Mapping notes:
//  - paramiko's sftp.stat()           -> stat()      (follows symlinks)
//  - paramiko's sftp.listdir()        -> listdir()   (names only)
//  - paramiko's sftp.listdir_attr()   -> listdirAttr()
//  - paramiko's sftp.file(path,'r'/'w'/'ab') reads/writes -> get()/put() and
//    readFile()/writeFile()
//  - util.deleteFolder recursion      -> removeRecursive()
//  - util.download_with_resume / resume_upload -> download()/upload() with
//    resume support (offset continuation), driven asynchronously on a QThread.
//
// Threading: the SFTP session multiplexes over the same LIBSSH2_SESSION as the
// interactive shell channel owned by SshClient. libssh2 is NOT thread-safe on a
// shared session, so EVERY libssh2 SFTP call is serialized through
// SshClient::sessionLock(). Blocking transfers (upload/download) run on a
// QThread worker and report progress/results back via queued Qt signals so the
// UI thread stays responsive (mirrors Python's refresh_thread / upload worker
// threads).

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

struct _LIBSSH2_SFTP;
struct _LIBSSH2_SFTP_HANDLE;

class QThread;
class QRecursiveMutex;

namespace cubeshell {

class SshClient;
class SftpTransferPool;
struct SshError;

// One directory entry, mirroring paramiko's SFTPAttributes (the subset the UI
// consumes: name, size, mode, mtime, plus directory/symlink classification).
// 对应 paramiko.SFTPAttributes
struct SftpFileInfo {
    QString filename;      // entry name (no path)
    QString longname;      // "ls -l" style long entry (from readdir_ex)
    QString symlinkTarget; // symlink target parsed from longname (empty otherwise)
    qint64  size = 0;      // st_size
    quint32 mode = 0;      // st_mode (permissions + type bits)
    qint64  mtime = 0;     // st_mtime (seconds since epoch)
    qint64  atime = 0;     // st_atime
    quint32 uid = 0;
    quint32 gid = 0;

    bool isValid() const { return !filename.isEmpty(); }
    bool isDirectory() const;   // S_ISDIR(mode)
    bool isSymlink() const;     // S_ISLNK(mode)
    bool isRegular() const;     // S_ISREG(mode)
    // Permission bits only (low 12 bits), e.g. 0755.
    quint32 permissions() const { return mode & 07777; }
};

using SftpFileInfoList = QVector<SftpFileInfo>;

// 对应 paramiko.SFTPClient (the cube-shell subset)
class SftpClient : public QObject {
    Q_OBJECT
public:
    // client: a connected SshClient. SftpClient does NOT own it; it must
    // outlive this object. The SFTP channel is opened lazily on first use.
    explicit SftpClient(SshClient *client, QObject *parent = nullptr);
    ~SftpClient() override;

    // Open the SFTP subsystem channel. Called lazily; explicit open() lets the
    // caller surface errors early (mirrors ssh_func.open_sftp()).
    bool open(SshError &error);
    bool isOpen() const { return m_sftp != nullptr; }
    void close();
    // 放弃 SFTP 句柄：不做 libssh2_sftp_shutdown 网络往返，仅清本地指针。
    // 用于底层 socket 已被 shutdownSocket() 关闭之后（标签页/应用退出路径）：
    // 此时再 shutdown 会往死 socket 写 → libssh2 内部崩溃（EXC_BAD_ACCESS）。
    // 句柄资源随 SshClient 析构时 libssh2_session_free 一并回收，不会泄漏。
    void abandon();

    // --- synchronous metadata / directory operations (caller serializes via
    // the UI or a worker thread; the libssh2 lock is taken internally) ---

    // Names only (paramiko listdir). Returns entry names, no "." / "..".
    QStringList listdir(const QString &path, SshError &error);
    // Full attributes (paramiko listdir_attr). Excludes "." / "..".
    SftpFileInfoList listdirAttr(const QString &path, SshError &error);

    // stat follows symlinks; lstat does not. Fills info; returns false+error
    // when the path does not exist (error.code == LIBSSH2_FX_NO_SUCH_FILE).
    bool stat(const QString &path, SftpFileInfo &info, SshError &error);
    bool lstat(const QString &path, SftpFileInfo &info, SshError &error);
    // Convenience: true when stat() succeeds (util.check_dir_exists).
    bool exists(const QString &path);

    bool mkdir(const QString &path, int mode = 0777, SshError *error = nullptr);
    // Recursive mkdir -p (sftp_uploader_core._mkdir_p).
    bool mkdirP(const QString &path, int mode = 0777, SshError *error = nullptr);
    bool rmdir(const QString &path, SshError *error = nullptr);
    // Remove a single file (paramiko remove / unlink).
    bool remove(const QString &path, SshError *error = nullptr);
    // Recursive delete of a file or directory tree (util.deleteFolder).
    bool removeRecursive(const QString &path, SshError *error = nullptr);
    bool rename(const QString &oldPath, const QString &newPath, SshError *error = nullptr);
    bool chmod(const QString &path, int mode, SshError *error = nullptr);

    // Read an entire remote file into memory (hermes read_file /
    // docker-compose editor). Decoding to QString is left to the caller.
    QByteArray readFile(const QString &remotePath, SshError &error);
    // Write bytes to a remote file (truncate/create). hermes write_file.
    bool writeFile(const QString &remotePath, const QByteArray &data, SshError &error);

    // --- asynchronous transfers (run on a worker QThread) ---
    // Download a remote file to a local path with resume support
    // (util.download_with_resume). If the local file exists and is smaller,
    // the transfer continues from the local size.
    void download(const QString &remotePath, const QString &localPath);
    // Upload a local file to a remote path with resume support
    // (util.resume_upload). Continues from the existing remote size.
    void upload(const QString &localPath, const QString &remotePath);
    // Request cancellation of the in-flight transfer.
    void cancelTransfer();
    // 析构/关标签页路径的停机信号：取消在传传输 + 让同步元数据操作
    // （listdir/stat/readFile 等）的 EAGAIN 重试循环尽快退出。
    // 与 cancelTransfer 的区别：用户点"取消"只停传输，面板还要继续浏览，
    // 同步操作不能跟着失效（否则取消一次下载后 listdir 永远报 Would block）。
    void beginTeardown();
    // 有限等待所有在传 download/upload worker 结束；false = 有超时逃逸，
    // 由调用方决定兜底（析构走泄漏，见 .cpp）。供 ~SftpBrowserWidget 在
    // ~QObject 删子对象前调用——传输 worker 捕获裸 this，没退干净就不能
    // 让本对象析构。
    bool joinTransferWorkers(int timeoutMs);

signals:
    void operationFailed(const QString &op, const QString &path, const QString &message);

    // Transfer progress: bytesTransferred of bytesTotal (bytesTotal may be 0 if
    // unknown). Mirrors util progress_callback(current, total).
    void transferProgress(const QString &path, qint64 bytesTransferred, qint64 bytesTotal);
    void transferFinished(const QString &path, bool success, const QString &message);

private:
    // 一次并行传输的共享状态（工作窃取式分片分配）。定义在 .cpp 里。
    struct TransferState;

    bool ensureOpen(SshError &error);
    // 登记并启动 download/upload 的自建 worker 线程（finished→deleteLater，
    // QPointer 随删除自动置空），供析构函数 cancelTransfer 后统一
    // quit()+有限 wait()，避免后台线程访问已删除的 this/m_sftp。
    void startWorker(QThread *worker);
    // 是否还有在传的 download/upload worker（决定 download/upload 入口能否
    // 安全复位共享的 m_cancel）。
    bool hasActiveTransfer() const;
    void doDownload(const QString &remotePath, const QString &localPath);
    void doUpload(const QString &localPath, const QString &remotePath);
    void fail(const QString &op, const QString &path, SshError &error);

    // --- 并行传输（见 .cpp 顶部的分片/流数常量） ---
    // 按文件大小和连接池实际能给的连接数决定开几条流。
    int planStreamCount(qint64 size) const;
    // 跑 streams 条流并等全部结束（当前线程也算一条）。
    void runStreams(int streams, const std::function<void()> &body);
    void downloadStream(TransferState &state, const QString &remotePath,
                        const QString &localPath);
    void uploadStream(TransferState &state, const QString &localPath,
                      const QString &remotePath);

    SshClient *m_client;         // not owned
    _LIBSSH2_SFTP *m_sftp = nullptr;
    // 并行传输连接池：为 download/upload 另开独立 SSH 连接，避免与交互 shell
    // 抢主 session（旧实现下载大文件时会把终端卡到传完）。见 SftpTransferPool.h。
    std::unique_ptr<SftpTransferPool> m_transferPool;
    // socket 已关（abandon() 置位）后，close() 只清指针、不做网络往返。
    bool m_abandoned = false;
    // 传输取消标志：cancelTransfer() 置位，download/upload worker 的分块循环、
    // EAGAIN 重试循环与连接池等槽都检查它。用户可主动触发（取消按钮），
    // 下一次 download/upload 入口在无在传 worker 时复位。
    std::atomic<bool> m_cancel{false};
    // 停机标志：仅 beginTeardown()（析构/关标签页）置位，永不复位。
    // 同步元数据操作的 EAGAIN 重试循环只看它——用户取消传输不该让面板
    // 后续的 listdir/opendir 跟着失败（会报 Would block）。
    std::atomic<bool> m_teardown{false};
    // 仍可能在运行的 download/upload worker 登记表（元素被 deleteLater 后自动置空）。
    QVector<QPointer<QThread>> m_workers;
};

} // namespace cubeshell

// Register the info-list type for queued signal delivery across threads.
Q_DECLARE_METATYPE(cubeshell::SftpFileInfo)
Q_DECLARE_METATYPE(cubeshell::SftpFileInfoList)
