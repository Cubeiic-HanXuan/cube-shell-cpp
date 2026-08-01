// FrequentlyUsedCommands.cpp — command reference data. See FrequentlyUsedCommands.h.

#include "FrequentlyUsedCommands.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cubeshell {

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
