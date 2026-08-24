#pragma once

// CommandIndex.h — 终端命令提示的静态命令/选项索引。
//
// 对应Python: function/ssh_prompt_client.py::load_linux_commands
//           + cube-shell.py::_compute_suggestions
//
// 数据来源两部分（与 Python 侧一致）：
//   1. conf/linux_commands.json 树（复用 FrequentlyUsedCommands 加载），
//      叶子节点的 command 作为命令名，option 文本用正则提取候选选项；
//   2. Python 侧内置命令表/内置选项表（builtin_commands / builtin_options，
//      逐项照抄），JSON 缺失时内置表仍然可用。
// 命令列表与每个命令的选项列表均保持有序，供 computeSuggestions 二分前缀匹配。

#include <QHash>
#include <QString>
#include <QStringList>

namespace cubeshell {

class CommandIndex {
public:
    CommandIndex() = default;

    // 加载 conf/linux_commands.json 并合并内置表。jsonPath 为空时走
    // FrequentlyUsedCommands::defaultPath() 探测（磁盘 conf/ 优先，
    // 打包产物里落到编进二进制的 :/conf/linux_commands.json）。
    // 返回值仅表示 JSON 是否加载成功；内置表始终会被合并。
    // 对应Python: function/ssh_prompt_client.py::load_linux_commands
    bool load(const QString &jsonPath = QString());

    // 已排序的全部命令名。
    const QStringList &commands() const { return m_commands; }

    // 指定命令的已排序选项列表（无则为空）。
    QStringList optionsFor(const QString &cmd) const { return m_options.value(cmd); }

    // 基于前缀的候选计算（返回 ≤80 条）：
    //   lstrip 后为空        → 全部命令；
    //   单 token            → 命令表上二分求 [prefix, prefix+"\uffff") 区间；
    //   多 token 且末 token 以 '-' 开头 → 该命令的选项表上同样二分；
    //   其他                → 空。
    // 对应Python: cube-shell.py::_compute_suggestions (L7867-7895)
    QStringList computeSuggestions(const QString &text) const;

private:
    QStringList m_commands;                  // 排序后的命令名
    QHash<QString, QStringList> m_options;   // 命令 -> 排序后的选项
};

} // namespace cubeshell
