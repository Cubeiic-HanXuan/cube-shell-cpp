#include "device_list_widget.h"

#include <QCheckBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QStyle>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

// Item payload roles. 对应Python: item.setData(0, Qt.UserRole, "group"/"device")
static constexpr int kNameRole = Qt::UserRole;      // device name / group name
static constexpr int kTypeRole = Qt::UserRole + 1;  // "group" | "device"

static const QLatin1String kTypeGroup("group");
static const QLatin1String kTypeDevice("device");

namespace {

// 分组节点字体：粗体，macOS 下 15pt，其它平台 14pt。
// 对应Python: refreshConf::_make_group_font
QFont groupFont()
{
    QFont f;
#ifdef Q_OS_MACOS
    f.setPointSize(15);
#else
    f.setPointSize(14);
#endif
    f.setBold(true);
    return f;
}

// 设备节点字体：macOS 下 15pt 粗体，其它平台 14pt 常规。
// 对应Python: refreshConf::_make_device_font
QFont deviceFont()
{
    QFont f;
#ifdef Q_OS_MACOS
    f.setPointSize(15);
    f.setBold(true);
#else
    f.setPointSize(14);
#endif
    return f;
}

} // namespace

DeviceListWidget::DeviceListWidget(QWidget *parent)
    : QWidget(parent)
{
    // 单列树 + "设备列表" 表头，与 Python 侧完全一致。
    // 对应Python: ui/main.py::treeWidget + refreshConf
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(1);
    m_tree->setHeaderLabels({tr("设备列表")});
    m_tree->setRootIsDecorated(true);   // 分组节点带展开箭头
    m_tree->setIndentation(20);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setSelectionMode(QTreeWidget::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // 优化左侧图标显示间距。对应Python: treeWidget 的 item padding-left
    m_tree->setStyleSheet(QStringLiteral("QTreeWidget::item { padding-left: 5px; }"));

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setVisible(false);   // 只有确有提示时才占位

    // 对应Python: ui.follow_folder / ui.remote_monitoring（左侧面板底部复选框）
    m_followFolder = new QCheckBox(tr("跟随终端目录"), this);
    m_followFolder->setToolTip(tr("文件浏览器跟随 Shell 当前工作目录"));
    m_remoteMonitoring = new QCheckBox(tr("远程监控"), this);
    m_remoteMonitoring->setToolTip(tr("采集远程主机的 CPU / 内存 / 网络指标"));
    m_remoteMonitoring->setChecked(false);

    // 复选框水平排列，间距 15px。对应Python: checkbox_row_layout
    auto *checkboxRow = new QHBoxLayout;
    checkboxRow->setContentsMargins(0, 0, 0, 0);
    checkboxRow->setSpacing(15);
    checkboxRow->addWidget(m_followFolder);
    checkboxRow->addWidget(m_remoteMonitoring);
    checkboxRow->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_tree, 1);
    layout->addLayout(checkboxRow);
    layout->addWidget(m_status);

    setSessionActive(false);   // 无连接时隐藏复选框

    connect(m_followFolder, &QCheckBox::toggled,
            this, &DeviceListWidget::followFolderToggled);
    connect(m_remoteMonitoring, &QCheckBox::toggled,
            this, &DeviceListWidget::remoteMonitoringToggled);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &DeviceListWidget::onContextMenu);
    // 双击是打开设备的主要操作；仅连接 itemDoubleClicked。
    // 注意：不能同时连接 itemActivated，因为双击也触发 itemActivated，
    // 导致 slot 被调用两次。对应Python: treeWidget.doubleClicked.connect(self.cd)
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &DeviceListWidget::onItemActivated);

    // 初始化防抖计时器（对应Python: _last_connect_attempt_ts = 0）
    m_activateTimer.start();
}

void DeviceListWidget::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

// 对应Python: cube-shell.py 里连接成功后 show()/断开后 hide() 两个复选框
void DeviceListWidget::setSessionActive(bool active)
{
    m_followFolder->setVisible(active);
    m_remoteMonitoring->setVisible(active);
}

// 浏览器模式：隐藏设备树，仅保留底部复选框行（文件浏览器占据左栏）。
// 对应Python: 连接后 treeWidget 改为展示远程文件树，复选框固定在底部
void DeviceListWidget::setBrowserMode(bool on)
{
    m_tree->setVisible(!on);
    m_status->setVisible(!on && !m_status->text().isEmpty());
}

QString DeviceListWidget::selectedName() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item || item->data(0, kTypeRole).toString() != kTypeDevice)
        return QString();
    return item->data(0, kNameRole).toString();
}

bool DeviceListWidget::followFolderEnabled() const
{
    return m_followFolder->isChecked();
}

bool DeviceListWidget::remoteMonitoringEnabled() const
{
    return m_remoteMonitoring->isChecked();
}

// 对应Python: cube-shell.py::treeRight（分组/设备/空白三种右键菜单）
void DeviceListWidget::onContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    const QString type = item ? item->data(0, kTypeRole).toString() : QString();
    const QString name = item ? item->data(0, kNameRole).toString() : QString();
    QMenu menu(this);

    if (type == kTypeDevice) {
        // 设备节点：连接 / 编辑配置 / 删除配置 / 移到分组
        menu.addAction(tr("连接"), this, [this, item]() { onItemActivated(item, 0); });
        menu.addAction(tr("编辑配置"), this, [this, name]() { emit editRequested(name); });
        menu.addAction(tr("删除配置"), this, [this, name]() { emit removeRequested(name); });
        menu.addSeparator();
        QMenu *moveMenu = menu.addMenu(tr("移到分组"));
        const GroupData data = m_groups.loadGroups();
        for (const QString &group : data.groups) {
            moveMenu->addAction(group, this, [this, name, group]() {
                m_groups.moveDeviceToGroup(name, group);
                rebuildTree();
            });
        }
        if (!data.groups.isEmpty())
            moveMenu->addSeparator();
        moveMenu->addAction(tr("移出分组"), this, [this, name]() {
            m_groups.removeDeviceFromGroup(name);
            rebuildTree();
        });
    } else if (type == kTypeGroup) {
        // 分组节点：重命名 / 删除分组（"未分组"不允许）/ 新建子设备 / 新建分组
        const bool ungrouped = (name == GroupManager::kUngrouped);
        if (!ungrouped) {
            menu.addAction(tr("重命名分组"), this, [this, name]() { renameGroup(name); });
            menu.addAction(tr("删除分组"), this, [this, name]() { deleteGroup(name); });
            menu.addSeparator();
        }
        menu.addAction(tr("添加配置"), this, &DeviceListWidget::addRequested);
        menu.addAction(tr("新建分组"), this, &DeviceListWidget::createGroup);
    } else {
        // 空白区域：新建分组 / 新建设备 / 本机终端 / 保存
        menu.addAction(tr("添加配置"), this, &DeviceListWidget::addRequested);
        menu.addAction(tr("新建分组"), this, &DeviceListWidget::createGroup);
        menu.addSeparator();
#ifdef CUBESHELL_WITH_LOCALPTY
        // 鸿蒙：无本地 shell，「新建本机终端」入口摘除。
        menu.addAction(tr("新建本机终端"), this,
                       &DeviceListWidget::localTerminalRequested);
#endif
        menu.addAction(tr("保存设备配置"), this, &DeviceListWidget::saveRequested);
    }

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void DeviceListWidget::setDevices(const QList<DeviceEntry> &devices)
{
    m_devices = devices;
    rebuildTree();
}

// 对应Python: refreshConf 里按 get_grouped_devices 组装 treeWidget
void DeviceListWidget::rebuildTree()
{
    m_tree->clear();

    QStringList names;
    QHash<QString, const DeviceEntry *> byName;
    for (const DeviceEntry &d : m_devices) {
        names << d.name;
        byName.insert(d.name, &d);
    }

    const QList<GroupedDevices> grouped = m_groups.groupedDevices(names);
    for (const GroupedDevices &g : grouped) {
        const bool ungrouped = (g.group == GroupManager::kUngrouped);
        auto *root = new QTreeWidgetItem(m_tree);
        root->setText(0, ungrouped ? tr("未分组") : g.group);
        root->setFont(0, groupFont());
        // 分组用系统文件夹图标。对应Python: style().standardIcon(SP_DirIcon)
        root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        root->setData(0, kNameRole, g.group);
        root->setData(0, kTypeRole, QString(kTypeGroup));
        for (const QString &deviceName : g.devices) {
            const DeviceEntry *d = byName.value(deviceName);
            if (!d)
                continue;
            const HostPort hp = d->hostPort();
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, d->name);
            item->setFont(0, deviceFont());
            // 对应Python: cube-shell.py:3966-3994 — RDP 设备用 Windows 图标，
            // Serial 设备用 icons8-serial-48.png，SSH 设备用 icons8-ssh-48.png
            if (d->isRdp())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-windows-48.png")));
            else if (d->isSerial())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-serial-48.png")));
            else
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-ssh-48.png")));
            item->setToolTip(0, d->username + QLatin1Char('@') + hp.host +
                                QStringLiteral(":%1").arg(hp.port));
            item->setData(0, kNameRole, d->name);
            item->setData(0, kTypeRole, QString(kTypeDevice));
        }
    }
    m_tree->expandAll();
}

void DeviceListWidget::onItemActivated(QTreeWidgetItem *item, int /*column*/)
{
    // 防抖：500ms 内重复触发直接忽略。
    // 对应Python: cd() 中 if now_ms - _last_connect_attempt_ts < 800: return
    if (m_activateTimer.elapsed() < kActivateDebounceMs)
        return;

    // Groups toggle expansion on double-click; only devices open a session.
    if (!item || item->data(0, kTypeRole).toString() != kTypeDevice)
        return;
    DeviceEntry d;
    d.name = item->data(0, kNameRole).toString();
    // Credentials / host are looked up by the receiver from the store by name.

    // 重置防抖计时器（对应Python: self._last_connect_attempt_ts = now_ms）
    m_activateTimer.restart();

    emit activated(d);
}

// ---------------------------------------------------------------------------
// 分组操作。对应Python: _create_new_group / _rename_group / _delete_group
// ---------------------------------------------------------------------------

void DeviceListWidget::createGroup()
{
    const QString name = QInputDialog::getText(this, tr("新建分组"), tr("请输入分组名称"));
    if (name.trimmed().isEmpty())
        return;
    if (!m_groups.createGroup(name.trimmed())) {
        QMessageBox::warning(this, tr("警告"), tr("分组已存在"));
        return;
    }
    rebuildTree();
}

void DeviceListWidget::renameGroup(const QString &oldName)
{
    const QString newName = QInputDialog::getText(
        this, tr("重命名分组"), tr("请输入分组名称"), QLineEdit::Normal, oldName);
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty() || trimmed == oldName)
        return;
    if (!m_groups.renameGroup(oldName, trimmed)) {
        QMessageBox::warning(this, tr("警告"), tr("分组已存在"));
        return;
    }
    rebuildTree();
}

void DeviceListWidget::deleteGroup(const QString &name)
{
    if (QMessageBox::question(this, tr("确认删除"),
                              tr("确定要删除分组吗？") + QLatin1Char('\n')
                                  + tr("分组内的设备将移至未分组"))
            != QMessageBox::Yes)
        return;
    m_groups.deleteGroup(name);
    rebuildTree();
}

} // namespace cubeshell
