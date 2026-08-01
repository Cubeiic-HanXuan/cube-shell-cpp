#pragma once

// DeviceListWidget.h — device/connection list panel (vertical slice).
//
// Shows the saved devices grouped by GroupManager (root nodes = groups,
// children = devices), mirroring cube-shell.py's treeWidget + treeRight()
// context menu. Emits activated() when the user double-clicks a device:
//   device list -> double-click -> SSH connect -> terminal tab.
// Also hosts the follow_folder / remote_monitoring checkboxes shown at the
// bottom of the left panel in the Python UI.

#include <QElapsedTimer>
#include <QWidget>

#include "config/DeviceConfigStore.h"
#include "config/GroupManager.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QCheckBox;

namespace cubeshell {

class DeviceListWidget : public QWidget {
    Q_OBJECT
public:
    explicit DeviceListWidget(QWidget *parent = nullptr);

    // Replace the shown devices (rebuilds the grouped tree).
    void setDevices(const QList<DeviceEntry> &devices);
    // Status line shown under the list (e.g. "3 devices · loaded from ...").
    void setStatus(const QString &text);
    // Currently selected device name (empty if none or a group is selected).
    QString selectedName() const;

    // 对应Python: ui.follow_folder / ui.remote_monitoring 复选框状态
    bool followFolderEnabled() const;
    bool remoteMonitoringEnabled() const;
    // 无连接时两个复选框隐藏，与 Python 侧一致。
    // 对应Python: cube-shell.py 里 follow_folder.hide() / remote_monitoring.hide()
    void setSessionActive(bool active);
    // 浏览器模式：连接后文件浏览器替换设备列表，本控件仅保留底部复选框。
    // 对应Python: 连接后 treeWidget 改展示远程文件树，复选框行固定在底部
    void setBrowserMode(bool on);

signals:
    // User double-clicked a device; the main window opens an SSH tab.
    void activated(const cubeshell::DeviceEntry &device);
    // User clicked the "local terminal" button.
    void localTerminalRequested();
    // CRUD requests (handled by the main window, which owns the store).
    void addRequested();
    void editRequested(const QString &name);
    void removeRequested(const QString &name);
    // Persist the current store (Save button) -> main window writes JSON.
    void saveRequested();
    // 对应Python: _on_follow_folder_changed / _on_remote_monitoring_changed
    void followFolderToggled(bool enabled);
    void remoteMonitoringToggled(bool enabled);

private slots:
    void onItemActivated(QTreeWidgetItem *item, int column);
    void onContextMenu(const QPoint &pos);

private:
    // Rebuild the tree from m_devices + GroupManager::groupedDevices().
    void rebuildTree();
    // 对应Python: _create_new_group / _rename_group / _delete_group
    void createGroup();
    void renameGroup(const QString &oldName);
    void deleteGroup(const QString &name);

    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_followFolder = nullptr;
    QCheckBox *m_remoteMonitoring = nullptr;

    GroupManager m_groups;              // groups.json（与 Python 侧共用格式）
    QList<DeviceEntry> m_devices;       // cache for rebuildTree()

    // 防抖：抑制双击时 itemActivated + itemDoubleClicked 双触发。
    // 对应Python: cd() 中 is_connecting_lock + _last_connect_attempt_ts 节流
    QElapsedTimer m_activateTimer;
    static constexpr int kActivateDebounceMs = 500;
};

} // namespace cubeshell
