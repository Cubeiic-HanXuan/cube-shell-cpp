#pragma once

// FrequentlyUsedCommands.h — frequently-used Linux command reference data.
//
// 对应Python: core/frequently_used_commands.py
//
// The Python module is a QTreeView UI over conf/linux_commands.json; this
// class ports its data layer: loading/saving the JSON tree and the recursive,
// case-insensitive filtering performed by TreeFilterProxyModel. On-disk
// format is identical to the Python side:
//     { "treeData": [ { "command": ..., "option": ..., "description": ...,
//                       "children": [ ... ] }, ... ] }

#include <QList>
#include <QString>

namespace cubeshell {

// One node of the command tree (category rows have children, leaf rows are
// concrete commands).
// 对应Python: core/frequently_used_commands.py::TreeSearchApp.add_items 的 element 结构
struct CommandEntry {
    QString command;
    QString option;
    QString description;
    QList<CommandEntry> children;

    bool hasChildren() const { return !children.isEmpty(); }
};

class FrequentlyUsedCommands {
public:
    FrequentlyUsedCommands() = default;

    // linux_commands.json 的运行期定位（LinuxCommandsDialog 与 CommandIndex 共用）：
    //   应用目录/conf → macOS bundle 的 Contents/Resources/conf →
    //   安装布局 <prefix>/share/cube-shell/conf → cwd/conf →
    //   源码树 conf/（开发期，CMake 注入 CUBESHELL_SOURCE_CONF_DIR）→
    //   编进二进制的 ":/conf/linux_commands.json"（conf/conf.qrc）。
    // 磁盘副本优先，用户在应用旁放一份 conf/linux_commands.json 即可覆盖内置
    // 数据；打包产物不带 conf/ 目录，靠最后的 qrc 兜底，故返回值永不为空。
    // 对应Python: function/ssh_prompt_client.py::_default_linux_commands_path
    static QString defaultPath();

    // 对应Python: core/frequently_used_commands.py::TreeSearchApp.load_data_from_json
    bool load(const QString &filePath, QString *errorOut = nullptr);

    // Write the tree back in the Python-compatible {"treeData": [...]} shape.
    // (Python 侧只读该文件；写出格式与其 json.load 期望完全一致)
    bool save(const QString &filePath, QString *errorOut = nullptr) const;

    const QList<CommandEntry> &entries() const { return m_entries; }
    void setEntries(const QList<CommandEntry> &entries) { m_entries = entries; }
    bool isEmpty() const { return m_entries.isEmpty(); }

    // Recursive case-insensitive substring search over command/option/
    // description; a matching parent keeps its whole subtree, a matching
    // child keeps its ancestors (same visible result as the proxy model).
    // 对应Python: core/frequently_used_commands.py::TreeFilterProxyModel.filterAcceptsRow
    QList<CommandEntry> filter(const QString &searchText) const;

    // Depth-first lookup of an exact command name; nullptr when absent.
    const CommandEntry *find(const QString &command) const;

private:
    QList<CommandEntry> m_entries;
};

} // namespace cubeshell
