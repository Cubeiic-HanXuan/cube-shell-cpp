#include "local_file_browser_widget.h"

#include "file_icons.h"
#include "dialogs/CompressDialog.h"
#include "editors/TextEditor.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyle>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace cubeshell {

// Roles on tree items（与 SftpBrowserWidget 保持一致的角色布局）.
static constexpr int kPathRole = Qt::UserRole;       // full local path
static constexpr int kIsDirRole = Qt::UserRole + 1;  // bool
static constexpr int kIsUpRole = Qt::UserRole + 3;   // ".." 返回上级条目

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

// ls 风格权限串（类型位 + rwx 九位），由 QFileInfo 权限位拼出。
// 对应Python: _get_local_dir_now 里 stat.filemode(st.st_mode)
static QString permissionText(const QFileInfo &fi)
{
    QString s(fi.isSymLink() ? QLatin1Char('l')
                             : fi.isDir() ? QLatin1Char('d') : QLatin1Char('-'));
    const QFile::Permissions p = fi.permissions();
    s += (p & QFile::ReadOwner) ? QLatin1Char('r') : QLatin1Char('-');
    s += (p & QFile::WriteOwner) ? QLatin1Char('w') : QLatin1Char('-');
    s += (p & QFile::ExeOwner) ? QLatin1Char('x') : QLatin1Char('-');
    s += (p & QFile::ReadGroup) ? QLatin1Char('r') : QLatin1Char('-');
    s += (p & QFile::WriteGroup) ? QLatin1Char('w') : QLatin1Char('-');
    s += (p & QFile::ExeGroup) ? QLatin1Char('x') : QLatin1Char('-');
    s += (p & QFile::ReadOther) ? QLatin1Char('r') : QLatin1Char('-');
    s += (p & QFile::WriteOther) ? QLatin1Char('w') : QLatin1Char('-');
    s += (p & QFile::ExeOther) ? QLatin1Char('x') : QLatin1Char('-');
    return s;
}

// 所有者/组：取不到名称时退回数字 uid/gid。
// 对应Python: _get_local_dir_now 里 _owner_name/_group_name
static QString ownerText(const QFileInfo &fi)
{
    QString owner = fi.owner();
    if (owner.isEmpty())
        owner = QString::number(fi.ownerId());
    QString group = fi.group();
    if (group.isEmpty())
        group = QString::number(fi.groupId());
    return owner + QLatin1Char('/') + group;
}

// 对应Python: 本机终端时左侧 treeWidget 展示本地目录（与远程文件树同款五列）
LocalFileBrowserWidget::LocalFileBrowserWidget(QWidget *parent)
    : QWidget(parent)
{
    // 顶部仅保留路径栏，与 SftpBrowserWidget 一致（".." 行负责返回上级）。
    m_pathEdit = new QLineEdit(this);

    // 平铺列表（非展开树），五列表头与 SFTP 浏览器 / Python 侧一致。
    // 对应Python: handle_file_tree_updated 里 setRootIsDecorated(False)/setIndentation(0)
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("文件名"), tr("文件大小"), tr("修改日期"),
                             tr("权限"), tr("所有者/组")});
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(0);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QTreeWidget::ExtendedSelection);
    // 列宽可拖动、总宽超出面板时出现水平滚动条（与 SFTP 浏览器一致）。
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setSectionsMovable(true);
    m_tree->setColumnWidth(0, 200);
    m_tree->setColumnWidth(1, 90);
    m_tree->setColumnWidth(2, 130);
    m_tree->setColumnWidth(3, 100);
    m_tree->setColumnWidth(4, 110);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(m_pathEdit);
    layout->addWidget(m_tree, 1);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &LocalFileBrowserWidget::onPathEdited);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &LocalFileBrowserWidget::onItemDoubleClicked);

    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接 + is_local 分支）
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &LocalFileBrowserWidget::showContextMenu);

    setRootPath(QDir::homePath());
}

void LocalFileBrowserWidget::setRootPath(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || !QFileInfo(clean).isDir())
        return;
    m_rootPath = clean;
    m_pathEdit->setText(clean);
    populate();
}

// 渲染当前目录（平铺，首行为 ".." 返回上级）。
// 对应Python: handle_file_tree_updated（本地分支复用同一渲染逻辑）
void LocalFileBrowserWidget::populate()
{
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    // ".." 返回上级目录条目（根目录除外）。对应Python: ls -al 里的 ".." 行
    QDir dir(m_rootPath);
    if (!dir.isRoot()) {
        auto *upItem = new QTreeWidgetItem(m_tree);
        upItem->setText(0, QStringLiteral(".."));
        upItem->setIcon(0, iconForFile(QString(), true));
        upItem->setData(0, kIsDirRole, true);
        upItem->setData(0, kIsUpRole, true);
    }

    // ls 风格排序：忽略隐藏文件前导点、忽略大小写（与 SFTP 浏览器一致）。
    QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    const auto sortKey = [](const QFileInfo &e) {
        QString k = e.fileName();
        while (k.startsWith(QLatin1Char('.')))
            k.remove(0, 1);
        return k.isEmpty() ? e.fileName().toLower() : k.toLower();
    };
    std::sort(entries.begin(), entries.end(), [&sortKey](const QFileInfo &a, const QFileInfo &b) {
        return sortKey(a) < sortKey(b);
    });

    for (const QFileInfo &fi : entries) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, fi.fileName());
        item->setText(1, formatFileSize(fi.size()));
        item->setText(2, fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setText(3, permissionText(fi));
        item->setText(4, ownerText(fi));
        item->setData(0, kPathRole, fi.absoluteFilePath());
        item->setData(0, kIsDirRole, fi.isDir());
        // 按扩展名映射类型图标（与 Python 版共用同一套映射）。
        // 对应Python: item.setIcon(0, util.get_default_file_icon(n[8]))
        item->setIcon(0, iconForFile(fi.fileName(), fi.isDir()));
    }

    m_tree->setUpdatesEnabled(true);
}

void LocalFileBrowserWidget::goUp()
{
    QDir dir(m_rootPath);
    if (dir.cdUp())
        setRootPath(dir.absolutePath());
}

void LocalFileBrowserWidget::goHome()
{
    setRootPath(QDir::homePath());
}

void LocalFileBrowserWidget::onPathEdited()
{
    const QString path = m_pathEdit->text().trimmed();
    if (!path.isEmpty())
        setRootPath(path);
    else
        m_pathEdit->setText(m_rootPath);
}

void LocalFileBrowserWidget::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
    if (!item || !item->data(0, kIsDirRole).toBool())
        return;
    if (item->data(0, kIsUpRole).toBool())
        goUp();
    else
        setRootPath(item->data(0, kPathRole).toString());
}

void LocalFileBrowserWidget::refresh()
{
    populate();
}

QString LocalFileBrowserWidget::selectedPath() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    // ".." 不参与针对选中项的操作（删除/重命名/编辑等）。
    if (!item || item->data(0, kIsUpRole).toBool())
        return QString();
    return item->data(0, kPathRole).toString();
}

QStringList LocalFileBrowserWidget::selectedPaths() const
{
    QStringList paths;
    const QList<QTreeWidgetItem *> items = m_tree->selectedItems();
    for (QTreeWidgetItem *item : items) {
        if (item->data(0, kIsUpRole).toBool())
            continue;
        const QString p = item->data(0, kPathRole).toString();
        if (!p.isEmpty())
            paths.append(p);
    }
    return paths;
}

// 右键菜单：与 Python 已连接分支逐字对齐，本机模式额外含
// “新建位于文件夹位置的终端窗口”与“在文件资源管理器中显示”两项。
// 对应Python: cube-shell.py::treeRight elif self.isConnected 分支（is_local）
void LocalFileBrowserWidget::showContextMenu(const QPoint &pos)
{
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
                                       void (LocalFileBrowserWidget::*slot)()) {
        QAction *act = menu.addAction(QIcon(icon), text, this, slot);
        act->setIconVisibleInMenu(true);
        return act;
    };
    addItem(QStringLiteral(":/Download.png"), tr("下载文件"), &LocalFileBrowserWidget::downloadSelected);
    addItem(QStringLiteral(":/Upload.png"), tr("上传文件"), &LocalFileBrowserWidget::uploadFiles);
    addItem(QStringLiteral(":/Edit.png"), tr("编辑文本"), &LocalFileBrowserWidget::editSelected);
    addItem(QStringLiteral(":/createdirector.png"), tr("创建文件夹"), &LocalFileBrowserWidget::createDirHere);
    addItem(QStringLiteral(":/createfile.png"), tr("创建文件"), &LocalFileBrowserWidget::createFileHere);
    addItem(QStringLiteral(":/refresh.png"), tr("刷新"), &LocalFileBrowserWidget::refresh);
    // 本机专属两项。对应Python: is_local 时的 action_new_local_terminal / action_show_in_explorer
    addItem(QStringLiteral(":/icons8-ssh-48.png"), tr("新建位于文件夹位置的终端窗口"),
            &LocalFileBrowserWidget::openTerminalHere);
    addItem(QStringLiteral(":/open.png"), tr("在文件资源管理器中显示"),
            &LocalFileBrowserWidget::showInFileManager);
    addItem(QStringLiteral(":/permissions-48.png"), tr("权限"), &LocalFileBrowserWidget::showPermissions);
    menu.addSeparator();
    addItem(QStringLiteral(":/remove.png"), tr("删除"), &LocalFileBrowserWidget::removeSelected);
    addItem(QStringLiteral(":/icons-rename-48.png"), tr("重命名"), &LocalFileBrowserWidget::renameSelected);
    menu.addSeparator();
    addItem(QStringLiteral(":/icons-unzip-48.png"), tr("解压"), &LocalFileBrowserWidget::decompressSelected);
    addItem(QStringLiteral(":/icons8-zip-48.png"), tr("新建压缩"), &LocalFileBrowserWidget::compressSelected);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// 本机“下载”= 把选中文件复制到用户选择的目录。
// 对应Python: downloadFile（本机路径下 download_with_resume 退化为本地拷贝）
void LocalFileBrowserWidget::downloadSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("选择保存文件夹"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (directory.isEmpty())
        return;
    QStringList failed;
    for (const QString &src : paths) {
        const QFileInfo fi(src);
        // Python 本机分支同样不支持文件夹下载（open 目录会失败弹窗）
        if (fi.isDir()) {
            failed.append(fi.fileName());
            continue;
        }
        const QString dst = directory + QLatin1Char('/') + fi.fileName();
        QFile::remove(dst);   // 覆盖同名目标（QFile::copy 遇同名会失败）
        if (!QFile::copy(src, dst))
            failed.append(fi.fileName());
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("下载文件"),
                             tr("无法下载文件，请确认！") + QLatin1Char('\n') + failed.join(QLatin1Char('\n')));
}

// 本机“上传”= 把选择的文件复制进当前目录。
// 对应Python: uploadFile（LocalSFTPClient 下上传退化为本地拷贝）
void LocalFileBrowserWidget::uploadFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("选择文件"), QString(),
                                                            tr("所有文件 (*)"));
    if (files.isEmpty())
        return;
    QStringList failed;
    for (const QString &src : files) {
        const QString dst = m_rootPath + QLatin1Char('/') + QFileInfo(src).fileName();
        QFile::remove(dst);
        if (!QFile::copy(src, dst))
            failed.append(QFileInfo(src).fileName());
    }
    populate();
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("上传文件"),
                             tr("上传失败：%1").arg(failed.join(QStringLiteral(", "))));
}

// 本机文件编辑：直接读本地文件 → TextEditor → 保存回写。
// 对应Python: editFile（is_local 分支 open(local_path)）+ save_file（is_local 分支）
void LocalFileBrowserWidget::editSelected()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    const QFileInfo fi(path);
    if (fi.isDir()) {
        QMessageBox::warning(this, tr("编辑文本"), tr("文件夹不能被编辑！"));
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("编辑文本"), tr("无法编辑文件，请确认！"));
        return;
    }
    const QByteArray data = f.readAll();
    f.close();

    auto *editor = new TextEditor(nullptr);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->setFileLabel(path);
    editor->setPlainText(QString::fromUtf8(data));
    connect(editor, &TextEditor::saveRequested, this, [this, path](const QString &content) {
        // QSaveFile：写临时文件后原子替换，避免写到一半损坏原文件
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) ||
            out.write(content.toUtf8()) < 0 || !out.commit()) {
            QMessageBox::warning(this, tr("编辑文本"), tr("保存失败：%1").arg(path));
            return;
        }
        populate();
    });
    editor->show();
}

// 对应Python: createDir（本机路径下 LocalSFTPClient.mkdir → os.mkdir）
void LocalFileBrowserWidget::createDirHere()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件夹"), tr("文件夹名字:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QDir dir(m_rootPath);
    if (dir.exists(name.trimmed())) {
        QMessageBox::warning(this, tr("创建文件夹"), tr("文件夹已存在"));
        return;
    }
    if (dir.mkdir(name.trimmed()))
        populate();
    else
        QMessageBox::warning(this, tr("创建文件夹"), tr("当前文件夹权限不足，请设置权限之后再操作"));
}

// 对应Python: createFile（sftp.file(path,'w') 空写，本机即新建空文件）
void LocalFileBrowserWidget::createFileHere()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建文件"), tr("文件名字:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QFile f(m_rootPath + QLatin1Char('/') + name.trimmed());
    if (f.open(QIODevice::WriteOnly)) {   // 与 Python 'w' 一致：存在则清空
        f.close();
        populate();
    } else {
        QMessageBox::warning(this, tr("创建文件"), tr("当前文件夹权限不足，请设置权限之后再操作"));
    }
}

// 选中目录则在该目录、否则在当前目录新开本机终端（由外层接线）。
// 对应Python: open_local_terminal_in_selected_folder
void LocalFileBrowserWidget::openTerminalHere()
{
    QString target = m_rootPath;
    const QString sel = selectedPath();
    if (!sel.isEmpty() && QFileInfo(sel).isDir())
        target = sel;
    emit newTerminalRequested(target);
}

// 跨平台“在文件资源管理器中显示”。对应Python: util.open_file_in_explorer
void LocalFileBrowserWidget::showInFileManager()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("错误"), tr("路径不存在: ") + path);
        return;
    }
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
    // Linux：依次尝试 nautilus/dolphin 选中定位，否则 xdg-open 打开所在目录
    if (!QStandardPaths::findExecutable(QStringLiteral("nautilus")).isEmpty())
        QProcess::startDetached(QStringLiteral("nautilus"), {QStringLiteral("--select"), path});
    else if (!QStandardPaths::findExecutable(QStringLiteral("dolphin")).isEmpty())
        QProcess::startDetached(QStringLiteral("dolphin"), {QStringLiteral("--select"), path});
    else
        QProcess::startDetached(QStringLiteral("xdg-open"), {QFileInfo(path).absolutePath()});
#endif
}

// 权限对话框：用户/分组/其他 × R/W/X 九个复选框 → chmod。
// 对应Python: show_auth + Auth.ok_auth（is_local 分支 os.chmod）
void LocalFileBrowserWidget::showPermissions()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    const QFile::Permissions perm = QFileInfo(path).permissions();
    // 预勾选状态按 rwxrwxrwx 顺序（高位在前），与 Python 从权限列解析一致
    const QFile::Permissions bits[9] = {
        QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner,
        QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup,
        QFile::ReadOther, QFile::WriteOther, QFile::ExeOther,
    };

    QDialog dlg(this);
    dlg.setWindowTitle(tr("权限设置"));
    auto *grid = new QGridLayout;
    const QStringList rows = {tr("用户"), tr("分组"), tr("其他")};
    const QStringList cols = {QStringLiteral("R"), QStringLiteral("W"), QStringLiteral("X")};
    QVector<QCheckBox *> boxes;
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows[r], &dlg), r, 0);
        for (int c = 0; c < 3; ++c) {
            auto *box = new QCheckBox(cols[c], &dlg);
            box->setChecked(perm & bits[r * 3 + c]);
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

    QFile::Permissions next;
    for (int i = 0; i < 9; ++i)
        if (boxes[i]->isChecked())
            next |= bits[i];
    if (QFile::setPermissions(path, next))
        populate();
    else
        QMessageBox::warning(this, tr("权限设置"), tr("设置权限失败：%1").arg(path));
}

// 删除：文件 QFile::remove，目录递归删除。对应Python: remove（批量 + 确认框）
void LocalFileBrowserWidget::removeSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    if (QMessageBox::question(this, tr("确认删除"),
                              tr("确定删除选中项目吗？这将无法恢复！")) != QMessageBox::Yes)
        return;
    QStringList failed;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        bool ok = false;
        if (fi.isDir() && !fi.isSymLink())
            ok = QDir(p).removeRecursively();
        else
            ok = QFile::remove(p);
        if (!ok)
            failed.append(fi.fileName());
    }
    populate();
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("删除"),
                             tr("删除失败：%1").arg(failed.join(QStringLiteral(", "))));
}

// 重命名：逐项弹框。对应Python: rename（is_local 分支 os.rename）
void LocalFileBrowserWidget::renameSelected()
{
    const QStringList paths = selectedPaths();
    for (const QString &p : paths) {
        const QString oldName = QFileInfo(p).fileName();
        bool ok = false;
        const QString newName = QInputDialog::getText(this, tr("重命名"),
                                                      tr("请输入新的文件名") + QStringLiteral("："),
                                                      QLineEdit::Normal, oldName, &ok);
        if (!ok || newName.trimmed().isEmpty() || newName == oldName)
            continue;
        const QString dst = QFileInfo(p).absolutePath() + QLatin1Char('/') + newName.trimmed();
        if (!QFile::rename(p, dst))
            QMessageBox::warning(this, tr("重命名"), tr("重命名失败：%1").arg(oldName));
    }
    populate();
}

// 本机解压：按后缀调用系统 unzip/tar（后台线程，避免大包卡 UI）。
// 对应Python: unzip → DecompressThread（is_local 分支 zipfile/tarfile）
void LocalFileBrowserWidget::decompressSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    QList<QPair<QString, QStringList>> cmds;   // program + args
    for (const QString &p : paths) {
        if (p.endsWith(QLatin1String(".zip")))
            cmds.append({QStringLiteral("unzip"), {QStringLiteral("-o"), p, QStringLiteral("-d"), m_rootPath}});
        else if (p.endsWith(QLatin1String(".tar.gz")) || p.endsWith(QLatin1String(".tgz")))
            cmds.append({QStringLiteral("tar"), {QStringLiteral("-xzf"), p, QStringLiteral("-C"), m_rootPath}});
        else if (p.endsWith(QLatin1String(".tar")))
            cmds.append({QStringLiteral("tar"), {QStringLiteral("-xf"), p, QStringLiteral("-C"), m_rootPath}});
        else {
            QMessageBox::warning(this, tr("解压"),
                                 tr("不支持的压缩格式：%1").arg(QFileInfo(p).fileName()));
            return;
        }
    }

    QThread *worker = QThread::create([this, cmds]() {
        QString errMsg;
        for (const auto &c : cmds) {
            QProcess proc;
            proc.start(c.first, c.second);
            if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
                errMsg = QString::fromUtf8(proc.readAllStandardError());
                if (errMsg.isEmpty())
                    errMsg = tr("命令执行失败：%1").arg(c.first);
                break;
            }
        }
        QMetaObject::invokeMethod(this, [this, errMsg]() {
            if (errMsg.isEmpty())
                populate();
            else
                QMessageBox::warning(this, tr("解压失败"), errMsg);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// 本机压缩：在当前目录下 tar/zip 选中项（后台线程）。
// 对应Python: zip → CompressDialog + CompressThread（is_local 分支 tarfile/zipfile）
void LocalFileBrowserWidget::compressSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    // 默认文件名：首个选中项去前导点去后缀。对应Python: zip 里的 base_name
    QString base = QFileInfo(paths.first()).fileName();
    while (base.startsWith(QLatin1Char('.')))
        base.remove(0, 1);
    const int dot = base.indexOf(QLatin1Char('.'));
    if (dot > 0)
        base = base.left(dot);

    CompressDialog dlg(this, base);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (dlg.archiveName().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("错误"), tr("文件名不能为空"));
        return;
    }

    // 相对当前目录的文件名列表（与 Python 保持一致，归档内不带绝对路径）
    QStringList relNames;
    for (const QString &p : paths)
        relNames.append(QFileInfo(p).fileName());

    QString program;
    QStringList args;
    if (dlg.format() == QLatin1String(".zip")) {
        program = QStringLiteral("zip");
        args << QStringLiteral("-r") << dlg.fileName() << relNames;
    } else {
        program = QStringLiteral("tar");
        args << QStringLiteral("-czf") << dlg.fileName() << relNames;
    }

    const QString workDir = m_rootPath;
    QThread *worker = QThread::create([this, program, args, workDir]() {
        QProcess proc;
        proc.setWorkingDirectory(workDir);
        proc.start(program, args);
        QString errMsg;
        if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
            errMsg = QString::fromUtf8(proc.readAllStandardError());
            if (errMsg.isEmpty())
                errMsg = tr("命令执行失败：%1").arg(program);
        }
        QMetaObject::invokeMethod(this, [this, errMsg]() {
            if (errMsg.isEmpty())
                populate();
            else
                QMessageBox::warning(this, tr("压缩失败"), errMsg);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

} // namespace cubeshell
