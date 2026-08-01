// ConfigUtil.cpp — JSON/TOML config loading. See ConfigUtil.h.

#include "ConfigUtil.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QVariantList>

namespace cubeshell {

namespace ConfigUtil {

// 对应Python: function/util.py::read_json / read_json_file
QJsonValue readJson(const QString &filePath, QString *errorOut)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot open %1").arg(filePath);
        return QJsonValue(QJsonValue::Undefined);
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (errorOut) *errorOut = QStringLiteral("invalid JSON in %1: %2")
                                      .arg(filePath, perr.errorString());
        return QJsonValue(QJsonValue::Undefined);
    }
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    if (errorOut) *errorOut = QStringLiteral("unsupported JSON root in %1").arg(filePath);
    return QJsonValue(QJsonValue::Undefined);
}

// 对应Python: function/util.py::write_json (indent=4 -> Indented)
bool writeJson(const QString &filePath, const QJsonValue &value, QString *errorOut)
{
    QJsonDocument doc;
    if (value.isObject())
        doc = QJsonDocument(value.toObject());
    else if (value.isArray())
        doc = QJsonDocument(value.toArray());
    else {
        if (errorOut) *errorOut = QStringLiteral("JSON root must be object or array");
        return false;
    }

    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(filePath);
        return false;
    }
    f.write(doc.toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(filePath);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal TOML subset parser (enough for conf/frpc.toml, see ConfigUtil.h).
// ---------------------------------------------------------------------------

// Cut an unquoted '#' comment off the line (respects "..." and '...').
static QString stripTomlComment(const QString &line)
{
    bool inBasic = false;   // "..."
    bool inLiteral = false; // '...'
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inBasic) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('"')) inBasic = false;
        } else if (inLiteral) {
            if (c == QLatin1Char('\'')) inLiteral = false;
        } else {
            if (c == QLatin1Char('"')) inBasic = true;
            else if (c == QLatin1Char('\'')) inLiteral = true;
            else if (c == QLatin1Char('#')) return line.left(i);
        }
    }
    return line;
}

// Unescape a TOML basic string body (subset: \" \\ \n \t \r \uXXXX).
static QString unescapeBasic(const QString &body)
{
    QString out;
    out.reserve(body.size());
    for (int i = 0; i < body.size(); ++i) {
        const QChar c = body.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= body.size()) {
            out.append(c);
            continue;
        }
        const QChar esc = body.at(++i);
        switch (esc.unicode()) {
        case 'n': out.append(QLatin1Char('\n')); break;
        case 't': out.append(QLatin1Char('\t')); break;
        case 'r': out.append(QLatin1Char('\r')); break;
        case '"': out.append(QLatin1Char('"')); break;
        case '\\': out.append(QLatin1Char('\\')); break;
        case 'u':
            if (i + 4 < body.size()) {
                bool ok = false;
                const ushort code = body.mid(i + 1, 4).toUShort(&ok, 16);
                if (ok) { out.append(QChar(code)); i += 4; break; }
            }
            out.append(esc);
            break;
        default:
            out.append(esc);
            break;
        }
    }
    return out;
}

static QVariant parseTomlScalar(const QString &raw);

// Split a top-level inline array "[a, b, c]" body on commas outside quotes.
static QVariantList parseTomlArray(const QString &body)
{
    QVariantList list;
    QString current;
    bool inBasic = false, inLiteral = false;
    int depth = 0;
    for (int i = 0; i < body.size(); ++i) {
        const QChar c = body.at(i);
        if (inBasic) {
            if (c == QLatin1Char('\\')) {
                current.append(c);
                if (++i < body.size())
                    current.append(body.at(i));
                continue;
            }
            if (c == QLatin1Char('"')) inBasic = false;
            current.append(c);
        } else if (inLiteral) {
            if (c == QLatin1Char('\'')) inLiteral = false;
            current.append(c);
        } else if (c == QLatin1Char('"')) {
            inBasic = true; current.append(c);
        } else if (c == QLatin1Char('\'')) {
            inLiteral = true; current.append(c);
        } else if (c == QLatin1Char('[')) {
            ++depth; current.append(c);
        } else if (c == QLatin1Char(']')) {
            --depth; current.append(c);
        } else if (c == QLatin1Char(',') && depth == 0) {
            if (!current.trimmed().isEmpty())
                list.append(parseTomlScalar(current.trimmed()));
            current.clear();
        } else {
            current.append(c);
        }
    }
    if (!current.trimmed().isEmpty())
        list.append(parseTomlScalar(current.trimmed()));
    return list;
}

// Scalar value: string / bool / int / float / inline array; fallback = text.
static QVariant parseTomlScalar(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return QString();
    if (s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')) && s.size() >= 2)
        return unescapeBasic(s.mid(1, s.size() - 2));
    if (s.startsWith(QLatin1Char('\'')) && s.endsWith(QLatin1Char('\'')) && s.size() >= 2)
        return s.mid(1, s.size() - 2); // literal string, no escapes
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']')))
        return parseTomlArray(s.mid(1, s.size() - 2));
    if (s == QLatin1String("true"))
        return true;
    if (s == QLatin1String("false"))
        return false;

    QString num = s;
    num.remove(QLatin1Char('_')); // TOML digit separators
    bool ok = false;
    const qlonglong i = num.toLongLong(&ok);
    if (ok)
        return i;
    const double d = num.toDouble(&ok);
    if (ok)
        return d;
    return s; // unknown token: surface as raw text rather than failing
}

// Split "a.b.c" (with optional quoted parts) into path components.
static QStringList splitTomlKey(const QString &key)
{
    QStringList parts;
    QString current;
    bool inBasic = false, inLiteral = false;
    for (const QChar c : key) {
        if (inBasic) {
            if (c == QLatin1Char('"')) { inBasic = false; continue; }
            current.append(c);
        } else if (inLiteral) {
            if (c == QLatin1Char('\'')) { inLiteral = false; continue; }
            current.append(c);
        } else if (c == QLatin1Char('"')) {
            inBasic = true;
        } else if (c == QLatin1Char('\'')) {
            inLiteral = true;
        } else if (c == QLatin1Char('.')) {
            parts.append(current.trimmed());
            current.clear();
        } else {
            current.append(c);
        }
    }
    parts.append(current.trimmed());
    return parts;
}

// Insert value at keyPath inside a plain table (creating nested maps).
static void setInTable(QVariantMap &table, const QStringList &keyPath, const QVariant &value)
{
    if (keyPath.size() == 1) {
        table.insert(keyPath.first(), value);
        return;
    }
    QVariantMap child = table.value(keyPath.first()).toMap();
    setInTable(child, keyPath.mid(1), value);
    table.insert(keyPath.first(), child);
}

// Walk tablePath from `node`, descending into the *last* element of any
// array-of-tables on the way (TOML semantics for [[x]] sections), then set
// keyPath = value there.
static void navigateAndSet(QVariantMap &node, const QStringList &tablePath,
                           const QStringList &keyPath, const QVariant &value)
{
    if (tablePath.isEmpty()) {
        setInTable(node, keyPath, value);
        return;
    }
    const QString head = tablePath.first();
    QVariant childVar = node.value(head);
    if (childVar.typeId() == QMetaType::QVariantList) {
        QVariantList list = childVar.toList();
        QVariantMap last = list.isEmpty() ? QVariantMap() : list.takeLast().toMap();
        navigateAndSet(last, tablePath.mid(1), keyPath, value);
        list.append(last);
        node.insert(head, list);
    } else {
        QVariantMap child = childVar.toMap();
        navigateAndSet(child, tablePath.mid(1), keyPath, value);
        node.insert(head, child);
    }
}

// [name]: make sure an (possibly empty) table exists at the path without
// disturbing anything already stored beneath it.
static void ensureTable(QVariantMap &node, const QStringList &tablePath)
{
    if (tablePath.isEmpty())
        return;
    const QString head = tablePath.first();
    QVariant childVar = node.value(head);
    if (childVar.typeId() == QMetaType::QVariantList) {
        QVariantList list = childVar.toList();
        QVariantMap last = list.isEmpty() ? QVariantMap() : list.takeLast().toMap();
        ensureTable(last, tablePath.mid(1));
        list.append(last);
        node.insert(head, list);
    } else {
        QVariantMap child = childVar.toMap();
        ensureTable(child, tablePath.mid(1));
        node.insert(head, child);
    }
}

// [[name]]: append a fresh table to the list at tablePath.
static void appendArrayTable(QVariantMap &node, const QStringList &tablePath)
{
    const QString head = tablePath.first();
    if (tablePath.size() == 1) {
        QVariantList list = node.value(head).toList();
        list.append(QVariantMap());
        node.insert(head, list);
        return;
    }
    QVariant childVar = node.value(head);
    if (childVar.typeId() == QMetaType::QVariantList) {
        QVariantList list = childVar.toList();
        QVariantMap last = list.isEmpty() ? QVariantMap() : list.takeLast().toMap();
        appendArrayTable(last, tablePath.mid(1));
        list.append(last);
        node.insert(head, list);
    } else {
        QVariantMap child = childVar.toMap();
        appendArrayTable(child, tablePath.mid(1));
        node.insert(head, child);
    }
}

// 对应Python: toml.loads 的等价子集实现
QVariantMap parseToml(const QString &tomlText, QString *errorOut)
{
    QVariantMap root;
    QStringList currentTable; // path of the active [table] / [[array]] section

    const QStringList lines = tomlText.split(QLatin1Char('\n'));
    for (int lineNo = 0; lineNo < lines.size(); ++lineNo) {
        const QString line = stripTomlComment(lines.at(lineNo)).trimmed();
        if (line.isEmpty())
            continue;

        // [[array-of-tables]] header
        if (line.startsWith(QLatin1String("[[")) && line.endsWith(QLatin1String("]]"))) {
            const QStringList path = splitTomlKey(line.mid(2, line.size() - 4).trimmed());
            appendArrayTable(root, path);
            currentTable = path;
            continue;
        }
        // [table] header
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            currentTable = splitTomlKey(line.mid(1, line.size() - 2).trimmed());
            ensureTable(root, currentTable);
            continue;
        }

        // key = value
        // Find the first '=' outside quotes (keys may be quoted).
        int eq = -1;
        bool inBasic = false, inLiteral = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line.at(i);
            if (inBasic) { if (c == QLatin1Char('"')) inBasic = false; }
            else if (inLiteral) { if (c == QLatin1Char('\'')) inLiteral = false; }
            else if (c == QLatin1Char('"')) inBasic = true;
            else if (c == QLatin1Char('\'')) inLiteral = true;
            else if (c == QLatin1Char('=')) { eq = i; break; }
        }
        if (eq < 0) {
            if (errorOut) *errorOut = QStringLiteral("TOML parse error at line %1: missing '='")
                                          .arg(lineNo + 1);
            return QVariantMap();
        }
        const QStringList keyPath = splitTomlKey(line.left(eq).trimmed());
        const QVariant value = parseTomlScalar(line.mid(eq + 1).trimmed());
        navigateAndSet(root, currentTable, keyPath, value);
    }
    return root;
}

// 对应Python: cube-shell.py::nat_lod 中的 toml.load(file)
QVariantMap readToml(const QString &filePath, QString *errorOut)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot open %1").arg(filePath);
        return QVariantMap();
    }
    return parseToml(QString::fromUtf8(f.readAll()), errorOut);
}

// Route by extension: .json -> readJson, .toml -> readToml.
QVariant loadConfig(const QString &filePath, QString *errorOut)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix == QLatin1String("json")) {
        const QJsonValue v = readJson(filePath, errorOut);
        return v.isUndefined() ? QVariant() : v.toVariant();
    }
    if (suffix == QLatin1String("toml")) {
        QString err;
        const QVariantMap m = readToml(filePath, &err);
        if (!err.isEmpty()) {
            if (errorOut) *errorOut = err;
            return QVariant();
        }
        return m;
    }
    if (errorOut) *errorOut = QStringLiteral("unsupported config format: %1").arg(filePath);
    return QVariant();
}

} // namespace ConfigUtil

} // namespace cubeshell
