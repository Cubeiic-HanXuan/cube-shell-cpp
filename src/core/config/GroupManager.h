#pragma once

// GroupManager.h — device group configuration management.
//
// 对应Python: core/group_manager.py
//
// Persists to groups.json inside the user config dir. On-disk format is fully
// interoperable with the Python side:
//     {
//         "groups": ["华东地区", "华南地区"],
//         "device_group_map": { "设备A": "华东地区", ... }
//     }
// Like the Python module, every operation reads the file fresh and writes it
// back immediately, so both apps can share the file without a daemon.

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {

// In-memory mirror of the groups.json document.
// 对应Python: core/group_manager.py::_empty_data (结构定义)
struct GroupData {
    QStringList groups;                     // "groups": ordered group names
    QHash<QString, QString> deviceGroupMap; // "device_group_map": device -> group
};

// One entry of the grouped-devices view; order matters (groups order, then
// "__ungrouped__" last), so a list is used instead of a hash.
struct GroupedDevices {
    QString group;       // group name, or GroupManager::kUngrouped
    QStringList devices;
};

class GroupManager {
public:
    // Sentinel group name for devices without a group.
    // 对应Python: core/group_manager.py::get_grouped_devices 的 "__ungrouped__"
    static const QString kUngrouped;

    // filePath empty -> GlobalState::configDir()/groups.json (Python default).
    explicit GroupManager(const QString &filePath = QString());

    QString filePath() const { return m_filePath; }

    // 对应Python: core/group_manager.py::load_groups
    // Missing file or bad JSON yields the empty structure (never fails).
    GroupData loadGroups() const;

    // 对应Python: core/group_manager.py::save_groups
    bool saveGroups(const GroupData &data, QString *errorOut = nullptr) const;

    // 对应Python: core/group_manager.py::create_group
    bool createGroup(const QString &name);

    // 对应Python: core/group_manager.py::rename_group
    bool renameGroup(const QString &oldName, const QString &newName);

    // 对应Python: core/group_manager.py::delete_group
    void deleteGroup(const QString &name);

    // 对应Python: core/group_manager.py::move_device_to_group
    void moveDeviceToGroup(const QString &deviceName, const QString &groupName);

    // 对应Python: core/group_manager.py::remove_device_from_group
    void removeDeviceFromGroup(const QString &deviceName);

    // 对应Python: core/group_manager.py::get_device_group (无分组返回空串)
    QString deviceGroup(const QString &deviceName) const;

    // 对应Python: core/group_manager.py::get_grouped_devices
    // Every known group is present (possibly empty); kUngrouped appears last
    // and only when there are ungrouped devices.
    QList<GroupedDevices> groupedDevices(const QStringList &allDeviceNames) const;

    // 对应Python: core/group_manager.py::on_device_deleted
    void onDeviceDeleted(const QString &deviceName);

    // 对应Python: core/group_manager.py::on_device_renamed
    void onDeviceRenamed(const QString &oldName, const QString &newName);

private:
    QString m_filePath;
};

} // namespace cubeshell
