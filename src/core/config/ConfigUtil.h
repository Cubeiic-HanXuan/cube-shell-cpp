#pragma once

// ConfigUtil.h — JSON/TOML configuration loading helpers.
//
// 对应Python: function/util.py 配置读写部分 (read_json_file / read_json /
//             write_json) + cube-shell.py::nat_lod 的 toml.load 用法。
//
// TOML note: the only TOML file the app ships is conf/frpc.toml (flat scalars,
// dotted keys, [table] and [[array-of-tables]]), so instead of pulling in
// toml++ this module implements a deliberately minimal TOML subset reader
// (comments, bare/basic strings, integers, floats, booleans, dotted keys,
// [table], [[array]] headers). Values land in a QVariantMap mirroring what
// Python's toml.load() would return for the same file.

#include <QJsonDocument>
#include <QJsonValue>
#include <QString>
#include <QVariantMap>

namespace cubeshell {

namespace ConfigUtil {

// 对应Python: function/util.py::read_json / read_json_file
// Parse a JSON file (object or array). Returns Undefined value on failure
// (read_json_file returns None and logs).
QJsonValue readJson(const QString &filePath, QString *errorOut = nullptr);

// 对应Python: function/util.py::write_json (indent=4)
bool writeJson(const QString &filePath, const QJsonValue &value,
               QString *errorOut = nullptr);

// 对应Python: cube-shell.py::nat_lod 中的 toml.load(file)
// Minimal TOML subset reader (see header comment). Dotted keys and [table]
// headers become nested QVariantMap; [[name]] appends to a QVariantList.
QVariantMap readToml(const QString &filePath, QString *errorOut = nullptr);
// Same parser over an in-memory document (used by tests).
QVariantMap parseToml(const QString &tomlText, QString *errorOut = nullptr);

// Extension-based loader routing: ".json" -> readJson (as QVariant),
// ".toml" -> readToml. Anything else fails. Keeps callers agnostic of the
// underlying format, matching how the Python side picks json/toml per file.
QVariant loadConfig(const QString &filePath, QString *errorOut = nullptr);

} // namespace ConfigUtil

} // namespace cubeshell
