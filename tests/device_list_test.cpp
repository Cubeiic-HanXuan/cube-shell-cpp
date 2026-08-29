// 设备列表右键菜单的「作用范围」单测（离屏 GUI，无网络）。
//
// 回归的是这条真实故障：左侧设备列表多选若干设备后右键「删除配置」，只删掉了
// 一台。根因是菜单一路只认 itemAt(pos) 拿到的那个节点，树上的
// ExtendedSelection 形同虚设。
//
// 三条不变量钉在这里：
//   1. selectedDeviceNames() 收全选区里的设备，并把分组节点滤掉；
//   2. 右键落在选区**之内**时，删除带走整个选区（这就是那条 bug）；
//   3. 右键落在选区**之外**时，选区先收敛到被点的节点，只删它一个——
//      否则用户看到的高亮和实际删掉的东西是两码事，比少删更危险。
//
// 测试直接走 onContextMenu 真身（QMetaObject 调私有槽）并在菜单自己的事件循环
// 里触发菜单项，而不是复述一遍菜单的组装逻辑——后者对这个 bug 毫无约束力。

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "device_list_widget.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 菜单探针的落点。用文件作用域而不是栈上变量：QMenu::exec 万一没能进事件循环
// （离屏平台异常时），singleShot 会在之后某轮才触发，捕获栈地址就成了悬垂引用。
static QString g_actionText;
static QStringList g_removeNames;
static bool g_removeEmitted = false;
static bool g_actionFound = false;
static bool g_popupSeen = false;

static DeviceEntry makeDevice(const QString &name)
{
    DeviceEntry d;
    d.id = QStringLiteral("id-") + name;
    d.name = name;
    d.username = QStringLiteral("root");
    d.host = QStringLiteral("10.0.0.1");
    d.port = 22;
    return d;
}

static QTreeWidgetItem *deviceItem(QTreeWidget *tree, const QString &name)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = tree->topLevelItem(i);
        for (int j = 0; j < root->childCount(); ++j) {
            if (root->child(j)->text(0) == name)
                return root->child(j);
        }
    }
    return nullptr;
}

static QStringList sorted(QStringList in)
{
    in.sort();
    return in;
}

// 在 item 上打开右键菜单，找到「删除配置…」那一项，记下它的文案并（可选）触发它。
// 菜单是模态的（QMenu::exec），所以操作要排在它自己的事件循环里跑：先挂
// singleShot，再同步调 onContextMenu。
static void openMenuOn(DeviceListWidget *w, QTreeWidget *tree,
                       QTreeWidgetItem *item, bool trigger)
{
    g_actionText.clear();
    g_actionFound = false;
    g_popupSeen = false;
    g_removeNames.clear();
    g_removeEmitted = false;

    QTimer::singleShot(0, w, [trigger]() {
        QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
            return;
        g_popupSeen = true;
        for (QAction *act : menu->actions()) {
            if (!act->text().startsWith(QStringLiteral("删除配置")))
                continue;
            g_actionFound = true;
            g_actionText = act->text();
            if (trigger)
                act->trigger();
            break;
        }
        menu->close();
    });

    QMetaObject::invokeMethod(w, "onContextMenu", Qt::DirectConnection,
                              Q_ARG(QPoint, tree->visualItemRect(item).center()));
}

int main(int argc, char **argv)
{
    // 离屏平台：设备树要有真实几何（visualItemRect / itemAt 都按 viewport 坐标
    // 算），所以控件必须 show()，但不需要显示器。与其它 widget 单测同款。
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    // 隔离配置目录：DeviceListWidget 默认构造的 GroupManager 读的是真实
    // groups.json，测试不能碰用户的分组文件。必须在任何 configDir() 之前设。
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    DeviceListWidget widget;
    QList<DeviceEntry> devices;
    for (const QString &n : {QStringLiteral("dev-a"), QStringLiteral("dev-b"),
                             QStringLiteral("dev-c"), QStringLiteral("dev-d")})
        devices << makeDevice(n);
    widget.setDevices(devices);
    widget.resize(320, 480);
    widget.show();
    QApplication::processEvents();

    QObject::connect(&widget, &DeviceListWidget::removeRequested, &widget,
                     [](const QStringList &names) {
                         g_removeNames = names;
                         g_removeEmitted = true;
                     });

    QTreeWidget *tree = widget.findChild<QTreeWidget *>();
    CHECK(tree != nullptr);
    if (!tree)
        return 1;

    // 前提：多选是真的开着的。关掉它这个 bug 就"修不了"。
    CHECK(tree->selectionMode() == QAbstractItemView::ExtendedSelection);

    QTreeWidgetItem *a = deviceItem(tree, QStringLiteral("dev-a"));
    QTreeWidgetItem *b = deviceItem(tree, QStringLiteral("dev-b"));
    QTreeWidgetItem *c = deviceItem(tree, QStringLiteral("dev-c"));
    QTreeWidgetItem *d = deviceItem(tree, QStringLiteral("dev-d"));
    CHECK(a && b && c && d);
    if (!(a && b && c && d))
        return 1;
    CHECK(tree->topLevelItemCount() == 1);   // 都未分组 → 单个"未分组"根

    const QStringList abc{QStringLiteral("dev-a"), QStringLiteral("dev-b"),
                          QStringLiteral("dev-c")};

    // (1) 空选区 → 空列表；分组节点不算设备。
    CHECK(widget.selectedDeviceNames().isEmpty());
    tree->topLevelItem(0)->setSelected(true);
    CHECK(widget.selectedDeviceNames().isEmpty());
    tree->clearSelection();

    // (2) 三台设备 + 分组根一起选中：只收设备，分组被滤掉。
    a->setSelected(true);
    b->setSelected(true);
    c->setSelected(true);
    tree->topLevelItem(0)->setSelected(true);
    CHECK(sorted(widget.selectedDeviceNames()) == abc);

    // (3) 右键点在选区之内（dev-b）→ 删除带走整个选区。这就是原 bug 的复现点：
    //     修复前这里只会拿到 {"dev-b"}。
    openMenuOn(&widget, tree, b, /*trigger=*/true);
    CHECK(g_popupSeen);
    CHECK(g_actionFound);
    CHECK(g_removeEmitted);
    CHECK(sorted(g_removeNames) == abc);
    // 菜单文案要报出条数（用户点下去之前唯一的范围提示）。
    CHECK(g_actionText.contains(QStringLiteral("3")));
    // 选区不因右键而缩水。
    CHECK(sorted(widget.selectedDeviceNames()) == abc);

    // (4) 右键点在选区之外（dev-d 未被选）→ 选区收敛到 dev-d，只删它一个。
    openMenuOn(&widget, tree, d, /*trigger=*/true);
    CHECK(g_actionFound);
    CHECK(g_removeEmitted);
    CHECK(g_removeNames == QStringList({QStringLiteral("dev-d")}));
    CHECK(g_actionText == QStringLiteral("删除配置"));   // 单选不带条数
    CHECK(widget.selectedDeviceNames() == QStringList({QStringLiteral("dev-d")}));
    CHECK(tree->currentItem() == d);

    // (5) 只看菜单不点：不该发出任何删除请求（确认框之前不能有副作用）。
    openMenuOn(&widget, tree, a, /*trigger=*/false);
    CHECK(g_actionFound);
    CHECK(!g_removeEmitted);

    if (failures == 0)
        qInfo() << "device_list_test: all checks passed";
    return failures == 0 ? 0 : 1;
}
