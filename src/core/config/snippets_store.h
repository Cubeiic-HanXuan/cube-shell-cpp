#pragma once

// snippets_store.h — 用户自定义参数化片段（Snippets）存储。
//
// FrequentlyUsedCommands 是只读的静态命令库（conf/linux_commands.json，随包分发）；
// 本类是用户可写的那一层：自建、分组、带 {占位参数}、可绑快捷键、一键下发到当前会话。
// 持久化到用户配置目录 snippets.json，原子写 + 0600（ConfigUtil::writeSecure），
// 与 devices.json/groups.json 同一套落盘约定。
//
// 文件格式：
//   { "snippets": [ { "id", "group", "name", "body",
//                     "shortcut", "appendNewline" }, ... ] }

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {

struct Snippet {
    QString id;                 // 稳定 id（QUuid），重命名/改组不变
    QString group;              // 分组（作用域），空 = 未分组
    QString name;               // 显示名 / 按钮文字
    QString body;               // 内容，可含 {param} 占位
    QString shortcut;           // QKeySequence 序列化串，空 = 无快捷键
    bool    appendNewline = true; // 下发后是否补回车
};

class SnippetsStore {
public:
    // filePath 为空 -> GlobalState::configFilePath("snippets.json")。
    explicit SnippetsStore(const QString &filePath = QString());

    QString filePath() const { return m_filePath; }

    // 读全部片段。文件缺失/坏 JSON 返回空表（不报错），与 GroupManager 同款容错。
    QList<Snippet> load() const;
    // 整表原子写回。
    bool save(const QList<Snippet> &snippets, QString *errorOut = nullptr) const;

    // 便捷 CRUD（读→改→写回）。
    bool upsert(const Snippet &snippet, QString *errorOut = nullptr);   // 有 id 则改，否则新增
    bool remove(const QString &id, QString *errorOut = nullptr);
    // 全部已用分组名（去重、按出现顺序）。
    QStringList groups() const;

    // --- 占位参数（纯逻辑，便于单测）---
    // 提取 body 里的 {name} 占位符（去重、按出现顺序）。{{ 转义暂不支持。
    static QStringList extractParams(const QString &body);
    // 用 values 替换 {name}；缺值的占位符原样保留（不下发半成品替换）。
    static QString expand(const QString &body, const QHash<QString, QString> &values);

private:
    QString m_filePath;
};

} // namespace cubeshell
