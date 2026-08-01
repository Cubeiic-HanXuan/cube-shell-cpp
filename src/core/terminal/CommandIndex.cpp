// CommandIndex.cpp — 终端命令提示索引。See CommandIndex.h.
// 对应Python: function/ssh_prompt_client.py::load_linux_commands
//           + cube-shell.py::_compute_suggestions

#include "CommandIndex.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QSet>

#include <algorithm>

#include "config/FrequentlyUsedCommands.h"

namespace cubeshell {

// conf/linux_commands.json 探测顺序与 LinuxCommandsDialog::resolveCommandsFile
// 一致：应用目录 → macOS bundle Resources → cwd → 源码树（CMake 注入）。
// core 不依赖 ui，故在此重复实现同一逻辑。
// 对应Python: function/ssh_prompt_client.py::_default_linux_commands_path
static QString resolveCommandsFile()
{
    const QString fileName = QStringLiteral("linux_commands.json");
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/conf/") + fileName
               << appDir + QStringLiteral("/../Resources/conf/") + fileName
               << QDir::currentPath() + QStringLiteral("/conf/") + fileName;
#ifdef CUBESHELL_SOURCE_CONF_DIR
    // Dev tree: cpp/build/... -> repo root conf/ (source layout, CMake 注入).
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/") + fileName;
#endif
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    return QString();
}

// 从 option 文本中提取形如 -x / --xxx 的选项集合。
// 对应Python: function/ssh_prompt_client.py::_extract_options (L15-23)
static QSet<QString> extractOptions(const QString &optionText)
{
    QSet<QString> opts;
    if (optionText.isEmpty())
        return opts;
    static const QRegularExpression reLineStart(
        QStringLiteral("^\\s*(--?[A-Za-z0-9][\\w\\-]*)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression reWord(
        QStringLiteral("\\b(--?[A-Za-z0-9][\\w\\-]*)\\b"));
    for (const QRegularExpression *re : {&reLineStart, &reWord}) {
        QRegularExpressionMatchIterator it = re->globalMatch(optionText);
        while (it.hasNext())
            opts.insert(it.next().captured(1));
    }
    return opts;
}

// 内置命令表（JSON 缺失时的兜底，加载后始终合并）。
// 对应Python: function/ssh_prompt_client.py L35-57 builtin_commands（逐项照抄）
static const QStringList &builtinCommands()
{
    static const QStringList commands = {
        // 基础文件/目录操作
        QStringLiteral("ls"), QStringLiteral("cd"), QStringLiteral("pwd"),
        QStringLiteral("cat"), QStringLiteral("echo"), QStringLiteral("touch"),
        QStringLiteral("mkdir"), QStringLiteral("rm"), QStringLiteral("rmdir"),
        QStringLiteral("cp"), QStringLiteral("mv"), QStringLiteral("ln"),
        QStringLiteral("find"), QStringLiteral("tail"), QStringLiteral("head"),
        QStringLiteral("less"), QStringLiteral("more"), QStringLiteral("wc"),
        QStringLiteral("du"), QStringLiteral("df"), QStringLiteral("file"),
        QStringLiteral("diff"), QStringLiteral("sed"), QStringLiteral("awk"),
        QStringLiteral("sort"), QStringLiteral("uniq"),
        // 压缩/解压缩
        QStringLiteral("tar"), QStringLiteral("zip"), QStringLiteral("unzip"),
        QStringLiteral("gzip"), QStringLiteral("gunzip"), QStringLiteral("bzip2"),
        QStringLiteral("bunzip2"), QStringLiteral("xz"), QStringLiteral("unxz"),
        // 网络操作
        QStringLiteral("ssh"), QStringLiteral("scp"), QStringLiteral("rsync"),
        QStringLiteral("ping"), QStringLiteral("traceroute"), QStringLiteral("mtr"),
        QStringLiteral("curl"), QStringLiteral("wget"), QStringLiteral("nc"),
        QStringLiteral("telnet"), QStringLiteral("ss"), QStringLiteral("netstat"),
        QStringLiteral("ip"), QStringLiteral("ifconfig"),
        // 权限/用户管理
        QStringLiteral("chmod"), QStringLiteral("chown"), QStringLiteral("chgrp"),
        QStringLiteral("sudo"), QStringLiteral("su"), QStringLiteral("useradd"),
        QStringLiteral("userdel"), QStringLiteral("groupadd"), QStringLiteral("passwd"),
        // 进程/系统管理
        QStringLiteral("ps"), QStringLiteral("kill"), QStringLiteral("killall"),
        QStringLiteral("pkill"), QStringLiteral("systemctl"), QStringLiteral("journalctl"),
        QStringLiteral("top"), QStringLiteral("htop"), QStringLiteral("free"),
        QStringLiteral("vmstat"), QStringLiteral("iostat"), QStringLiteral("uptime"),
        QStringLiteral("who"), QStringLiteral("w"),
        // 容器/版本控制
        QStringLiteral("docker"), QStringLiteral("git"), QStringLiteral("podman"),
        // 包管理
        QStringLiteral("apt"), QStringLiteral("apt-get"), QStringLiteral("apt-cache"),
        QStringLiteral("dpkg"), QStringLiteral("yum"), QStringLiteral("dnf"),
        QStringLiteral("rpm"),
        // 编程语言/工具
        QStringLiteral("python3"), QStringLiteral("python"), QStringLiteral("pip"),
        QStringLiteral("pip3"), QStringLiteral("node"), QStringLiteral("npm"),
        QStringLiteral("java"), QStringLiteral("javac"), QStringLiteral("gcc"),
        QStringLiteral("make"),
        // 系统配置/日志
        QStringLiteral("hostname"), QStringLiteral("hostnamectl"),
        QStringLiteral("timedatectl"), QStringLiteral("logrotate"),
        QStringLiteral("dmesg"), QStringLiteral("lsblk"), QStringLiteral("mount"),
        QStringLiteral("umount"),
        // 文本处理
        QStringLiteral("cut"), QStringLiteral("paste"), QStringLiteral("tr"),
        QStringLiteral("grep"), QStringLiteral("egrep"), QStringLiteral("fgrep"),
    };
    return commands;
}

// 内置选项表。
// 对应Python: function/ssh_prompt_client.py L59-115 builtin_options（逐项照抄）
static const QHash<QString, QStringList> &builtinOptions()
{
    static const QHash<QString, QStringList> options = {
        // 基础命令补充
        {QStringLiteral("ls"), {QStringLiteral("-l"), QStringLiteral("-a"), QStringLiteral("-h"),
                                QStringLiteral("-t"), QStringLiteral("-r"), QStringLiteral("-S"),
                                QStringLiteral("-R"), QStringLiteral("-d"), QStringLiteral("-1"),
                                QStringLiteral("--color=auto")}},
        {QStringLiteral("rm"), {QStringLiteral("-f"), QStringLiteral("-r"), QStringLiteral("-i"),
                                QStringLiteral("-v")}},
        {QStringLiteral("cp"), {QStringLiteral("-r"), QStringLiteral("-f"), QStringLiteral("-i"),
                                QStringLiteral("-v"), QStringLiteral("-p")}},
        {QStringLiteral("mv"), {QStringLiteral("-f"), QStringLiteral("-i"), QStringLiteral("-v"),
                                QStringLiteral("-u")}},
        {QStringLiteral("mkdir"), {QStringLiteral("-p"), QStringLiteral("-v"), QStringLiteral("-m")}},
        {QStringLiteral("find"), {QStringLiteral("-name"), QStringLiteral("-type"),
                                  QStringLiteral("-size"), QStringLiteral("-mtime"),
                                  QStringLiteral("-exec"), QStringLiteral("-print"),
                                  QStringLiteral("-iname"), QStringLiteral("-maxdepth")}},
        {QStringLiteral("wc"), {QStringLiteral("-l"), QStringLiteral("-w"), QStringLiteral("-c"),
                                QStringLiteral("-m")}},
        {QStringLiteral("du"), {QStringLiteral("-h"), QStringLiteral("-s"), QStringLiteral("-c"),
                                QStringLiteral("-x")}},
        {QStringLiteral("df"), {QStringLiteral("-h"), QStringLiteral("-T"), QStringLiteral("-i")}},

        // 文本处理命令补充
        {QStringLiteral("grep"), {QStringLiteral("-n"), QStringLiteral("-i"), QStringLiteral("-r"),
                                  QStringLiteral("-E"), QStringLiteral("-F"), QStringLiteral("-C"),
                                  QStringLiteral("-v"), QStringLiteral("-l"), QStringLiteral("-c"),
                                  QStringLiteral("-w"), QStringLiteral("--color=auto")}},
        {QStringLiteral("sed"), {QStringLiteral("-i"), QStringLiteral("-e"), QStringLiteral("-f"),
                                 QStringLiteral("-n")}},
        {QStringLiteral("awk"), {QStringLiteral("-F"), QStringLiteral("-v"), QStringLiteral("-f")}},
        {QStringLiteral("sort"), {QStringLiteral("-n"), QStringLiteral("-r"), QStringLiteral("-k"),
                                  QStringLiteral("-u"), QStringLiteral("-t")}},
        {QStringLiteral("uniq"), {QStringLiteral("-c"), QStringLiteral("-d"), QStringLiteral("-u")}},

        // 压缩命令补充
        {QStringLiteral("tar"), {QStringLiteral("-x"), QStringLiteral("-c"), QStringLiteral("-v"),
                                 QStringLiteral("-f"), QStringLiteral("-z"), QStringLiteral("-j"),
                                 QStringLiteral("-J"), QStringLiteral("-C"),
                                 QStringLiteral("--exclude"), QStringLiteral("-p")}},
        {QStringLiteral("zip"), {QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("-u"),
                                 QStringLiteral("-d")}},
        {QStringLiteral("unzip"), {QStringLiteral("-d"), QStringLiteral("-l"), QStringLiteral("-q"),
                                   QStringLiteral("-o")}},

        // 网络命令补充
        {QStringLiteral("tail"), {QStringLiteral("-n"), QStringLiteral("-f"), QStringLiteral("-F"),
                                  QStringLiteral("-q"), QStringLiteral("-v")}},
        {QStringLiteral("head"), {QStringLiteral("-n"), QStringLiteral("-q"), QStringLiteral("-v")}},
        {QStringLiteral("ssh"), {QStringLiteral("-p"), QStringLiteral("-i"), QStringLiteral("-o"),
                                 QStringLiteral("-t"), QStringLiteral("-v"), QStringLiteral("-X"),
                                 QStringLiteral("-Y"), QStringLiteral("-N"), QStringLiteral("-f")}},
        {QStringLiteral("scp"), {QStringLiteral("-P"), QStringLiteral("-i"), QStringLiteral("-r"),
                                 QStringLiteral("-v"), QStringLiteral("-p"), QStringLiteral("-C")}},
        {QStringLiteral("curl"), {QStringLiteral("-L"), QStringLiteral("-I"), QStringLiteral("-s"),
                                  QStringLiteral("-S"), QStringLiteral("-o"), QStringLiteral("-O"),
                                  QStringLiteral("-X"), QStringLiteral("-H"), QStringLiteral("-d"),
                                  QStringLiteral("-u"), QStringLiteral("-k"), QStringLiteral("-v")}},
        {QStringLiteral("wget"), {QStringLiteral("-O"), QStringLiteral("-q"),
                                  QStringLiteral("--no-check-certificate"), QStringLiteral("-c"),
                                  QStringLiteral("-r"), QStringLiteral("-np"), QStringLiteral("-P"),
                                  QStringLiteral("-b")}},
        {QStringLiteral("ping"), {QStringLiteral("-c"), QStringLiteral("-i"), QStringLiteral("-s"),
                                  QStringLiteral("-W")}},
        {QStringLiteral("ip"), {QStringLiteral("addr"), QStringLiteral("link"),
                                QStringLiteral("route"), QStringLiteral("neigh"),
                                QStringLiteral("s"), QStringLiteral("a")}},
        {QStringLiteral("netstat"), {QStringLiteral("-t"), QStringLiteral("-u"), QStringLiteral("-l"),
                                     QStringLiteral("-n"), QStringLiteral("-p"), QStringLiteral("-a")}},
        {QStringLiteral("ss"), {QStringLiteral("-t"), QStringLiteral("-u"), QStringLiteral("-l"),
                                QStringLiteral("-n"), QStringLiteral("-p"), QStringLiteral("-a"),
                                QStringLiteral("-s")}},

        // 进程管理补充
        {QStringLiteral("ps"), {QStringLiteral("-ef"), QStringLiteral("-aux"), QStringLiteral("-eLf"),
                                QStringLiteral("-u"), QStringLiteral("-p"), QStringLiteral("-f"),
                                QStringLiteral("-l")}},
        {QStringLiteral("kill"), {QStringLiteral("-9"), QStringLiteral("-15"),
                                  QStringLiteral("-TERM"), QStringLiteral("-KILL"),
                                  QStringLiteral("-HUP")}},
        {QStringLiteral("top"), {QStringLiteral("-d"), QStringLiteral("-p"), QStringLiteral("-u"),
                                 QStringLiteral("-n")}},
        {QStringLiteral("free"), {QStringLiteral("-h"), QStringLiteral("-m"), QStringLiteral("-g"),
                                  QStringLiteral("-s")}},

        // 系统管理补充
        {QStringLiteral("chmod"), {QStringLiteral("-R"), QStringLiteral("-v"), QStringLiteral("-c")}},
        {QStringLiteral("chown"), {QStringLiteral("-R"), QStringLiteral("-v"), QStringLiteral("-c")}},
        {QStringLiteral("systemctl"), {QStringLiteral("start"), QStringLiteral("stop"),
                                       QStringLiteral("restart"), QStringLiteral("status"),
                                       QStringLiteral("enable"), QStringLiteral("disable"),
                                       QStringLiteral("reload"), QStringLiteral("is-active"),
                                       QStringLiteral("is-enabled")}},
        {QStringLiteral("journalctl"), {QStringLiteral("-f"), QStringLiteral("-n"),
                                        QStringLiteral("-u"), QStringLiteral("-p"),
                                        QStringLiteral("--since"), QStringLiteral("--until"),
                                        QStringLiteral("-o short-iso")}},

        // 容器/版本控制补充
        {QStringLiteral("docker"), {QStringLiteral("ps"), QStringLiteral("images"),
                                    QStringLiteral("pull"), QStringLiteral("run"),
                                    QStringLiteral("exec"), QStringLiteral("logs"),
                                    QStringLiteral("compose"), QStringLiteral("build"),
                                    QStringLiteral("rm"), QStringLiteral("stop"),
                                    QStringLiteral("start"), QStringLiteral("restart"),
                                    QStringLiteral("inspect")}},
        {QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("add"),
                                 QStringLiteral("commit"), QStringLiteral("push"),
                                 QStringLiteral("pull"), QStringLiteral("clone"),
                                 QStringLiteral("branch"), QStringLiteral("checkout"),
                                 QStringLiteral("merge"), QStringLiteral("log")}},

        // 包管理补充
        {QStringLiteral("apt"), {QStringLiteral("update"), QStringLiteral("upgrade"),
                                 QStringLiteral("install"), QStringLiteral("remove"),
                                 QStringLiteral("autoremove"), QStringLiteral("search"),
                                 QStringLiteral("show")}},
        {QStringLiteral("yum"), {QStringLiteral("install"), QStringLiteral("remove"),
                                 QStringLiteral("update"), QStringLiteral("list"),
                                 QStringLiteral("search"), QStringLiteral("clean all"),
                                 QStringLiteral("makecache")}},
    };
    return options;
}

// 递归遍历命令树，收集命令名与选项。
// 对应Python: function/ssh_prompt_client.py::_walk_tree + load_linux_commands L123-135
static void collectFromTree(const QList<CommandEntry> &entries,
                            QSet<QString> &commands,
                            QHash<QString, QSet<QString>> &options)
{
    // 分类节点名称集合，遍历时跳过（与 Python 侧硬编码一致）。
    static const QSet<QString> categoryNames = {
        QStringLiteral("文件管理"), QStringLiteral("网络管理"),
        QStringLiteral("系统管理"), QStringLiteral("磁盘管理"),
        QStringLiteral("进程管理"), QStringLiteral("文本处理"),
        QStringLiteral("用户管理"), QStringLiteral("权限管理"),
    };
    for (const CommandEntry &entry : entries) {
        const QString cmd = entry.command.trimmed();
        // command 为空 / 含空格 / 是分类名 → 跳过（子节点仍需遍历）
        if (!cmd.isEmpty() && !cmd.contains(QLatin1Char(' '))
            && !categoryNames.contains(cmd)) {
            commands.insert(cmd);
            const QSet<QString> opts = extractOptions(entry.option);
            if (!opts.isEmpty())
                options[cmd].unite(opts);
        }
        if (entry.hasChildren())
            collectFromTree(entry.children, commands, options);
    }
}

bool CommandIndex::load(const QString &jsonPath)
{
    QSet<QString> commands;
    QHash<QString, QSet<QString>> options;

    // 1. JSON 树（失败时静默跳过，内置表仍可用 — 对应 Python 的 try/except pass）
    bool jsonOk = false;
    const QString path = jsonPath.isEmpty() ? resolveCommandsFile() : jsonPath;
    if (!path.isEmpty()) {
        FrequentlyUsedCommands tree;
        if (tree.load(path)) {
            collectFromTree(tree.entries(), commands, options);
            jsonOk = true;
        }
    }

    // 2. 合并内置表（对应 Python L139-141）
    for (const QString &cmd : builtinCommands())
        commands.insert(cmd);
    const QHash<QString, QStringList> &builtins = builtinOptions();
    for (auto it = builtins.constBegin(); it != builtins.constEnd(); ++it) {
        QSet<QString> &dst = options[it.key()];
        for (const QString &opt : it.value())
            dst.insert(opt);
    }

    // 3. 排序落盘到成员（对应 Python L142 的 sorted）
    m_commands = QStringList(commands.constBegin(), commands.constEnd());
    std::sort(m_commands.begin(), m_commands.end());
    m_options.clear();
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        QStringList lst(it.value().constBegin(), it.value().constEnd());
        std::sort(lst.begin(), lst.end());
        m_options.insert(it.key(), lst);
    }
    return jsonOk;
}

// 在有序列表上二分求 [prefix, prefix+"\uffff") 区间并截取上限条数。
// 对应Python: cube-shell.py L7880-7882 / L7892-7894 的 bisect_left 逻辑
static QStringList prefixRange(const QStringList &sorted, const QString &prefix, int limit)
{
    const QString hi = prefix + QChar(0xffff);
    const auto lo = std::lower_bound(sorted.constBegin(), sorted.constEnd(), prefix);
    auto end = std::lower_bound(lo, sorted.constEnd(), hi);
    if (end - lo > limit)
        end = lo + limit;
    QStringList out;
    out.reserve(int(end - lo));
    for (auto it = lo; it != end; ++it)
        out.append(*it);
    return out;
}

// 对应Python: cube-shell.py::_compute_suggestions (L7867-7895)
QStringList CommandIndex::computeSuggestions(const QString &text) const
{
    // lstrip 后为空 → 返回全部命令
    QString s = text;
    while (!s.isEmpty() && s.front().isSpace())
        s.remove(0, 1);
    if (s.isEmpty())
        return m_commands;
    const QStringList parts = s.split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return m_commands;
    if (parts.size() == 1) {
        // 单 token：命令表上二分前缀匹配，截 ≤80 条
        return prefixRange(m_commands, parts.first(), 80);
    }
    // 多 token 且末 token 以 '-' 开头：在该命令的选项表上二分，截 ≤80 条
    const QString &cmd = parts.first();
    const QString &last = parts.last();
    if (last.startsWith(QLatin1Char('-')))
        return prefixRange(m_options.value(cmd), last, 80);
    return {};
}

} // namespace cubeshell
