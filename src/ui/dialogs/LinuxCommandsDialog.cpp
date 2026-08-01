// LinuxCommandsDialog.cpp — see LinuxCommandsDialog.h.
// 对应Python: core/frequently_used_commands.py::TreeSearchApp

#include "LinuxCommandsDialog.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cubeshell {

LinuxCommandsDialog::LinuxCommandsDialog(QWidget *parent, const QString &jsonPath)
    : QDialog(parent)
{
    // 对应Python: TreeSearchApp.__init__（标题/尺寸/搜索框/树视图）
    setWindowTitle(tr("Linux常用命令查找"));
    resize(600, 400);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索命令…"));
    connect(m_search, &QLineEdit::textChanged, this, &LinuxCommandsDialog::onSearchChanged);
    layout->addWidget(m_search);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("命令"), tr("选项"), tr("描述")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    connect(m_tree, &QTreeWidget::itemClicked, this, &LinuxCommandsDialog::onItemClicked);
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    const QString path = jsonPath.isEmpty() ? resolveCommandsFile() : jsonPath;
    QString error;
    if (path.isEmpty() || !m_commands.load(path, &error)) {
        m_status->setText(tr("加载命令数据失败：%1")
                              .arg(error.isEmpty() ? tr("未找到 linux_commands.json") : error));
        return;
    }
    rebuildTree(QString());
}

void LinuxCommandsDialog::onSearchChanged(const QString &text)
{
    rebuildTree(text);
}

// 点击即复制 "命令 选项" 到剪贴板（分类节点只复制命令列文本）。
void LinuxCommandsDialog::onItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    const QString text =
        QStringLiteral("%1 %2").arg(item->text(0), item->text(1)).trimmed();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    m_status->setText(tr("已复制：%1").arg(text));
}

// 对应Python: TreeFilterProxyModel 的过滤呈现（filter() 已保留祖先/子树）
void LinuxCommandsDialog::rebuildTree(const QString &searchText)
{
    m_tree->clear();
    const QString needle = searchText.trimmed();
    const QList<CommandEntry> entries =
        needle.isEmpty() ? m_commands.entries() : m_commands.filter(needle);
    addEntries(nullptr, entries);
    if (!needle.isEmpty())
        m_tree->expandAll();
}

// 对应Python: TreeSearchApp.add_items
void LinuxCommandsDialog::addEntries(QTreeWidgetItem *parent,
                                     const QList<CommandEntry> &entries)
{
    for (const CommandEntry &entry : entries) {
        auto *item = new QTreeWidgetItem({entry.command, entry.option, entry.description});
        if (parent)
            parent->addChild(item);
        else
            m_tree->addTopLevelItem(item);
        if (entry.hasChildren())
            addEntries(item, entry.children);
    }
}

// conf/linux_commands.json 探测顺序与 AiPreferences 的 conf 探测一致：
// 应用目录 → macOS bundle Resources → cwd → 源码树（CMake 注入）。
QString LinuxCommandsDialog::resolveCommandsFile()
{
    const QString fileName = QStringLiteral("linux_commands.json");
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/conf/") + fileName
               << appDir + QStringLiteral("/../Resources/conf/") + fileName
               << QDir::currentPath() + QStringLiteral("/conf/") + fileName;
#ifdef CUBESHELL_SOURCE_CONF_DIR
    // Dev tree: cpp/build/... -> repo root conf/ (source layout, CMake 注入).
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/") + fileName;
#endif
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    return QString();
}

} // namespace cubeshell
