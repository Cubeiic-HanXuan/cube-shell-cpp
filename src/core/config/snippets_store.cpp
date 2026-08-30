#include "snippets_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include "ConfigUtil.h"
#include "GlobalState.h"

namespace cubeshell {

SnippetsStore::SnippetsStore(const QString &filePath)
    : m_filePath(filePath.isEmpty() ? GlobalState::configFilePath(QStringLiteral("snippets.json"))
                                    : filePath)
{
}

QList<Snippet> SnippetsStore::load() const
{
    QList<Snippet> out;
    const QJsonValue root = ConfigUtil::readJson(m_filePath);
    if (!root.isObject())
        return out;   // 缺失/坏文件 = 空表

    const QJsonArray arr = root.toObject().value(QStringLiteral("snippets")).toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Snippet s;
        s.id = o.value(QStringLiteral("id")).toString();
        if (s.id.isEmpty())   // 旧数据没 id：补一个，保住后续编辑的定位不变量
            s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        s.group = o.value(QStringLiteral("group")).toString();
        s.name = o.value(QStringLiteral("name")).toString();
        s.body = o.value(QStringLiteral("body")).toString();
        s.shortcut = o.value(QStringLiteral("shortcut")).toString();
        s.appendNewline = o.value(QStringLiteral("appendNewline")).toBool(true);
        if (s.name.isEmpty() && s.body.isEmpty())
            continue;   // 空条目没意义
        out.append(s);
    }
    return out;
}

bool SnippetsStore::save(const QList<Snippet> &snippets, QString *errorOut) const
{
    QJsonArray arr;
    for (const Snippet &s : snippets) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("group"), s.group);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("body"), s.body);
        o.insert(QStringLiteral("shortcut"), s.shortcut);
        o.insert(QStringLiteral("appendNewline"), s.appendNewline);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("snippets"), arr);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return ConfigUtil::writeSecure(m_filePath, data, errorOut);
}

bool SnippetsStore::upsert(const Snippet &snippet, QString *errorOut)
{
    QList<Snippet> all = load();
    Snippet s = snippet;
    if (s.id.isEmpty())
        s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool found = false;
    for (Snippet &cur : all) {
        if (cur.id == s.id) {
            cur = s;
            found = true;
            break;
        }
    }
    if (!found)
        all.append(s);
    return save(all, errorOut);
}

bool SnippetsStore::remove(const QString &id, QString *errorOut)
{
    QList<Snippet> all = load();
    for (auto it = all.begin(); it != all.end(); ++it) {
        if (it->id == id) {
            all.erase(it);
            return save(all, errorOut);
        }
    }
    return true;   // 不存在视为已删除（幂等）
}

QStringList SnippetsStore::groups() const
{
    QStringList out;
    for (const Snippet &s : load()) {
        if (!s.group.isEmpty() && !out.contains(s.group))
            out.append(s.group);
    }
    return out;
}

QStringList SnippetsStore::extractParams(const QString &body)
{
    static const QRegularExpression re(QStringLiteral("\\{(\\w+)\\}"));
    QStringList out;
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!out.contains(name))
            out.append(name);
    }
    return out;
}

QString SnippetsStore::expand(const QString &body, const QHash<QString, QString> &values)
{
    static const QRegularExpression re(QStringLiteral("\\{(\\w+)\\}"));
    QString out;
    out.reserve(body.size());
    qsizetype pos = 0;
    auto it = re.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.append(body.mid(pos, m.capturedStart() - pos));
        const QString name = m.captured(1);
        // 缺值的占位符原样保留，让用户看到还有参数没填，而不是悄悄下发半截命令。
        out.append(values.contains(name) ? values.value(name) : m.captured(0));
        pos = m.capturedEnd();
    }
    out.append(body.mid(pos));
    return out;
}

} // namespace cubeshell
