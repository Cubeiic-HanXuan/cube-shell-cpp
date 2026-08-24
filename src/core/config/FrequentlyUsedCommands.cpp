// FrequentlyUsedCommands.cpp — command reference data. See FrequentlyUsedCommands.h.

#include "FrequentlyUsedCommands.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cubeshell {

// 探测顺序见头文件。最后一项是编进二进制的副本（conf/conf.qrc），QFile 能像
// 普通文件一样读 ":/" 路径，因此 load() 无需为它做任何特殊处理。
QString FrequentlyUsedCommands::defaultPath()
{
    const QString fileName = QStringLiteral("linux_commands.json");
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/conf/") + fileName
               // macOS bundle: Contents/MacOS -> Contents/Resources/conf/
               << appDir + QStringLiteral("/../Resources/conf/") + fileName
               // 安装布局: <prefix>/bin -> <prefix>/share/cube-shell/conf/
               //（CMakeLists.txt 的 install(DIRECTORY conf/) 就装在这里）
               << appDir + QStringLiteral("/../share/cube-shell/conf/") + fileName
               << QDir::currentPath() + QStringLiteral("/conf/") + fileName;
#ifdef CUBESHELL_SOURCE_CONF_DIR
    // 开发期: build/bin/... -> 源码树 conf/（CMake 注入）。
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/") + fileName;
#endif
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    return QStringLiteral(":/conf/") + fileName;
}

// 对应Python: core/frequently_used_commands.py::TreeSearchApp.add_items (JSON -> 树)
static QList<CommandEntry> parseEntries(const QJsonArray &arr)
{
    QList<CommandEntry> out;
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        CommandEntry e;
        e.command     = o.value(QStringLiteral("command")).toString();
        e.option      = o.value(QStringLiteral("option")).toString();
        e.description = o.value(QStringLiteral("description")).toString();
        const QJsonValue kids = o.value(QStringLiteral("children"));
        if (kids.isArray())
            e.children = parseEntries(kids.toArray());
        out.append(e);
    }
    return out;
}

static QJsonArray serializeEntries(const QList<CommandEntry> &entries)
{
    QJsonArray arr;
    for (const CommandEntry &e : entries) {
        QJsonObject o;
        o.insert(QStringLiteral("command"), e.command);
        o.insert(QStringLiteral("option"), e.option);
        o.insert(QStringLiteral("description"), e.description);
        // Python only reads "children" when present — omit it for leaves.
        if (e.hasChildren())
            o.insert(QStringLiteral("children"), serializeEntries(e.children));
        arr.append(o);
    }
    return arr;
}

// 对应Python: core/frequently_used_commands.py::TreeSearchApp.load_data_from_json
bool FrequentlyUsedCommands::load(const QString &filePath, QString *errorOut)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot open %1").arg(filePath);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) *errorOut = QStringLiteral("invalid JSON in %1: %2")
                                      .arg(filePath, perr.errorString());
        return false;
    }
    const QJsonValue tree = doc.object().value(QStringLiteral("treeData"));
    if (!tree.isArray()) {
        if (errorOut) *errorOut = QStringLiteral("missing \"treeData\" in %1").arg(filePath);
        return false;
    }
    m_entries = parseEntries(tree.toArray());
    return true;
}

bool FrequentlyUsedCommands::save(const QString &filePath, QString *errorOut) const
{
    QJsonObject root;
    root.insert(QStringLiteral("treeData"), serializeEntries(m_entries));

    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(filePath);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(filePath);
        return false;
    }
    return true;
}

// True when any of the three columns contains `needle` (case-insensitive) —
// same columns the proxy model scans.
static bool entryMatches(const CommandEntry &e, const QString &needle)
{
    return e.command.contains(needle, Qt::CaseInsensitive)
        || e.option.contains(needle, Qt::CaseInsensitive)
        || e.description.contains(needle, Qt::CaseInsensitive);
}

// 对应Python: core/frequently_used_commands.py::TreeFilterProxyModel.filterAcceptsRow
// A row is kept when it matches itself or any descendant matches (recursive
// filtering); a kept row keeps its children pruned to the matching subtree,
// except a directly matching row which keeps everything below it visible.
static bool filterEntry(const CommandEntry &e, const QString &needle, CommandEntry &out)
{
    if (entryMatches(e, needle)) {
        out = e; // direct hit: keep whole subtree, mirroring the proxy view
        return true;
    }
    CommandEntry pruned;
    pruned.command = e.command;
    pruned.option = e.option;
    pruned.description = e.description;
    for (const CommandEntry &child : e.children) {
        CommandEntry keptChild;
        if (filterEntry(child, needle, keptChild))
            pruned.children.append(keptChild);
    }
    if (!pruned.children.isEmpty()) {
        out = pruned;
        return true;
    }
    return false;
}

QList<CommandEntry> FrequentlyUsedCommands::filter(const QString &searchText) const
{
    if (searchText.isEmpty())
        return m_entries;
    QList<CommandEntry> result;
    for (const CommandEntry &e : m_entries) {
        CommandEntry kept;
        if (filterEntry(e, searchText, kept))
            result.append(kept);
    }
    return result;
}

static const CommandEntry *findEntry(const QList<CommandEntry> &entries, const QString &command)
{
    for (const CommandEntry &e : entries) {
        if (e.command == command)
            return &e;
        if (const CommandEntry *hit = findEntry(e.children, command))
            return hit;
    }
    return nullptr;
}

const CommandEntry *FrequentlyUsedCommands::find(const QString &command) const
{
    return findEntry(m_entries, command);
}

} // namespace cubeshell
