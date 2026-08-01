#pragma once

// CommandHistory.h — 终端命令提示的本地历史命令存储。
//
// 对应Python: cube-shell.py::_get_history_key/_load_history_data/
//             _save_history_data/_add_history_entry/_history_suggestions
//             (L7607-7699)
//
// 磁盘格式与 Python 侧完全一致（可共用同一文件）：
//     { "global": [...], "by_profile": { key: [...] } }
// global 保留最近 500 条，单 profile 保留最近 200 条，均去重头插。
//
// 并发语义（对齐 Python 进程级共享 _history_data）：
// 1. 同进程内所有指向同一文件路径的实例共享同一份底层数据
//    （按路径索引的进程级注册表），多 Tab 写入互不覆盖；
// 2. addEntry() 落盘前重新读取磁盘并以磁盘为基底合并本次新条目，
//    与 Python 版进程并用时后写不会整段覆盖前写。

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <memory>

namespace cubeshell {

class CommandHistory {
public:
    // filePath 为空时默认 configDir()/command_history.json；
    // 单测可传入自定义路径。同路径实例共享同一份内存数据（按路径隔离）。
    explicit CommandHistory(const QString &filePath = QString());

    // 读取历史文件：不存在/损坏时回退为空结构，不抛错。
    // 注意会刷新同路径所有实例共享的内存数据。
    // 对应Python: cube-shell.py::_load_history_data (L7614-7629)
    void load();

    // 持久化写回（QSaveFile 原子写、indent 格式）。
    // 对应Python: cube-shell.py::_save_history_data (L7631-7638)
    bool save() const;

    // 新增一条历史命令：trim 后为空则忽略；global 与 profile 双写
    // （去重、头插、global 截 500 / profile 截 200），随后立即 save()。
    // 写回前先重读磁盘文件并以磁盘内容为基底合并本次新条目，
    // 避免覆盖其他写入方（其他实例/进程）已落盘的历史。
    // profileKey 为空时按 "global" 分组。
    // 对应Python: cube-shell.py::_add_history_entry (L7640-7671)
    //           + _get_history_key (L7607-7612)
    void addEntry(const QString &cmd, const QString &profileKey);

    // 按前缀匹配历史候选：profile 优先、其次 global；
    // startsWith(prefix) 且 != prefix、去重、≤20 条。
    // 对应Python: cube-shell.py::_history_suggestions (L7673-7699)
    QStringList suggestions(const QString &prefix, const QString &profileKey) const;

    const QString &filePath() const { return m_filePath; }
    const QStringList &globalHistory() const;
    QStringList profileHistory(const QString &profileKey) const;

private:
    // 同进程共享的底层数据：同一文件路径只有一份。
    struct Data {
        QStringList global;                     // "global" 列表
        QHash<QString, QStringList> byProfile;  // "by_profile" 映射
        mutable QMutex mutex;                   // 防御性保护（当前均主线程调用）
    };

    // 进程级注册表：按规范化文件路径取共享数据（不存在则创建）。
    static std::shared_ptr<Data> sharedDataFor(const QString &filePath);

    QString m_filePath;
    std::shared_ptr<Data> m_data;  // 实例仅持共享指针
};

} // namespace cubeshell
