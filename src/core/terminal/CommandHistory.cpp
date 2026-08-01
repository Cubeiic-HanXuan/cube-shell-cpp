// CommandHistory.cpp — 历史命令存储。See CommandHistory.h.
// 对应Python: cube-shell.py L7607-7699

#include "CommandHistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>

#include "config/GlobalState.h"

namespace cubeshell {

// 历史分组键：profileKey 为空时用 "global"。
// 对应Python: cube-shell.py::_get_history_key (L7607-7612)
static QString historyKey(const QString &profileKey)
{
    return profileKey.isEmpty() ? QStringLiteral("global") : profileKey;
}

// JSON 数组 → 仅保留字符串项的列表（Python 侧字段类型异常时回退默认）。
static QStringList toStringList(const QJsonValue &value)
{
    QStringList out;
    if (!value.isArray())
        return out;
    const QJsonArray arr = value.toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (v.isString())
            out.append(v.toString());
    }
    return out;
}

// 读取磁盘历史文件。成功解析返回 true；不存在/损坏返回 false，输出参数置空。
// 对应Python: cube-shell.py::_load_history_data (L7614-7629)
static bool readHistoryFile(const QString &filePath,
                            QStringList *global,
                            QHash<QString, QStringList> *byProfile)
{
    global->clear();
    byProfile->clear();

    QFile f(filePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return false; // 损坏 → 空结构，不抛错

    const QJsonObject root = doc.object();
    *global = toStringList(root.value(QStringLiteral("global")));
    const QJsonValue bp = root.value(QStringLiteral("by_profile"));
    if (bp.isObject()) {
        const QJsonObject obj = bp.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
            byProfile->insert(it.key(), toStringList(it.value()));
    }
    return true;
}

// QSaveFile 原子写回（indent 格式）。
// 对应Python: cube-shell.py::_save_history_data (L7631-7638)
static bool writeHistoryFile(const QString &filePath,
                             const QStringList &global,
                             const QHash<QString, QStringList> &byProfile)
{
    // Python 侧 os.makedirs(dirname, exist_ok=True)
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    QJsonObject root;
    root.insert(QStringLiteral("global"), QJsonArray::fromStringList(global));
    QJsonObject bp;
    for (auto it = byProfile.constBegin(); it != byProfile.constEnd(); ++it)
        bp.insert(it.key(), QJsonArray::fromStringList(it.value()));
    root.insert(QStringLiteral("by_profile"), bp);

    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

// 进程级注册表：路径 → 共享数据。对齐 Python 进程级共享 _history_data。
std::shared_ptr<CommandHistory::Data> CommandHistory::sharedDataFor(const QString &filePath)
{
    static QMutex registryMutex;
    static QHash<QString, std::shared_ptr<Data>> registry;

    // 规范化路径作键，同一文件的相对/绝对写法归并；不同路径彼此隔离。
    const QString key = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());

    QMutexLocker locker(&registryMutex);
    std::shared_ptr<Data> &slot = registry[key];
    if (!slot)
        slot = std::make_shared<Data>();
    return slot;
}

CommandHistory::CommandHistory(const QString &filePath)
    : m_filePath(filePath.isEmpty()
                     ? GlobalState::configFilePath(QStringLiteral("command_history.json"))
                     : filePath)
    , m_data(sharedDataFor(m_filePath))
{
}

// 对应Python: cube-shell.py::_load_history_data (L7614-7629)
void CommandHistory::load()
{
    QMutexLocker locker(&m_data->mutex);
    readHistoryFile(m_filePath, &m_data->global, &m_data->byProfile);
}

// 对应Python: cube-shell.py::_save_history_data (L7631-7638)
bool CommandHistory::save() const
{
    QMutexLocker locker(&m_data->mutex);
    return writeHistoryFile(m_filePath, m_data->global, m_data->byProfile);
}

// 对应Python: cube-shell.py::_add_history_entry (L7640-7671)
void CommandHistory::addEntry(const QString &cmd, const QString &profileKey)
{
    const QString entry = cmd.trimmed();
    if (entry.isEmpty())
        return;

    QMutexLocker locker(&m_data->mutex);

    // 落盘前磁盘合并：重读磁盘作为基底，保留其他实例/进程已落盘的条目；
    // 磁盘不存在或损坏时退回当前内存数据，不丢本进程历史。
    QStringList diskGlobal;
    QHash<QString, QStringList> diskByProfile;
    if (readHistoryFile(m_filePath, &diskGlobal, &diskByProfile)) {
        m_data->global = diskGlobal;
        m_data->byProfile = diskByProfile;
    }

    // global：去重、头插、截 500
    m_data->global.removeAll(entry);
    m_data->global.prepend(entry);
    while (m_data->global.size() > 500)
        m_data->global.removeLast();

    // profile：去重、头插、截 200
    QStringList &lst = m_data->byProfile[historyKey(profileKey)];
    lst.removeAll(entry);
    lst.prepend(entry);
    while (lst.size() > 200)
        lst.removeLast();

    writeHistoryFile(m_filePath, m_data->global, m_data->byProfile);
}

// 对应Python: cube-shell.py::_history_suggestions (L7673-7699)
QStringList CommandHistory::suggestions(const QString &prefix, const QString &profileKey) const
{
    const QString p = prefix.trimmed();
    QStringList out;
    if (p.isEmpty())
        return out;

    QMutexLocker locker(&m_data->mutex);
    QSet<QString> seen;
    const QStringList profile = m_data->byProfile.value(historyKey(profileKey));
    const QStringList &global = m_data->global;
    for (const QStringList *src : {&profile, &global}) {
        for (const QString &s : *src) {
            if (!s.startsWith(p))
                continue;
            if (s == p)
                continue;
            if (seen.contains(s))
                continue;
            seen.insert(s);
            out.append(s);
            if (out.size() >= 20)
                return out;
        }
    }
    return out;
}

const QStringList &CommandHistory::globalHistory() const
{
    // 返回共享数据的引用；当前均主线程调用，引用生命周期由共享指针保证。
    return m_data->global;
}

QStringList CommandHistory::profileHistory(const QString &profileKey) const
{
    QMutexLocker locker(&m_data->mutex);
    return m_data->byProfile.value(historyKey(profileKey));
}

} // namespace cubeshell
