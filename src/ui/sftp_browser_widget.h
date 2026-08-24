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
#include <QQueue>
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
class QToolButton;
class QLabel;
class QResizeEvent;
class QDropEvent;

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

    // 批量下载的选中项拆分：把 (远端路径, 是否目录) 列表分成文件/目录两组
    // （目录不支持下载，由调用方收集后统一提示，不阻塞文件下载）。
    // 抽成静态纯函数便于单测。对应Python: downloadFile 对 is_dir 的过滤
    static void partitionDownloadSelection(const QList<QPair<QString, bool>> &selected,
                                           QStringList &files, QStringList &dirs);
    // 批量下载的目标本地路径：保存目录 + 远端文件名。抽成静态纯函数便于单测。
    static QString downloadTargetPath(const QString &dir, const QString &remotePath);

    // 拖放上传的目标目录：落在目录条目上进该目录（".." 条目的路径即上级目录，
    // 天然覆盖），其余落点（空白/文件条目）进当前目录。纯数据签名便于单测。
    static QString dropTargetDir(const QString &itemPath, bool itemIsDir,
                                 const QString &cwd);
    // 把拖入的本地路径展开成 (本地文件, 远端路径) 任务对：文件直接映射；
    // 文件夹递归遍历，远端 = 目标目录 + 顶层文件夹名 + 相对路径（远端父目录
    // 由 SftpUploaderCore 上传时自动补建）。抽成静态纯函数便于单测。
    static QList<QPair<QString, QString>> collectUploadTasks(const QString &targetDir,
                                                             const QStringList &localPaths);

    // 在路径栏左侧显示/隐藏分屏序号徽章（多分屏时避免混淆 SFTP 目录归属）。
    // paneNumber: 当前分屏序号（1-based）；totalPanes: 总分屏数（≤1 时隐藏徽章）；
    // tabTitle: 用于 tooltip 的标签名，展示完整的“分屏 N · 标签名”。
    void setPaneIndicator(int paneNumber, int totalPanes, const QString &tabTitle);

protected:
    // 宽度变化后按新宽度重新截断状态栏文本。
    void resizeEvent(QResizeEvent *event) override;
    // 文件树 viewport 的拖放事件（拖拽上传）：视图不接受自己的内部拖放，
    // 外部 URL 拖放经此过滤进 handleDropEvent。
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void goUp();
    void refresh();
    void mkdir();
    void removeSelected();
    void uploadFiles();
    void downloadSelected();
    void onPathEdited();
    // 表头点击排序：同列切换升降序，换列重置为升序。
    void onHeaderSectionClicked(int logical);
    // 筛选栏显隐（路径栏右侧筛选按钮 toggled）。打开即聚焦，关闭即清空还原。
    void setFilterBarVisible(bool on);
    // 按 m_filterEdit 当前文本重筛整棵树（textChanged 驱动）。
    void applyFilter();
    // 检索框回车：选中并滚动到第一个可见匹配项。
    void focusFirstFilterMatch();
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
    // 写底部状态栏：按 label 当前宽度做省略号截断，完整文本放 tooltip，
    // 避免长错误信息把面板撑宽。所有状态输出统一走这里，不要直接 setText。
    void setStatusText(const QString &text);
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

    // 批量下载：多选入队、串行逐文件下载。SftpClient 的 m_cancel 是全局共享
    // 单标志且每个 download() 入口都会复位它，并发多个下载 worker 下取消语义
    // 是坏的；单个大文件内部已有多流并行，文件间串行即可。
    // 取消语义：cancelTransfer() 取消当前在传文件，队列由 UI 侧 clear。
    struct DownloadTask { QString remote; QString local; };
    QQueue<DownloadTask> m_downloadQueue;
    // 在传下载的进度记账（按远端路径），与上传侧 m_activeUploads 同款聚合。
    QHash<QString, UploadProgress> m_activeDownloads;
    // 本批已完成的文件数与失败清单，队列排空后一次性汇总汇报。
    int m_downloadBatchDone = 0;
    QStringList m_downloadFailures;
    void dispatchNextDownload();    // 队列非空则取出首个任务点火
    void refreshDownloadProgress(); // 重算聚合进度并刷新进度条/状态栏
    // 拖放上传的处理（eventFilter 的 Drop 分支）。
    void handleDropEvent(QDropEvent *event);
    // 单个上传任务入队（进度记账 + 分发到线程池），uploadFiles/拖拽共用。
    void enqueueUpload(const QString &local, const QString &remote);
    // 取消全部在传传输：下载清空批量队列 + cancelTransfer，上传逐文件
    // cancelUpload。已传部分保留（断点续传），重新传输自动继续。
    void cancelTransfers();
    // 有在传传输时显示取消按钮，空闲时隐藏。所有改变 m_activeUploads /
    // m_activeDownloads / m_downloadQueue 的路径末尾都要调。
    void updateCancelButton();
    // 已请求取消、但在传任务还没收敛完。取消不是瞬时的（上传工作线程只在
    // 4MB 分片边界看标志），这段窗口里按钮置灰防重复点击；在传集合排空时
    // 自动复位，新起一轮传输也复位（见 enqueueUpload / dispatchNextDownload）。
    bool m_cancelPending = false;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_paneBadge = nullptr;     // 路径栏左侧的分屏序号圆角徽章
    QToolButton *m_filterBtn = nullptr; // 路径栏右侧的筛选开关（弹出/收起检索框）
    QWidget *m_filterBar = nullptr;    // 检索框所在行（默认隐藏）
    QLineEdit *m_filterEdit = nullptr;
    QTreeWidget *m_tree = nullptr;
    QProgressBar *m_progress = nullptr;
    QToolButton *m_cancelBtn = nullptr;
    QLabel *m_status = nullptr;
    // 状态栏完整文本（未截断），resize 后据此重新截断。
    QString m_statusFullText;

    QString m_cwd = QStringLiteral("/");
    // 最近一次成功加载的目录条目数（不含 ".."）：状态栏在筛选开关间还原计数用。
    int m_entryCount = 0;
    // 表头排序状态（跨目录保持；默认按文件名升序，与原 ls 风格预排序一致）。
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
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
