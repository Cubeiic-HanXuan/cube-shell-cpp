#pragma once

// SftpBrowserWidget.h — remote file browser panel (SFTP).
//
// A flat list of the remote working directory backed by SftpClient over an
// already-open SshClient, mirroring the Python left-panel file tree
// (cube-shell.py::handle_file_tree_updated): path bar on top, ".." row to go
// up, double-click a directory to descend, all operations on the context
// menu. Listing runs on a worker thread so the UI stays responsive (the
// SftpClient serializes libssh2 internally).

#include <QHash>
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
    // 以 shared_ptr 持有，保证应用关闭时 SshClient（及其底层 LIBSSH2_SESSION）
    // 比子对象 SftpClient 活得久——否则 terminal 先析构会 libssh2_session_free，
    // SftpClient::close() 再用悬空的 m_sftp 调 libssh2_sftp_shutdown → UAF 崩溃。
    void setClient(std::shared_ptr<SshClient> client);

    // 切换当前目录（follow_folder 联动 / OSC7 初始 home 目录）。
    // 客户端尚未挂接时仅记录路径，setClient 时再加载。
    // 对应Python: cube-shell.py::_on_cwd_changed → refreshDirs
    void setCurrentPath(const QString &path);
    QString currentPath() const { return m_cwd; }

    // 标记该会话是经跳板机（JumpServer/koko）代理建立的。
    // 代理端的 SFTP 子系统是一套与资产无关的虚拟命名空间：token 会话下根目录
    // 为空、资产上的任何绝对路径都打不开。有了这个标记才能在面板上给出准确
    // 说明，而不是把 libssh2 的原始报错糊给用户。
    // 对应Python: cube-shell.py 的 ssh_conn.is_jumpserver_proxy 标记
    void setBastionProxied(bool on) { m_bastionProxied = on; }

    // 「代理端不提供文件浏览」的判定。抽成静态纯函数便于单测。
    // 命中条件：经代理 + 连根目录都列不出东西 ⇒ 这条通道没有文件系统可给。
    // 只在 path 为根时判定：非根路径列不出来可能只是权限或路径不存在，
    // 不足以推断整条通道不可用。
    static bool sftpLooksUnavailable(bool bastionProxied, const QString &path, int entryCount);

    // 在路径栏左侧显示/隐藏分屏序号徽章（多分屏时避免混淆 SFTP 目录归属）。
    // paneNumber: 当前分屏序号（1-based）；totalPanes: 总分屏数（≤1 时隐藏徽章）；
    // tabTitle: 用于 tooltip 的标签名，展示完整的“分屏 N · 标签名”。
    void setPaneIndicator(int paneNumber, int totalPanes, const QString &tabTitle);

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
    // 目录加载的触发来源，决定失败时怎么表现。
    //  UserRequested — 用户主动导航（双击/路径栏/刷新/上一级）：失败必须报错，
    //                  否则用户不知道自己点的地方去不了。
    //  CwdSync       — 终端 OSC 7 上报 cwd 的联动：这是我们替用户猜的，猜错了
    //                  不能弹错误。终端 cwd 与 SFTP 通道可能压根不是同一个
    //                  命名空间（跳板机代理、chroot 过的 sftp 子系统都会这样）。
    //  Initial       — setClient 的首次加载，同样属于推测（m_cwd 可能是连接
    //                  就绪前由 setCurrentPath 种进来的终端 cwd）。
    enum class LoadReason { UserRequested, CwdSync, Initial };
    void loadPath(const QString &path, LoadReason reason = LoadReason::UserRequested);
    // 清空列表并显示「代理端不提供 SFTP 文件浏览」的说明行 + 状态栏文案。
    // 对应Python: cube-shell.py::_on_file_tree_unavailable（清空树 + 提示）
    void showUnavailableNotice();
    // 写操作闸门：处于不可用态时给出统一说明并返回 true（调用方应直接 return），
    // 免得用户点上传/新建各撞一次 libssh2 原始错误。
    bool blockedByUnavailable(const QString &title);
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

    std::shared_ptr<SshClient> m_client; // shared：保证比 m_sftp 后析构（见 setClient）
    SftpClient *m_sftp = nullptr;    // child
    SftpUploaderCore *m_uploader = nullptr; // child，分片上传

    // 并行上传的进度聚合：多个文件同时传时，各自的 progressChanged 会争抢同
    // 一个进度条，单看最后一个信号会让进度条来回跳。这里按 fileId 记账，
    // 进度条显示所有在传文件的字节总和。
    struct UploadProgress {
        qint64 done = 0;
        qint64 total = 0;
    };
    QHash<QString, UploadProgress> m_activeUploads;
    void refreshUploadProgress(); // 重算聚合进度并刷新进度条/状态栏

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_paneBadge = nullptr;     // 路径栏左侧的分屏序号圆角徽章
    QTreeWidget *m_tree = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;

    QString m_cwd = QStringLiteral("/");
    // 该会话经跳板机代理（见 setBastionProxied）。
    bool m_bastionProxied = false;
    // 已判定「代理端不提供文件浏览」，列表里现在是说明行而非目录内容。
    bool m_sftpUnavailable = false;
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
