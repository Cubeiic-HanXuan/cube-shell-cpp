#include "sftp_browser_widget.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QStyle>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "ssh/SshClient.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SftpUploaderCore.h"
#include "dialogs/CompressDialog.h"
#include "editors/TextEditor.h"
#include "file_icons.h"

namespace cubeshell {

// 析构时对每个存活 worker 的最长等待；超时则泄漏其引用对象而非无限阻塞 UI。
static constexpr int kWorkerJoinTimeoutMs = 5000;

// Roles on tree items.
static constexpr int kPathRole = Qt::UserRole;       // full remote path
static constexpr int kIsDirRole = Qt::UserRole + 1;  // bool
static constexpr int kModeRole = Qt::UserRole + 2;   // permission bits (quint32)
static constexpr int kIsUpRole = Qt::UserRole + 3;   // ".." 返回上级条目
static constexpr int kSymlinkTargetRole = Qt::UserRole + 4; // 符号链接目标（QString）

// 文件大小人类可读格式。对应Python: function/util.py::format_file_size
static QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 字节").arg(bytes);
    if (bytes < 1024LL * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 3);
}

// ls 风格权限串（类型位 + rwx 九位），优先取 readdir longname 首列（与远端 ls 完全一致）。
// 对应Python: handle_file_tree_updated 里 item.setText(3, n[0])
static QString permissionText(const SftpFileInfo &e)
{
    const QStringList parts = e.longname.simplified().split(QLatin1Char(' '));
    if (!parts.isEmpty() && parts.first().size() >= 10)
        return parts.first();
    QString s(e.isDirectory() ? QLatin1Char('d')
                              : e.isSymlink() ? QLatin1Char('l') : QLatin1Char('-'));
    static const char bits[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        s += (e.mode & (1u << (8 - i))) ? QLatin1Char(bits[i]) : QLatin1Char('-');
    return s;
}

// 所有者/组列：longname 第 3 列为属主名，取不到时退回 uid 数字。
// 对应Python: handle_file_tree_updated 里 item.setText(4, n[3])（仅属主，不含组）
static QString ownerText(const SftpFileInfo &e)
{
    const QStringList parts = e.longname.simplified().split(QLatin1Char(' '));
    if (parts.size() >= 3)
        return parts.at(2);
    return QString::number(e.uid);
}

SftpBrowserWidget::SftpBrowserWidget(QWidget *parent)
    : QWidget(parent)
{
    // 顶部仅保留路径栏，所有文件操作走右键菜单。
    // 对应Python: add_line_edit(pwd) 在文件树顶部展示当前目录
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setText(m_cwd);

    // 平铺列表（非展开树），五列表头与 Python 侧一致。
    // 对应Python: handle_file_tree_updated 里 setRootIsDecorated(False)/setIndentation(0)
    // 及 setHeaderLabels([文件名, 文件大小, 修改日期, 权限, 所有者/组])
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("文件名"), tr("文件大小"), tr("修改日期"),
                             tr("权限"), tr("所有者/组")});
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(0);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QTreeWidget::ExtendedSelection);
    // 列宽可拖动、总宽超出面板时出现水平滚动条。
    // 对应Python: QTreeWidget 默认 Interactive 表头 + header.setSectionsMovable(True)
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setSectionsMovable(true);
    m_tree->setColumnWidth(0, 200);
    m_tree->setColumnWidth(1, 90);
    m_tree->setColumnWidth(2, 130);
    m_tree->setColumnWidth(3, 100);
    m_tree->setColumnWidth(4, 110);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: gray;"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);
    layout->addWidget(m_pathEdit);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &SftpBrowserWidget::onPathEdited);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &SftpBrowserWidget::onItemDoubleClicked);

    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接分支）
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &SftpBrowserWidget::showContextMenu);
}

SftpBrowserWidget::~SftpBrowserWidget()
{
    // 取消协作：先置关停标志（压缩/解压的 runCommand 在下一轮读循环退出），
    // 再 cancelTransfer（SftpClient 同步操作的 EAGAIN 重试循环与传输分块
    // 循环都检查 m_cancel，listdir/read/write 等能尽快退出）。
    m_shuttingDown->store(true);
    if (m_sftp)
        m_sftp->cancelTransfer();
    // 有限等待：先于子对象（m_sftp 等）被 ~QObject 删除前，等待仍在运行的
    // 自建 worker 线程结束，避免后台线程对已删除 SftpClient 的 use-after-free。
    // 已正常结束的线程经 deleteLater 销毁后 QPointer 自动置空，此处跳过。
    bool joinTimedOut = false;
    for (const QPointer<QThread> &worker : std::as_const(m_workers)) {
        if (worker) {
            worker->quit();
            if (!worker->wait(kWorkerJoinTimeoutMs)) {
                qWarning() << "SftpBrowserWidget: worker thread did not finish within"
                           << kWorkerJoinTimeoutMs << "ms, leaking its referenced objects";
                joinTimedOut = true;
            }
        }
    }
    m_workers.clear();
    // 超时兑底：线程仍在用 m_sftp，而析构返回后 ~QObject 会删除子对象。
    // 把 m_sftp 脱离父子关系有意泄漏，病态场景宁可漏少量内存也不 UAF 崩溃。
    //（m_client 非本 widget 所有，不受 ~QObject 影响，无需处理。）
    if (joinTimedOut && m_sftp)
        m_sftp->setParent(nullptr);
}

// 登记并启动自建 worker 线程：finished 后 deleteLater，QPointer 随删除自动置空；
// 启动前顺手清理登记表中已置空的条目，避免长期会话下无限增长。
void SftpBrowserWidget::startWorker(QThread *worker)
{
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    m_workers.removeAll(QPointer<QThread>());
    m_workers.append(QPointer<QThread>(worker));
    worker->start();
}

void SftpBrowserWidget::setClient(SshClient *client)
{
    m_client = client;
    if (!m_client)
        return;
    if (!m_sftp) {
        m_sftp = new SftpClient(m_client, this);
        connect(m_sftp, &SftpClient::transferProgress, this, [this](const QString &, qint64 cur, qint64 total) {
            m_progress->setVisible(true);
            m_progress->setMaximum(total > 0 ? int(total / 1024) : 0);
            m_progress->setValue(int(cur / 1024));
        });
        connect(m_sftp, &SftpClient::transferFinished, this, [this](const QString &path, bool ok, const QString &msg) {
            m_progress->setVisible(false);
            m_status->setText(ok ? tr("已完成：%1").arg(QFileInfo(path).fileName())
                                 : tr("传输失败：%1").arg(msg));
            if (ok)
                refresh();
        });
        connect(m_sftp, &SftpClient::operationFailed, this, [this](const QString &op, const QString &, const QString &msg) {
            m_status->setText(tr("%1 失败：%2").arg(op, msg));
        });
    }
    if (!m_uploader) {
        // 分片上传核心（进度信号已在内部回投到本线程，按工程约定仍显式 QueuedConnection）。
        // 对应Python: core/uploader/sftp_uploader_core.py 的进度信号接线
        m_uploader = new SftpUploaderCore(m_client, this);
        connect(m_uploader, &SftpUploaderCore::progressUpdated, this,
                [this](const QString &, int progress, const QString &filename) {
                    m_progress->setVisible(true);
                    m_progress->setMaximum(100);
                    m_progress->setValue(progress);
                    m_status->setText(tr("正在上传 %1 … %2%").arg(filename).arg(progress));
                }, Qt::QueuedConnection);
        connect(m_uploader, &SftpUploaderCore::uploadCompleted, this,
                [this](const QString &, const QString &filename) {
                    m_progress->setVisible(false);
                    m_status->setText(tr("上传完成：%1").arg(filename));
                    refresh();
                }, Qt::QueuedConnection);
        connect(m_uploader, &SftpUploaderCore::uploadFailed, this,
                [this](const QString &, const QString &filename, const QString &error) {
                    m_progress->setVisible(false);
                    m_status->setText(tr("上传失败：%1（%2）").arg(filename, error));
                }, Qt::QueuedConnection);
    } else {
        m_uploader->setSshClient(m_client);
    }
    loadPath(m_cwd);
}

QString SftpBrowserWidget::joinPath(const QString &dir, const QString &name)
{
    if (dir == QLatin1String("/"))
        return QLatin1String("/") + name;
    return dir + QLatin1Char('/') + name;
}

// 对应Python: cube-shell.py::_on_cwd_changed（路径没变则不刷新）
void SftpBrowserWidget::setCurrentPath(const QString &path)
{
    QString clean = path;
    while (clean.size() > 1 && clean.endsWith(QLatin1Char('/')))
        clean.chop(1);
    if (clean.isEmpty())
        clean = QStringLiteral("/");
    if (!m_sftp) {
        // 连接尚未就绪：记下路径，setClient 时作为初始目录加载。
        m_cwd = clean;
        m_pathEdit->setText(clean);
        return;
    }
    if (clean == m_cwd)
        return;
    loadPath(clean);
}

void SftpBrowserWidget::loadPath(const QString &path)
{
    if (!m_sftp)
        return;
    // 关键：不在此处清空树、也不改路径栏。网络等待期间旧目录内容与路径栏
    // 保持一致（均为 m_cwd），依赖 m_cwd 的操作（新建/上传等）不会被误导；
    // 数据就绪后由 populate() 在挂起重绘的状态下一次性原子替换，消除“先空后满”的闪烁。
    // 对应Python: refreshDirs/handle_file_tree_updated（新数据就绪前不动旧树）
    m_status->setText(tr("正在加载 %1 …").arg(path));

    // 陈旧响应防护：每次加载递增序号，回调只接受最新一次请求的结果，
    // 防止快速连续双击时乱序返回的旧目录数据覆盖新目录。
    const quint64 seq = ++m_loadSeq;

    // Fetch on a worker thread (listdirAttr is blocking under the session lock).
    SftpClient *sftp = m_sftp;
    QThread *worker = QThread::create([this, sftp, path, seq]() {
        SshError err;
        const SftpFileInfoList entries = sftp->listdirAttr(path, err);
        const QString errMsg = err.message;
        // 回主线程渲染（与 editSelected/decompressSelected 同款接线）。
        QMetaObject::invokeMethod(this, [this, path, seq, entries, errMsg]() {
            if (seq != m_loadSeq)   // 已有更新的请求在途/完成，丢弃陈旧结果
                return;
            if (entries.isEmpty() && !errMsg.isEmpty()) {
                // 失败：旧树原样保留，路径栏回退到 m_cwd（手输无效路径时残留的
                // 错误文本会破坏“路径栏 == m_cwd == 树内容”不变量），
                // 状态栏明确给出目标路径与失败原因，避免误认旧内容为目标目录。
                m_status->setText(tr("加载 %1 失败：%2").arg(path, errMsg));
                m_pathEdit->setText(m_cwd);
                return;
            }
            // 成功后才一次性提交：保证任意时刻路径栏 == m_cwd == 树内容所属目录
            m_cwd = path;
            m_pathEdit->setText(m_cwd);
            populate(path, entries);
            m_status->setText(tr("%1 · %2 项").arg(path).arg(entries.size()));
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 渲染当前目录（平铺，首行为 ".." 返回上级）。
// 对应Python: cube-shell.py::handle_file_tree_updated
void SftpBrowserWidget::populate(const QString &path, const SftpFileInfoList &entries)
{
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    // ".." 返回上级目录条目（根目录除外）。对应Python: ls -al 里的 ".." 行
    if (path != QLatin1String("/")) {
        QString up = path.section(QLatin1Char('/'), 0, -2);
        if (up.isEmpty())
            up = QStringLiteral("/");
        auto *upItem = new QTreeWidgetItem(m_tree);
        upItem->setText(0, QStringLiteral(".."));
        upItem->setIcon(0, iconForFile(QString(), true));
        upItem->setData(0, kPathRole, up);
        upItem->setData(0, kIsDirRole, true);
        upItem->setData(0, kIsUpRole, true);
    }

    // ls 风格排序：忽略隐藏文件前导点、忽略大小写。
    // 对应Python: 远端 ls -al 的字典序输出
    SftpFileInfoList sorted = entries;
    const auto sortKey = [](const SftpFileInfo &e) {
        QString k = e.filename;
        while (k.startsWith(QLatin1Char('.')))
            k.remove(0, 1);
        return k.isEmpty() ? e.filename.toLower() : k.toLower();
    };
    std::sort(sorted.begin(), sorted.end(), [&sortKey](const SftpFileInfo &a, const SftpFileInfo &b) {
        return sortKey(a) < sortKey(b);
    });

    for (const SftpFileInfo &e : sorted) {
        auto *item = new QTreeWidgetItem(m_tree);
        const QString fullPath = joinPath(path, e.filename);
        item->setText(0, e.filename);
        // 目录也展示大小（4096 → "4.00 KB"），与 Python 侧 ls 一致
        item->setText(1, formatFileSize(e.size));
        item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setText(3, permissionText(e));
        item->setText(4, ownerText(e));
        item->setData(0, kPathRole, fullPath);
        item->setData(0, kIsDirRole, e.isDirectory());
        item->setData(0, kModeRole, e.permissions());
        item->setData(0, kSymlinkTargetRole, e.symlinkTarget);
        // 按扩展名映射类型图标（与本地浏览器共用 iconForFile）；
        // 符号链接与可执行文件优先使用专用图标。
        // 对应Python: handle_file_tree_updated 里根据 n[0] 权限位选图标
        item->setIcon(0, iconForFile(e.filename, e.isDirectory(), e.isSymlink(), e.mode));
    }

    m_tree->setUpdatesEnabled(true);
}

void SftpBrowserWidget::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
    if (!item)
        return;
    if (item->data(0, kIsDirRole).toBool())
        loadPath(item->data(0, kPathRole).toString());
}

void SftpBrowserWidget::goUp()
{
    if (m_cwd == QLatin1String("/"))
        return;
    QString up = m_cwd.section(QLatin1Char('/'), 0, -2);
    if (up.isEmpty())
        up = QStringLiteral("/");
    loadPath(up);
}

void SftpBrowserWidget::refresh()
{
    loadPath(m_cwd);
}

void SftpBrowserWidget::onPathEdited()
{
    loadPath(m_pathEdit->text().trimmed().isEmpty() ? QStringLiteral("/") : m_pathEdit->text().trimmed());
}

QString SftpBrowserWidget::selectedRemotePath() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    // ".." 不参与针对选中项的操作（删除/重命名/下载等）。
    if (!item || item->data(0, kIsUpRole).toBool())
        return QString();
    return item->data(0, kPathRole).toString();
}

void SftpBrowserWidget::mkdir()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件夹"), tr("文件夹名称："),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = joinPath(m_cwd, name.trimmed());
    SshError err;
    // Quick op; run directly (short under the lock).
    if (m_sftp->mkdir(path, 0755, &err))
        refresh();
    else
        QMessageBox::warning(this, tr("创建文件夹"), err.message);
}

void SftpBrowserWidget::removeSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty())
        return;
    if (QMessageBox::question(this, tr("删除"), tr("确定要递归删除 %1 吗？").arg(path)) != QMessageBox::Yes)
        return;
    SftpClient *sftp = m_sftp;
    QThread *worker = QThread::create([sftp, path]() {
        SshError err;
        const bool ok = sftp->removeRecursive(path, &err);
        QMetaObject::invokeMethod(sftp, [sftp, ok, err]() {
            if (!ok)
                emit sftp->operationFailed(QStringLiteral("remove"), QString(), err.message);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, this, &SftpBrowserWidget::refresh);
    startWorker(worker);
}

void SftpBrowserWidget::uploadFiles()
{
    if (!m_uploader)
        return;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("上传文件"));
    for (const QString &local : files) {
        const QString remote = joinPath(m_cwd, QFileInfo(local).fileName());
        // 分片上传（断点续传）；fileId 用远端路径。
        // 对应Python: sftp_uploader_core.py::upload_file
        m_progress->setVisible(true);
        m_uploader->uploadFile(remote, local, remote);
    }
}

void SftpBrowserWidget::downloadSelected()
{
    const QString remote = selectedRemotePath();
    if (remote.isEmpty())
        return;
    QTreeWidgetItem *item = m_tree->currentItem();
    if (item && item->data(0, kIsDirRole).toBool()) {
        QMessageBox::information(this, tr("下载文件"), tr("暂不支持下载文件夹。"));
        return;
    }
    const QString local = QFileDialog::getSaveFileName(this, tr("另存为"), QFileInfo(remote).fileName());
    if (local.isEmpty())
        return;
    m_progress->setVisible(true);
    m_sftp->download(remote, local);
}

// 右键菜单：与 Python 已连接分支逐字对齐（项目、顺序、分隔线）。
// 对应Python: cube-shell.py::treeRight elif self.isConnected 分支
void SftpBrowserWidget::showContextMenu(const QPoint &pos)
{
    if (!m_sftp)
        return;
    if (QTreeWidgetItem *item = m_tree->itemAt(pos))
        m_tree->setCurrentItem(item);

    QMenu menu(this);
    // 图标与文字的间距样式与 Python 侧一致
    menu.setStyleSheet(QStringLiteral(
        "QMenu::item { padding-left: 5px; }"
        "QMenu::icon { padding-right: 0px; }"));
    // macOS 默认隐藏菜单图标，需逐项 setIconVisibleInMenu(true)。
    // 对应Python: 每个 QAction 均调用 setIconVisibleInMenu(True)
    const auto addItem = [this, &menu](const QString &icon, const QString &text,
                                       void (SftpBrowserWidget::*slot)()) {
        QAction *act = menu.addAction(QIcon(icon), text, this, slot);
        act->setIconVisibleInMenu(true);
        return act;
    };
    addItem(QStringLiteral(":/Download.png"), tr("下载文件"), &SftpBrowserWidget::downloadSelected);
    addItem(QStringLiteral(":/Upload.png"), tr("上传文件"), &SftpBrowserWidget::uploadFiles);
    addItem(QStringLiteral(":/Edit.png"), tr("编辑文本"), &SftpBrowserWidget::editSelected);
    addItem(QStringLiteral(":/createdirector.png"), tr("创建文件夹"), &SftpBrowserWidget::mkdir);
    addItem(QStringLiteral(":/createfile.png"), tr("创建文件"), &SftpBrowserWidget::createFileHere);
    addItem(QStringLiteral(":/refresh.png"), tr("刷新"), &SftpBrowserWidget::refresh);
    addItem(QStringLiteral(":/permissions-48.png"), tr("权限"), &SftpBrowserWidget::showPermissions);
    menu.addSeparator();
    addItem(QStringLiteral(":/remove.png"), tr("删除"), &SftpBrowserWidget::removeSelected);
    addItem(QStringLiteral(":/icons-rename-48.png"), tr("重命名"), &SftpBrowserWidget::renameSelected);
    menu.addSeparator();
    addItem(QStringLiteral(":/icons-unzip-48.png"), tr("解压"), &SftpBrowserWidget::decompressSelected);
    addItem(QStringLiteral(":/icons8-zip-48.png"), tr("新建压缩"), &SftpBrowserWidget::compressSelected);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// 创建空文件。对应Python: cube-shell.py::createFile（sftp.file(path,'w') 空写）
void SftpBrowserWidget::createFileHere()
{
    if (!m_sftp)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件"), tr("文件名字:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString path = joinPath(m_cwd, name.trimmed());
    SshError err;
    if (m_sftp->writeFile(path, QByteArray(), err))
        refresh();
    else
        QMessageBox::warning(this, tr("创建文件"), err.message);
}

// 权限对话框：用户/分组/其他 × R/W/X 九个复选框 → chmod。
// 对应Python: cube-shell.py::show_auth + Auth.ok_auth（ui/auth.py）
void SftpBrowserWidget::showPermissions()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !item || !m_sftp)
        return;
    const quint32 perm = item->data(0, kModeRole).toUInt();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("权限设置"));
    auto *grid = new QGridLayout;
    const QStringList rows = {tr("用户"), tr("分组"), tr("其他")};
    const QStringList cols = {QStringLiteral("R"), QStringLiteral("W"), QStringLiteral("X")};
    QVector<QCheckBox *> boxes;   // 顺序：rwxrwxrwx（高位在前）
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows[r], &dlg), r, 0);
        for (int c = 0; c < 3; ++c) {
            auto *box = new QCheckBox(cols[c], &dlg);
            const int bit = 8 - (r * 3 + c);   // 8..0
            box->setChecked(perm & (1u << bit));
            grid->addWidget(box, r, c + 1);
            boxes.append(box);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(grid);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // 对应Python: util.symbolic_to_octal 后 chmod
    int mode = 0;
    for (int i = 0; i < boxes.size(); ++i)
        if (boxes[i]->isChecked())
            mode |= 1 << (8 - i);
    SshError err;
    if (m_sftp->chmod(path, mode, &err))
        refresh();
    else
        QMessageBox::warning(this, tr("权限设置"), err.message);
}

// 远端解压：按后缀选择 unzip/tar 在远端执行。
// 对应Python: cube-shell.py::unzip → core/compressor.py::DecompressThread
void SftpBrowserWidget::decompressSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !m_client)
        return;

    QString cmd;
    if (path.endsWith(QLatin1String(".zip")))
        cmd = QStringLiteral("unzip -o '%1' -d '%2'").arg(path, m_cwd);
    else if (path.endsWith(QLatin1String(".tar.gz")) || path.endsWith(QLatin1String(".tgz")))
        cmd = QStringLiteral("tar -xzvf '%1' -C '%2'").arg(path, m_cwd);
    else if (path.endsWith(QLatin1String(".tar")))
        cmd = QStringLiteral("tar -xvf '%1' -C '%2'").arg(path, m_cwd);
    else {
        QMessageBox::warning(this, tr("解压"),
                             tr("不支持的压缩格式：%1").arg(path.section(QLatin1Char('/'), -1)));
        return;
    }

    m_status->setText(tr("正在解压..."));
    SshClient *client = m_client;
    // 关停标志以共享指针捕获：析构置位后 runCommand 在下一轮读循环退出，
    // 且超时泄漏的线程读到的标志仍有效。
    auto shuttingDown = m_shuttingDown;
    QThread *worker = QThread::create([this, client, cmd, shuttingDown]() {
        const ExecResult res = CommandExecutor::runCommand(client, cmd, false, 120000, {},
                                                           shuttingDown.get());
        if (shuttingDown->load())
            return;   // 关停中：this 可能已析构，不再回投
        const bool ok = res.ok() && res.exitCode == 0;
        const QString msg = ok ? QString() : (res.errorMessage.isEmpty() ? res.stderrText : res.errorMessage);
        QMetaObject::invokeMethod(this, [this, ok, msg]() {
            m_status->setText(ok ? tr("解压任务已完成") : tr("解压失败：%1").arg(msg));
            if (ok)
                refresh();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 对应Python: cube-shell.py::rename_file
void SftpBrowserWidget::renameSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty())
        return;
    const QString oldName = path.section(QLatin1Char('/'), -1);
    bool ok = false;
    const QString newName = QInputDialog::getText(this, tr("重命名"), tr("新名称："),
                                                  QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == oldName)
        return;
    const QString dir = path.section(QLatin1Char('/'), 0, -2);
    SshError err;
    if (m_sftp->rename(path, joinPath(dir.isEmpty() ? QStringLiteral("/") : dir, newName.trimmed()), &err))
        refresh();
    else
        QMessageBox::warning(this, tr("重命名"), err.message);
}

// 远程文件编辑：读取 → TextEditor → 保存回写。
// 对应Python: cube-shell.py::edit_file + text_editor 保存回调
void SftpBrowserWidget::editSelected()
{
    const QString remote = selectedRemotePath();
    if (remote.isEmpty() || !m_sftp)
        return;
    QTreeWidgetItem *item = m_tree->currentItem();
    if (item && item->data(0, kIsDirRole).toBool())
        return;

    m_status->setText(tr("正在打开 %1 …").arg(remote));
    SftpClient *sftp = m_sftp;
    QThread *worker = QThread::create([this, sftp, remote]() {
        SshError err;
        const QByteArray data = sftp->readFile(remote, err);
        const QString errMsg = err.message;
        // 回主线程开编辑器（跨线程 → QueuedConnection）。
        QMetaObject::invokeMethod(this, [this, remote, data, errMsg]() {
            if (!errMsg.isEmpty()) {
                m_status->setText(tr("打开失败：%1").arg(errMsg));
                return;
            }
            m_status->setText(m_cwd);
            auto *editor = new TextEditor(nullptr);
            editor->setAttribute(Qt::WA_DeleteOnClose);
            editor->setFileLabel(remote);
            editor->setPlainText(QString::fromUtf8(data));
            connect(editor, &TextEditor::saveRequested, this,
                    [this, remote](const QString &content) {
                        SftpClient *sftp = m_sftp;
                        if (!sftp)
                            return;
                        const QByteArray bytes = content.toUtf8();
                        QThread *w = QThread::create([this, sftp, remote, bytes]() {
                            SshError err;
                            const bool ok = sftp->writeFile(remote, bytes, err);
                            const QString msg = err.message;
                            QMetaObject::invokeMethod(this, [this, remote, ok, msg]() {
                                m_status->setText(ok ? tr("已保存：%1").arg(remote)
                                                     : tr("保存失败：%1").arg(msg));
                            }, Qt::QueuedConnection);
                        });
                        startWorker(w);
                    });
            editor->show();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

// 远端压缩：tar/zip 命令在远端执行。对应Python: cube-shell.py::compress_file
void SftpBrowserWidget::compressSelected()
{
    const QString path = selectedRemotePath();
    if (path.isEmpty() || !m_client)
        return;
    const QString baseName = path.section(QLatin1Char('/'), -1);

    CompressDialog dlg(this, baseName);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString dir = m_cwd;
    QString cmd;
    if (dlg.format() == QLatin1String(".zip"))
        cmd = QStringLiteral("cd '%1' && zip -r '%2' '%3'").arg(dir, dlg.fileName(), baseName);
    else
        cmd = QStringLiteral("tar -czf '%1' -C '%2' '%3'")
                  .arg(joinPath(dir, dlg.fileName()), dir, baseName);

    m_status->setText(tr("正在压缩 %1 …").arg(baseName));
    SshClient *client = m_client;
    // 阻塞执行（无信号，线程安全的静态入口）；关停标志接线同 decompressSelected。
    auto shuttingDown = m_shuttingDown;
    QThread *worker = QThread::create([this, client, cmd, shuttingDown]() {
        const ExecResult res = CommandExecutor::runCommand(client, cmd, false, 120000, {},
                                                           shuttingDown.get());
        if (shuttingDown->load())
            return;   // 关停中：this 可能已析构，不再回投
        const bool ok = res.ok() && res.exitCode == 0;
        const QString msg = ok ? QString() : (res.errorMessage.isEmpty() ? res.stderrText : res.errorMessage);
        QMetaObject::invokeMethod(this, [this, ok, msg]() {
            m_status->setText(ok ? tr("压缩完成") : tr("压缩失败：%1").arg(msg));
            if (ok)
                refresh();
        }, Qt::QueuedConnection);
    });
    startWorker(worker);
}

} // namespace cubeshell
