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

// 原子写 + 把最终文件权限收敛到 0600（仅属主可读写）。
//
// 配置目录里的文件默认按 umask 建出来是 0644——同一台机器上的其他用户可以
// 直接 cat。devices.json / tunnel.json 这类含连接凭据与内网拓扑的文件不该如此。
//
// 返回 false 只表示**数据没写成**。权限收敛失败会 qWarning 但仍返回 true：
// 字节已经落盘了，此时报「保存失败」会误导用户去重试一次同样写得成的操作。
// 需要确认权限是否真的收紧的调用方，自己再调一次 restrictPermissions()。
bool writeSecure(const QString &filePath, const QByteArray &data,
                 QString *errorOut = nullptr);

// 把已存在文件的权限收敛到 0600。幂等；文件不存在时视为成功（无可收敛之物）。
// 用于给历史遗留的 0644 文件补权限——只写新文件是不够的，用户磁盘上现存的
// 那一份才是正在泄露的那一份。
bool restrictPermissions(const QString &filePath, QString *errorOut = nullptr);

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
