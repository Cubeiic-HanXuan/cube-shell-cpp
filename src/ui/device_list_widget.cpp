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

#include "config/GlobalState.h"

namespace cubeshell {

// Item payload roles. 对应Python: item.setData(0, Qt.UserRole, "group"/"device")
static constexpr int kNameRole = Qt::UserRole;      // device name / group name
static constexpr int kTypeRole = Qt::UserRole + 1;  // "group" | "device"

static const QLatin1String kTypeGroup("group");
static const QLatin1String kTypeDevice("device");

namespace {

// 分组节点字体：粗体。字号来自设置（缺省 macOS 15pt，其它平台 14pt）。
// 对应Python: refreshConf::_make_group_font
QFont groupFont(int pointSize)
{
    QFont f;
    f.setPointSize(pointSize);
    f.setBold(true);
    return f;
}

// 设备节点字体：macOS 下加粗，其它平台常规。字号来自设置。
// 对应Python: refreshConf::_make_device_font
QFont deviceFont(int pointSize)
{
    QFont f;
    f.setPointSize(pointSize);
#ifdef Q_OS_MACOS
    f.setBold(true);
#endif
    return f;
}

// 设备节点的悬浮提示：按协议拼，各协议展示它自己有意义的字段。
//
// 早先这里无条件拼 "user@host:port"。串口条目本就没有 host/user，显示成
// "@:22" 毫无信息量；Telnet 常常没有用户名（很多设备只问密码），裸 TCP
// 更是没有登录概念，同样会拼出一个带 '@' 的假地址。
QString deviceTooltip(const DeviceEntry &d)
{
    if (d.isSerial()) {
        // 串口没有网络地址，有意义的是设备路径与帧格式。
        return QStringLiteral("%1 @%2 %3%4%5")
            .arg(d.portName.isEmpty() ? QObject::tr("(未指定串口)") : d.portName)
            .arg(d.baudRate)
            .arg(d.dataBits)
            // 校验位取首字母大写（none→N / even→E / odd→O / mark→M / space→S），
            // 与串口面板状态栏的 frameFormat() 同一记法。
            .arg(d.parity.isEmpty() ? QStringLiteral("N")
                                    : d.parity.left(1).toUpper())
            .arg(d.stopBits.isEmpty() ? QStringLiteral("1") : d.stopBits);
    }

    const HostPort hp = d.hostPort();
    const QString target = formatHostPort(hp.host, hp.port);
    // 裸 TCP 没有用户名概念；其余协议只在真有用户名时才带 "user@"。
    if (d.isTcp() || d.username.isEmpty())
        return target;
    return d.username + QLatin1Char('@') + target;
}

} // namespace

DeviceListWidget::DeviceListWidget(QWidget *parent)
    : QWidget(parent)
{
    // 设备列表字号：持久化在 theme.json（GlobalState），与设置对话框共享。
    m_fontSize = GlobalState::instance().deviceListFontSize();

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
    // 显式设置图标尺寸（随字号走）。鸿蒙高分屏上若不设，树会取样式默认的
    // PM_SmallIconSize——该值依赖平台主题的密度换算，而鸿蒙平台插件的密度/DPI
    // 上报有缺陷，解析得偏小，这就是设备列表图标「特别小」的根因。显式给逻辑像素
    // 尺寸后由 Qt 按 devicePixelRatio 正确放大（与左侧工具栏 setIconSize(24) 同理）。
    updateIconSize();

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

QStringList DeviceListWidget::selectedDeviceNames() const
{
    QStringList out;
    const QList<QTreeWidgetItem *> items = m_tree->selectedItems();
    out.reserve(items.size());
    for (const QTreeWidgetItem *item : items) {
        if (item->data(0, kTypeRole).toString() != kTypeDevice)
            continue;   // 分组节点也可能被框进选区，批量操作不管它
        const QString name = item->data(0, kNameRole).toString();
        if (!name.isEmpty() && !out.contains(name))
            out << name;
    }
    return out;
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
    // 右键点在选区之外时，把选区收敛到被点的那个节点。
    //
    // 树是 ExtendedSelection，但右键本身不改选区：不做这一步，菜单作用的对象就
    // 取决于一份用户看不见的旧选区，而高亮的却是另外几行——「删了不该删的」。
    // 点在选区之内则保留整个多选，这样多选后右键其中一台仍是批量操作。
    // （与文件管理器/IDE 一致）
    if (item && !item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
        m_tree->setCurrentItem(item);
    }

    const QString type = item ? item->data(0, kTypeRole).toString() : QString();
    const QString name = item ? item->data(0, kNameRole).toString() : QString();
    // 批量操作（删除 / 移到分组）的目标：整个选区，而不是光标下那一个。
    const QStringList names = selectedDeviceNames();
    QMenu menu(this);

    if (type == kTypeDevice) {
        // 设备节点：连接 / 编辑配置 / 删除配置 / 移到分组
        // 连接与编辑仍只作用于被点的那台——多开 N 个会话、弹 N 个编辑框都不是这里该做的事。
        menu.addAction(tr("连接"), this, [this, item]() { onItemActivated(item, 0); });
        menu.addAction(tr("编辑配置"), this, [this, name]() { emit editRequested(name); });
        // 菜单文案带上条数：批量删除不可撤销，先让人在点下去之前看到范围。
        menu.addAction(names.size() > 1 ? tr("删除配置（%1 项）").arg(names.size())
                                        : tr("删除配置"),
                       this, [this, names]() { emit removeRequested(names); });
        menu.addSeparator();
        QMenu *moveMenu = menu.addMenu(names.size() > 1
                                           ? tr("移到分组（%1 项）").arg(names.size())
                                           : tr("移到分组"));
        const GroupData data = m_groups.loadGroups();
        for (const QString &group : data.groups) {
            moveMenu->addAction(group, this, [this, names, group]() {
                for (const QString &n : names)
                    m_groups.moveDeviceToGroup(n, group);
                rebuildTree();
            });
        }
        if (!data.groups.isEmpty())
            moveMenu->addSeparator();
        moveMenu->addAction(tr("移出分组"), this, [this, names]() {
            for (const QString &n : names)
                m_groups.removeDeviceFromGroup(n);
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
        root->setFont(0, groupFont(m_fontSize));
        // 分组用系统文件夹图标。对应Python: style().standardIcon(SP_DirIcon)
        root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        root->setData(0, kNameRole, g.group);
        root->setData(0, kTypeRole, QString(kTypeGroup));
        for (const QString &deviceName : g.devices) {
            const DeviceEntry *d = byName.value(deviceName);
            if (!d)
                continue;
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, d->name);
            item->setFont(0, deviceFont(m_fontSize));
            // 对应Python: cube-shell.py:3966-3994 — RDP 设备用 Windows 图标，
            // Serial 设备用 icons8-serial-48.png，SSH 设备用 icons8-ssh-48.png
            // Telnet/TCP 各有自己的图标；两者必须排在 SSH 兜底分支之前判断。
            if (d->isRdp())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-windows-48.png")));
            else if (d->isSerial())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-serial-48.png")));
            else if (d->isTelnet())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-telnet-48.png")));
            else if (d->isTcp())
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-tcp-48.png")));
            else
                item->setIcon(0, QIcon(QStringLiteral(":/icons8-ssh-48.png")));
            item->setToolTip(0, deviceTooltip(*d));
            item->setData(0, kNameRole, d->name);
            item->setData(0, kTypeRole, QString(kTypeDevice));
        }
    }
    m_tree->expandAll();
}

// 设置字号并即时重设已有节点字体。不重建树，避免收起用户已展开的分组。
void DeviceListWidget::setFontSize(int pointSize)
{
    if (pointSize <= 0 || pointSize == m_fontSize)
        return;
    m_fontSize = pointSize;
    applyFonts();
}

void DeviceListWidget::applyFonts()
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = m_tree->topLevelItem(i);
        root->setFont(0, groupFont(m_fontSize));
        for (int j = 0; j < root->childCount(); ++j)
            root->child(j)->setFont(0, deviceFont(m_fontSize));
    }
    updateIconSize();
}

// 图标边长随字号走：pt → 逻辑像素按 ×96/72（14pt ≈ 19px）。显式尺寸在鸿蒙高密度
// 屏上能被正确放大，避免样式默认小图标在鸿蒙上解析偏小（见构造函数注释）。
void DeviceListWidget::updateIconSize()
{
    const int edge = qRound(m_fontSize * 96.0 / 72.0);
    m_tree->setIconSize(QSize(edge, edge));
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
