#pragma once

// SftpBrowserWidget.h — remote file browser panel (SFTP).
//
// A flat list of the remote working directory backed by SftpClient over an
// already-open SshClient, mirroring the Python left-panel file tree
// (cube-shell.py::handle_file_tree_updated): path bar on top, ".." row to go
// up, double-click a directory to descend, all operations on the context
// menu. Listing runs on a worker thread so the UI stays responsive (the
// SftpClient serializes libssh2 internally).

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>

#include "ssh/SftpClient.h"

class QThread;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QProgressBar;
class QLabel;

namespace cubeshell {

class SshClient;
class SftpUploaderCore;

class SftpBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit SftpBrowserWidget(QWidget *parent = nullptr);
    ~SftpBrowserWidget() override;

    // Attach to a connected client and load the home/root directory.
    void setClient(SshClient *client);

    // 切换当前目录（follow_folder 联动 / OSC7 初始 home 目录）。
    // 客户端尚未挂接时仅记录路径，setClient 时再加载。
    // 对应Python: cube-shell.py::_on_cwd_changed → refreshDirs
    void setCurrentPath(const QString &path);
    QString currentPath() const { return m_cwd; }

private slots:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void goUp();
    void refresh();
    void mkdir();
    void removeSelected();
    void uploadFiles();
    void downloadSelected();
    void onPathEdited();
    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接分支）
    void showContextMenu(const QPoint &pos);
    void renameSelected();
    void editSelected();
    void compressSelected();
    // 对应Python: cube-shell.py::createFile
    void createFileHere();
    // 权限（chmod）对话框。对应Python: cube-shell.py::show_auth + Auth.ok_auth
    void showPermissions();
    // 对应Python: cube-shell.py::unzip（DecompressThread）
    void decompressSelected();

private:
    void loadPath(const QString &path);
    // 登记并启动自建 worker 线程（finished→deleteLater，QPointer 随删除自动置空），
    // 供析构函数统一 quit()+wait()，避免后台线程访问已删除的 m_sftp 等子对象。
    // 经此启动的线程析构时会被有限等待，超时将泄漏其引用对象，
    // 避免用于不可中断的超长任务。
    void startWorker(QThread *worker);
    void populate(const QString &path, const SftpFileInfoList &entries);
    QString selectedRemotePath() const;
    // 全部选中项的远端路径（已剔除 ".." 条目），供删除等批量操作使用。
    QStringList selectedRemotePaths() const;
    static QString joinPath(const QString &dir, const QString &name);

    SshClient *m_client = nullptr;   // not owned
    SftpClient *m_sftp = nullptr;    // child
    SftpUploaderCore *m_uploader = nullptr; // child，分片上传

    QLineEdit *m_pathEdit = nullptr;
    QTreeWidget *m_tree = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;

    QString m_cwd = QStringLiteral("/");
    // 目录加载请求序号：回调仅接受最新请求的结果，防止乱序返回的
    // 陈旧目录数据覆盖新目录（快速连续双击多个文件夹时）。
    quint64 m_loadSeq = 0;
    // 仍可能在运行的自建 worker 线程登记表（元素被 deleteLater 后自动置空）。
    QVector<QPointer<QThread>> m_workers;
    // 关停标志：析构最先置位，压缩/解压 worker 以共享指针捕获后传给
    // CommandExecutor::runCommand 作取消标志（shared_ptr 保证 widget 析构后
    // 泄漏的超时线程读到的标志仍有效）。
    std::shared_ptr<std::atomic<bool>> m_shuttingDown =
        std::make_shared<std::atomic<bool>>(false);
};

} // namespace cubeshell
