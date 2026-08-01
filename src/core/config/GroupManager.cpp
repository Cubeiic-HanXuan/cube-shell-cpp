// GroupManager.cpp — device group persistence. See GroupManager.h.

#include "GroupManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "GlobalState.h"

namespace cubeshell {

const QString GroupManager::kUngrouped = QStringLiteral("__ungrouped__");

// 对应Python: core/group_manager.py::_get_groups_file_path
GroupManager::GroupManager(const QString &filePath)
    : m_filePath(filePath.isEmpty() ? GlobalState::groupsConfigPath() : filePath)
{
}

// 对应Python: core/group_manager.py::load_groups
GroupData GroupManager::loadGroups() const
{
    GroupData data; // _empty_data(): {"groups": [], "device_group_map": {}}

    QFile f(m_filePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return data;

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return data; // json.JSONDecodeError -> empty structure

    const QJsonObject obj = doc.object();

    // Python tolerates missing / wrongly-typed keys — mirror that here.
    const QJsonValue groupsVal = obj.value(QStringLiteral("groups"));
    if (groupsVal.isArray()) {
        for (const QJsonValue &v : groupsVal.toArray()) {
            if (v.isString())
                data.groups.append(v.toString());
        }
    }
    const QJsonValue mapVal = obj.value(QStringLiteral("device_group_map"));
    if (mapVal.isObject()) {
        const QJsonObject mapObj = mapVal.toObject();
        for (auto it = mapObj.constBegin(); it != mapObj.constEnd(); ++it)
            data.deviceGroupMap.insert(it.key(), it.value().toString());
    }
    return data;
}

// 对应Python: core/group_manager.py::save_groups (ensure_ascii=False, indent=2)
bool GroupManager::saveGroups(const GroupData &data, QString *errorOut) const
{
    QJsonArray groups;
    for (const QString &g : data.groups)
        groups.append(g);

    QJsonObject map;
    for (auto it = data.deviceGroupMap.constBegin();
         it != data.deviceGroupMap.constEnd(); ++it)
        map.insert(it.key(), it.value());

    QJsonObject root;
    root.insert(QStringLiteral("groups"), groups);
    root.insert(QStringLiteral("device_group_map"), map);

    QSaveFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(m_filePath);
        return false;
    }
    // toJson emits UTF-8 without \u escapes — same as ensure_ascii=False.
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(m_filePath);
        return false;
    }
    return true;
}

// 对应Python: core/group_manager.py::create_group
bool GroupManager::createGroup(const QString &name)
{
    GroupData data = loadGroups();
    if (data.groups.contains(name))
        return false;
    data.groups.append(name);
    saveGroups(data);
    return true;
}

// 对应Python: core/group_manager.py::rename_group
bool GroupManager::renameGroup(const QString &oldName, const QString &newName)
{
    GroupData data = loadGroups();
    const int idx = data.groups.indexOf(oldName);
    if (idx < 0)
        return false;
    if (data.groups.contains(newName))
        return false;
    data.groups[idx] = newName;
    // Update every device that referenced the old name.
    for (auto it = data.deviceGroupMap.begin(); it != data.deviceGroupMap.end(); ++it) {
        if (it.value() == oldName)
            it.value() = newName;
    }
    saveGroups(data);
    return true;
}

// 对应Python: core/group_manager.py::delete_group
void GroupManager::deleteGroup(const QString &name)
{
    GroupData data = loadGroups();
    if (!data.groups.removeOne(name))
        return;
    // Devices of the deleted group fall back to "ungrouped".
    for (auto it = data.deviceGroupMap.begin(); it != data.deviceGroupMap.end();) {
        if (it.value() == name)
            it = data.deviceGroupMap.erase(it);
        else
            ++it;
    }
    saveGroups(data);
}

// 对应Python: core/group_manager.py::move_device_to_group
void GroupManager::moveDeviceToGroup(const QString &deviceName, const QString &groupName)
{
    GroupData data = loadGroups();
    data.deviceGroupMap.insert(deviceName, groupName);
    saveGroups(data);
}

// 对应Python: core/group_manager.py::remove_device_from_group
void GroupManager::removeDeviceFromGroup(const QString &deviceName)
{
    GroupData data = loadGroups();
    if (data.deviceGroupMap.remove(deviceName) > 0)
        saveGroups(data);
}

// 对应Python: core/group_manager.py::get_device_group
QString GroupManager::deviceGroup(const QString &deviceName) const
{
    return loadGroups().deviceGroupMap.value(deviceName);
}

// 对应Python: core/group_manager.py::get_grouped_devices
QList<GroupedDevices> GroupManager::groupedDevices(const QStringList &allDeviceNames) const
{
    const GroupData data = loadGroups();

    QList<GroupedDevices> result;
    result.reserve(data.groups.size() + 1);
    // Seed every known group (possibly empty), preserving order.
    for (const QString &group : data.groups)
        result.append({group, {}});

    QStringList ungrouped;
    for (const QString &deviceName : allDeviceNames) {
        const QString group = data.deviceGroupMap.value(deviceName);
        const int idx = group.isEmpty() ? -1 : int(data.groups.indexOf(group));
        if (idx >= 0)
            result[idx].devices.append(deviceName);
        else
            ungrouped.append(deviceName); // group unset or unknown
    }

    // "__ungrouped__" appears only when there are ungrouped devices.
    if (!ungrouped.isEmpty())
        result.append({kUngrouped, ungrouped});
    return result;
}

// 对应Python: core/group_manager.py::on_device_deleted
void GroupManager::onDeviceDeleted(const QString &deviceName)
{
    removeDeviceFromGroup(deviceName);
}

// 对应Python: core/group_manager.py::on_device_renamed
void GroupManager::onDeviceRenamed(const QString &oldName, const QString &newName)
{
    GroupData data = loadGroups();
    auto it = data.deviceGroupMap.find(oldName);
    if (it != data.deviceGroupMap.end()) {
        const QString group = it.value();
        data.deviceGroupMap.erase(it);
        data.deviceGroupMap.insert(newName, group);
        saveGroups(data);
    }
}

} // namespace cubeshell
