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

struct _LIBSSH2_SFTP;
struct _LIBSSH2_SFTP_HANDLE;

class QThread;

namespace cubeshell {

class SshClient;
struct SshError;

// One directory entry, mirroring paramiko's SFTPAttributes (the subset the UI
// consumes: name, size, mode, mtime, plus directory/symlink classification).
// 对应 paramiko.SFTPAttributes
struct SftpFileInfo {
    QString filename;      // entry name (no path)
    QString longname;      // "ls -l" style long entry (from readdir_ex)
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

signals:
    void operationFailed(const QString &op, const QString &path, const QString &message);

    // Transfer progress: bytesTransferred of bytesTotal (bytesTotal may be 0 if
    // unknown). Mirrors util progress_callback(current, total).
    void transferProgress(const QString &path, qint64 bytesTransferred, qint64 bytesTotal);
    void transferFinished(const QString &path, bool success, const QString &message);

private:
    bool ensureOpen(SshError &error);
    // 登记并启动 download/upload 的自建 worker 线程（finished→deleteLater，
    // QPointer 随删除自动置空），供析构函数 cancelTransfer 后统一
    // quit()+有限 wait()，避免后台线程访问已删除的 this/m_sftp。
    void startWorker(QThread *worker);
    void doDownload(const QString &remotePath, const QString &localPath);
    void doUpload(const QString &localPath, const QString &remotePath);
    void fail(const QString &op, const QString &path, SshError &error);

    SshClient *m_client;         // not owned
    _LIBSSH2_SFTP *m_sftp = nullptr;
    // Set from the UI thread, read on workers — checked by the transfer chunk
    // loops AND the EAGAIN retry loops of the synchronous operations, so
    // cancelTransfer() also aborts a listdir/read/write stuck on retries.
    std::atomic<bool> m_cancel{false};
    // 仍可能在运行的 download/upload worker 登记表（元素被 deleteLater 后自动置空）。
    QVector<QPointer<QThread>> m_workers;
};

} // namespace cubeshell

// Register the info-list type for queued signal delivery across threads.
Q_DECLARE_METATYPE(cubeshell::SftpFileInfo)
Q_DECLARE_METATYPE(cubeshell::SftpFileInfoList)
