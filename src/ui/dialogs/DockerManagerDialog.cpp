// DockerManagerDialog.cpp — see DockerManagerDialog.h.
// 对应Python: cube-shell.py:1007-1045 _ensure_docker_manager_dialog
//           + cube-shell.py:3705-3785 treeDocker / execDockerTerminal / viewDockerLogs
//           + cube-shell.py:4693-4804 refreshDokerInfo / update_docker_ui
//           + cube-shell.py:5221-5362 Docker 操作流程（标记/局部刷新/删除行）

#include "DockerManagerDialog.h"

#include <QAction>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

// 树列序，与 Python treeWidgetDocker 一致：#/容器ID/容器/镜像/状态/创建时间/端口。
// 对应Python: cube-shell.py:1021-1022 setHeaderLabels
enum Column {
    ColIcon = 0,   // "#"（容器图标 / 项目名 / 提示文本）
    ColId,         // 容器ID
    ColName,       // 容器
    ColImage,      // 镜像
    ColState,      // 状态
    ColCreated,    // 创建时间
    ColPorts,      // 端口
    ColCount
};

// 占位行（加载中/提示行）标记，右键菜单对这类行不弹出。
constexpr int kPlaceholderRole = Qt::UserRole + 1;

// 对应Python: cube-shell.py:5243-5249 / 5343-5349 的 operation_names dict
QString operationDisplayName(const QString &op)
{
    if (op == QLatin1String("stop"))
        return QStringLiteral("停止");
    if (op == QLatin1String("restart"))
        return QStringLiteral("重启");
    if (op == QLatin1String("rm"))
        return QStringLiteral("删除");
    if (op == QLatin1String("start"))
        return QStringLiteral("启动");
    return op;
}

// 对应Python: cube-shell.py:3712-3719 tree_menu.setStyleSheet
const char *kMenuQss = R"(
                QMenu::item {
                    padding-left: 5px;  /* 调整图标和文字之间的间距 */
                }
                QMenu::icon {
                    padding-right: 0px; /* 设置图标右侧的间距 */
                }
            )";

} // namespace

DockerManagerDialog::DockerManagerDialog(DockerManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    // 对应Python: cube-shell.py:1012-1016 窗口参数（非模态 900x500）
    setWindowTitle(tr("Docker 容器管理"));
    setMinimumSize(900, 500);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    // 对应Python: cube-shell.py:1018-1025 QTreeWidget treeWidgetDocker
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("treeWidgetDocker"));
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("#"), tr("容器ID"), tr("容器"), tr("镜像"),
                             tr("状态"), tr("创建时间"), tr("端口")});
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &DockerManagerDialog::onTreeContextMenu);
    layout->addWidget(m_tree);

    // 对应Python: cube-shell.py:1027-1032 底部右对齐"刷新"按钮
    auto *btnLayout = new QHBoxLayout();
    auto *refreshBtn = new QPushButton(tr("刷新"), this);
    connect(refreshBtn, &QPushButton::clicked, this, &DockerManagerDialog::refreshInfo);
    btnLayout->addStretch();
    btnLayout->addWidget(refreshBtn);
    layout->addLayout(btnLayout);

    // Manager 信号来自工作线程 —— 显式 QueuedConnection。
    connect(m_manager, &DockerManager::groupedContainersUpdated,
            this, &DockerManagerDialog::onGroupedContainersUpdated, Qt::QueuedConnection);
    connect(m_manager, &DockerManager::containerOperationFinished,
            this, &DockerManagerDialog::onContainerOperationFinished, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// 刷新流程
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py:4693-4746 refreshDokerInfo
void DockerManagerDialog::refreshInfo()
{
    // 对应Python: cube-shell.py:4698-4702 未连接 → 单行提示
    if (!m_manager->isRemote()) {
        showPlaceholderRow(tr("没有可用的docker容器"));
        return;
    }

    m_tree->clear();
    // 对应Python: cube-shell.py:4713-4717 重设表头文本
    m_tree->setHeaderLabels({tr("#"), tr("容器ID"), tr("容器"), tr("镜像"),
                             tr("状态"), tr("创建时间"), tr("端口")});

    // 对应Python: cube-shell.py:4719-4728 表头居中/可拖动/Interactive 列宽
    QHeaderView *header = m_tree->header();
    header->setDefaultAlignment(Qt::AlignCenter);
    header->setSectionsMovable(true);
    header->setSectionResizeMode(QHeaderView::Interactive);

    // 对应Python: cube-shell.py:4730-4733 加载状态行
    auto *loadingItem = new QTreeWidgetItem();
    loadingItem->setText(ColIcon, QStringLiteral("正在加载 Docker 信息..."));
    loadingItem->setData(ColIcon, kPlaceholderRole, true);
    m_tree->addTopLevelItem(loadingItem);

    // 对应Python: cube-shell.py:4736-4746 已有刷新在跑则忽略新请求
    if (m_refreshing)
        return;
    m_refreshing = true;
    m_manager->refreshGroupedContainers();
}

void DockerManagerDialog::showPlaceholderRow(const QString &text)
{
    // 对应Python: cube-shell.py:4699-4701 / 4775-4776 的单行提示写法
    m_tree->clear();
    auto *item = new QTreeWidgetItem();
    item->setText(ColIcon, text);
    item->setData(ColIcon, kPlaceholderRole, true);
    m_tree->addTopLevelItem(item);
}

// 对应Python: cube-shell.py:4748-4784 update_docker_ui
void DockerManagerDialog::onGroupedContainersUpdated(const QList<cubeshell::DockerComposeGroup> &groups)
{
    m_refreshing = false;
    m_tree->clear();

    if (!groups.isEmpty()) {
        for (const DockerComposeGroup &group : groups) {
            // 项目顶层节点：加粗、全列居中
            // 对应Python: cube-shell.py:4756-4765
            auto *projectItem = new QTreeWidgetItem();
            projectItem->setText(ColIcon, group.name);
            QFont boldFont;
            boldFont.setBold(true);
            projectItem->setFont(ColIcon, boldFont);
            for (int i = 0; i < m_tree->columnCount(); ++i)
                projectItem->setTextAlignment(i, Qt::AlignCenter);
            m_tree->addTopLevelItem(projectItem);

            for (const DockerContainerRow &row : group.containers)
                addContainerItem(row, projectItem);
        }
    } else if (!m_manager->isRemote()) {
        // 未连接时 manager 直接发空列表。对应Python: cube-shell.py:4699-4701
        showPlaceholderRow(tr("没有可用的docker容器"));
        return;
    } else {
        // 对应Python: cube-shell.py:4775-4776
        showPlaceholderRow(tr("服务器还没有安装docker容器"));
        return;
    }

    // 对应Python: cube-shell.py:4779 展开所有节点
    m_tree->expandAll();
}

// 对应Python: cube-shell.py:4786-4804 _add_container_item
void DockerManagerDialog::addContainerItem(const DockerContainerRow &row, QTreeWidgetItem *parentItem)
{
    auto *item = new QTreeWidgetItem();
    item->setText(ColId, row.id);
    item->setText(ColName, row.name);
    item->setText(ColImage, row.image);
    item->setText(ColState, row.state);
    item->setText(ColCreated, row.createdAt);
    item->setText(ColPorts, row.ports);
    item->setIcon(ColIcon, QIcon(QStringLiteral(":/icons8-docker-48.png")));

    for (int i = 0; i < m_tree->columnCount(); ++i)
        item->setTextAlignment(i, Qt::AlignCenter);

    if (parentItem)
        parentItem->addChild(item);
    else
        m_tree->addTopLevelItem(item);
}

// ---------------------------------------------------------------------------
// 右键菜单
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py:3705-3765 treeDocker
void DockerManagerDialog::onTreeContextMenu(const QPoint &position)
{
    // 对应Python: cube-shell.py:3707 if self.isConnected 守卫
    if (!m_manager->isRemote())
        return;

    QTreeWidgetItem *item = m_tree->itemAt(position);
    // 空白处 / 加载中 / 提示行不弹菜单
    if (!item || item->data(ColIcon, kPlaceholderRole).toBool())
        return;

    auto *menu = new QMenu(this);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->setStyleSheet(QString::fromUtf8(kMenuQss));

    // 对应Python: cube-shell.py:3720-3736 五个动作 + 分隔线
    auto *actionStop = new QAction(QIcon(QStringLiteral(":/stop.png")), tr("停止"), menu);
    actionStop->setIconVisibleInMenu(true);
    auto *actionRestart = new QAction(QIcon(QStringLiteral(":/restart.png")), tr("重启"), menu);
    actionRestart->setIconVisibleInMenu(true);
    auto *actionRemove = new QAction(QIcon(QStringLiteral(":/remove.png")), tr("删除"), menu);
    actionRemove->setIconVisibleInMenu(true);
    auto *actionTerminal = new QAction(QIcon(QStringLiteral(":/icons8-linux-48.png")), tr("终端"), menu);
    actionTerminal->setIconVisibleInMenu(true);
    auto *actionLogs = new QAction(QIcon(QStringLiteral(":/icons-log-48.png")), tr("日志"), menu);
    actionLogs->setIconVisibleInMenu(true);

    menu->addAction(actionStop);
    menu->addAction(actionRestart);
    menu->addAction(actionRemove);
    menu->addSeparator();
    menu->addAction(actionTerminal);
    menu->addAction(actionLogs);

    if (item->parent() == nullptr) {
        // 父级（项目组）：收集所有子容器ID，禁用终端和日志
        // 对应Python: cube-shell.py:3740-3754
        QStringList containerIds;
        for (int i = 0; i < item->childCount(); ++i) {
            const QString containerId = item->child(i)->text(ColId);
            if (!containerId.isEmpty())
                containerIds.append(containerId);
        }

        connect(actionStop, &QAction::triggered, this, [this, containerIds] {
            startDockerOperation(QStringLiteral("stop"), containerIds);
        });
        connect(actionRestart, &QAction::triggered, this, [this, containerIds] {
            startDockerOperation(QStringLiteral("restart"), containerIds);
        });
        connect(actionRemove, &QAction::triggered, this, [this, containerIds] {
            startDockerOperation(QStringLiteral("rm"), containerIds);
        });
        // 父级菜单禁用终端和日志功能（不能同时查看多个容器）
        actionTerminal->setEnabled(false);
        actionLogs->setEnabled(false);
    } else {
        // 子级：单容器全功能。对应Python: cube-shell.py:3756-3762
        const QString containerId = item->text(ColId);
        connect(actionStop, &QAction::triggered, this, [this, containerId] {
            startDockerOperation(QStringLiteral("stop"), {containerId});
        });
        connect(actionRestart, &QAction::triggered, this, [this, containerId] {
            startDockerOperation(QStringLiteral("restart"), {containerId});
        });
        connect(actionRemove, &QAction::triggered, this, [this, containerId] {
            startDockerOperation(QStringLiteral("rm"), {containerId});
        });
        // 对应Python: cube-shell.py:3767-3785 execDockerTerminal / viewDockerLogs
        connect(actionTerminal, &QAction::triggered, this, [this, containerId] {
            emit terminalCommandRequested(
                QStringLiteral("docker exec -ti %1 /bin/bash\n").arg(containerId));
        });
        connect(actionLogs, &QAction::triggered, this, [this, containerId] {
            emit terminalCommandRequested(
                QStringLiteral("docker logs -n 100 -f %1\n").arg(containerId));
        });
    }

    // 对应Python: cube-shell.py:3765 popup(QCursor.pos())
    menu->popup(QCursor::pos());
}

// ---------------------------------------------------------------------------
// 操作流程
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py:5222-5257 stop/restart/rmDockerContainer + _start_docker_operation
void DockerManagerDialog::startDockerOperation(const QString &op, const QStringList &ids)
{
    // 对应Python: cube-shell.py:5223 if container_ids 守卫
    if (ids.isEmpty())
        return;

    const QString opName = operationDisplayName(op);
    // 记录本次操作的容器ID，供失败回调恢复状态列（失败时 map 为空）。
    m_lastOperationIds = ids;
    // 先标记被操作的容器为"操作中"状态
    markContainersOperating(ids, QStringLiteral("%1中...").arg(opName));

    m_manager->containerOperation(op, ids);
}

// 对应Python: cube-shell.py:5259-5276 _mark_containers_operating
void DockerManagerDialog::markContainersOperating(const QStringList &ids, const QString &statusText,
                                                  bool highlight)
{
    const QColor background = highlight ? QColor(255, 193, 7, 80) : QColor(0, 0, 0, 0);

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *projectItem = m_tree->topLevelItem(i);

        // 检查子容器
        for (int j = 0; j < projectItem->childCount(); ++j) {
            QTreeWidgetItem *containerItem = projectItem->child(j);
            if (ids.contains(containerItem->text(ColId))) {
                containerItem->setText(ColState, statusText);
                containerItem->setBackground(ColState, background);
            }
        }

        // 检查顶层容器
        if (ids.contains(projectItem->text(ColId))) {
            projectItem->setText(ColState, statusText);
            projectItem->setBackground(ColState, background);
        }
    }
}

// 对应Python: cube-shell.py:5278-5305 _update_container_info_in_tree
void DockerManagerDialog::updateContainerInfoInTree(const DockerStatePortsMap &idToStatePorts)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *projectItem = m_tree->topLevelItem(i);

        // 检查子容器
        for (int j = 0; j < projectItem->childCount(); ++j) {
            QTreeWidgetItem *containerItem = projectItem->child(j);
            const auto it = idToStatePorts.constFind(containerItem->text(ColId));
            if (it != idToStatePorts.constEnd()) {
                containerItem->setText(ColState, it->first);
                containerItem->setText(ColPorts, it->second);
                // 清除高亮背景
                containerItem->setBackground(ColState, QColor(0, 0, 0, 0));
            }
        }

        // 检查顶层容器
        const auto it = idToStatePorts.constFind(projectItem->text(ColId));
        if (it != idToStatePorts.constEnd()) {
            projectItem->setText(ColState, it->first);
            projectItem->setText(ColPorts, it->second);
            projectItem->setBackground(ColState, QColor(0, 0, 0, 0));
        }
    }
}

// 对应Python: cube-shell.py:5307-5338 _remove_containers_from_tree
void DockerManagerDialog::removeContainersFromTree(const QStringList &ids)
{
    QList<QPair<QTreeWidgetItem *, int>> itemsToRemove; // (parent, index)，parent 为空表示顶层

    // 收集要删除的项
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *projectItem = m_tree->topLevelItem(i);

        // 检查子容器
        for (int j = projectItem->childCount() - 1; j >= 0; --j) {
            if (ids.contains(projectItem->child(j)->text(ColId)))
                itemsToRemove.append({projectItem, j});
        }

        // 检查顶层容器
        if (ids.contains(projectItem->text(ColId)))
            itemsToRemove.append({nullptr, i});
    }

    // 从后往前删除，避免索引变化
    for (auto it = itemsToRemove.crbegin(); it != itemsToRemove.crend(); ++it) {
        if (it->first)
            delete it->first->takeChild(it->second);
        else
            delete m_tree->takeTopLevelItem(it->second);
    }

    // 清理空的项目组（含 default 组）：没有子容器且不是独立容器行
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *projectItem = m_tree->topLevelItem(i);
        if (projectItem->childCount() == 0 && projectItem->text(ColId).isEmpty())
            delete m_tree->takeTopLevelItem(i);
    }
}

// 对应Python: cube-shell.py:5340-5362 _on_docker_operation_finished
void DockerManagerDialog::onContainerOperationFinished(bool success, const QString &op,
                                                       const cubeshell::DockerStatePortsMap &idToStatePorts)
{
    const QString opName = operationDisplayName(op);

    if (success) {
        if (op == QLatin1String("rm")) {
            // 删除操作：从列表中移除容器（rm 的 value 为空对，只用 keys）
            removeContainersFromTree(idToStatePorts.keys());
        } else {
            // 其他操作：更新状态和端口
            updateContainerInfoInTree(idToStatePorts);
        }
    } else {
        // 操作失败，状态列置"操作失败"并清除高亮，弹警告框。
        // 失败时 idToStatePorts 为空 map，改用 m_lastOperationIds 定位行，
        // 避免状态列永久停留在"停止中..."等操作中文案。
        // 对应Python: cube-shell.py:5359-5361 for cid in container_info.keys()
        markContainersOperating(m_lastOperationIds, QStringLiteral("操作失败"),
                                /*highlight=*/false);
        // 对应Python: cube-shell.py:5362 self.alarm(f"容器{op_name}失败")
        QMessageBox alarmBox(this);
        alarmBox.setWindowTitle(tr("操作失败"));
        alarmBox.setText(QStringLiteral("容器%1失败").arg(opName));
        alarmBox.setIcon(QMessageBox::Warning);
        alarmBox.exec();
    }
}

} // namespace cubeshell
