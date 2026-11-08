// Static, conservative shell assessment. This is not a shell interpreter or a
// sandbox: only explicitly understood query forms qualify for auto-execution.
#include "CommandSafetyChecker.h"

#include <QDir>
#include <QRegularExpression>
#include <QSet>

namespace cubeshell {

QString riskLevelName(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("safe");
    case RiskLevel::Low:      return QStringLiteral("low");
    case RiskLevel::Medium:   return QStringLiteral("medium");
    case RiskLevel::High:     return QStringLiteral("high");
    case RiskLevel::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("medium");
}

QString riskLevelLabel(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("安全");
    case RiskLevel::Low:      return QStringLiteral("低");
    case RiskLevel::Medium:   return QStringLiteral("中");
    case RiskLevel::High:     return QStringLiteral("高");
    case RiskLevel::Critical: return QStringLiteral("危险");
    }
    return QStringLiteral("中");
}

QString riskLevelColor(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("#4CAF50");
    case RiskLevel::Low:      return QStringLiteral("#2196F3");
    case RiskLevel::Medium:   return QStringLiteral("#FF9800");
    case RiskLevel::High:     return QStringLiteral("#F44336");
    case RiskLevel::Critical: return QStringLiteral("#B71C1C");
    }
    return QStringLiteral("#FF9800");
}

namespace {

struct Assessment {
    RiskLevel level = RiskLevel::Safe;
    QStringList warnings;
    bool privileged = false;

    void raise(RiskLevel risk, const QString &reason)
    {
        if (risk > level) level = risk;
        if (!reason.isEmpty() && !warnings.contains(reason)) warnings.append(reason);
    }
    void merge(const Assessment &other)
    {
        if (other.level > level) level = other.level;
        for (const auto &warning : other.warnings)
            if (!warnings.contains(warning)) warnings.append(warning);
        privileged |= other.privileged;
    }
};

struct Token {
    QString text;
    bool op = false;
    bool dynamic = false;
    bool quoted = false;
    bool assignment = false; // the NAME= prefix itself must be unquoted/unescaped
};
struct ShellTokens {
    QList<Token> tokens;
    QStringList nestedScripts;
    QString error;
};

bool oneOf(const QString &value, std::initializer_list<const char *> values)
{
    for (const char *candidate : values)
        if (value == QLatin1String(candidate)) return true;
    return false;
}

// Consume $(...), ${...}, and backticks as ONE word fragment. Shell operators
// inside them are not operators of the enclosing command. Nested executable
// fragments are checked separately, but dynamic expansion always needs approval.
bool consumeExpansion(const QString &s, qsizetype &i, ShellTokens &out, int depth)
{
    if (depth > 16) return false;
    const bool backtick = s.at(i) == QLatin1Char('`');
    const bool braces = !backtick && i + 1 < s.size() && s.at(i + 1) == QLatin1Char('{');
    const qsizetype start = i + (backtick ? 1 : 2);
    i = start;
    int nesting = 1;
    QChar quote;
    for (; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) { ++i; continue; }
        if (backtick && c == QLatin1Char('`')) {
            out.nestedScripts.append(s.mid(start, i - start));
            return true;
        }
        if (c == QLatin1Char('\'') && quote != QLatin1Char('"')) {
            quote = quote.isNull() ? c : QChar();
            continue;
        }
        if (quote == QLatin1Char('\'')) continue;
        if (c == QLatin1Char('"')) {
            quote = quote.isNull() ? c : QChar();
            continue;
        }
        if (c == QLatin1Char('`') || (c == QLatin1Char('$') && i + 1 < s.size()
                && oneOf(s.mid(i + 1, 1), {"(", "{"}))) {
            ShellTokens nested;
            if (!consumeExpansion(s, i, nested, depth + 1)) return false;
            // Executable bodies are recursively analyzed later. Only parameter
            // expansions need to forward their nested executable fragments here.
            if (braces) out.nestedScripts.append(nested.nestedScripts);
            continue;
        }
        if (!quote.isNull()) continue;
        if (c == (braces ? QLatin1Char('{') : QLatin1Char('('))) ++nesting;
        if (c == (braces ? QLatin1Char('}') : QLatin1Char(')')) && --nesting == 0) {
            if (!braces) out.nestedScripts.append(s.mid(start, i - start));
            return true;
        }
    }
    return false;
}

ShellTokens tokenize(const QString &s)
{
    ShellTokens out;
    Token word;
    bool started = false;
    QChar quote;
    const auto flush = [&] {
        if (started) out.tokens.append(word);
        word = {};
        started = false;
    };
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c.unicode() == 0 || (c.unicode() < 32 && c != '\t' && c != '\n')) {
            out.error = QStringLiteral("包含不支持的控制字符");
            continue;
        }
        if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
            if (++i >= s.size()) { out.error = QStringLiteral("不完整的转义"); break; }
            const QChar next = s.at(i);
            if (next == QLatin1Char('\n')) continue;
            // In double quotes, backslash only escapes $, `, ", \\ and newline.
            if (quote == QLatin1Char('"') && !oneOf(QString(next), {"$", "`", "\"", "\\"}))
                word.text += QLatin1Char('\\');
            word.text += next;
            word.quoted = true;
            started = true;
            continue;
        }
        if (c == QLatin1Char('\'') && quote != QLatin1Char('"')) {
            quote = quote.isNull() ? c : QChar();
            word.quoted = started = true;
            continue;
        }
        if (c == QLatin1Char('"') && quote != QLatin1Char('\'')) {
            quote = quote.isNull() ? c : QChar();
            word.quoted = started = true;
            continue;
        }
        if (quote != QLatin1Char('\'') && (c == QLatin1Char('$') || c == QLatin1Char('`'))) {
            word.dynamic = started = true;
            const qsizetype start = i;
            if (c == QLatin1Char('`') || (i + 1 < s.size() && oneOf(s.mid(i + 1, 1), {"(", "{"}))) {
                if (!consumeExpansion(s, i, out, 0)) {
                    out.error = QStringLiteral("不完整或过深的 Shell 展开");
                    break;
                }
            }
            word.text += s.mid(start, i - start + 1);
            continue;
        }
        if (!quote.isNull()) { word.text += c; started = true; continue; }
        if (c == QLatin1Char('#') && !started) {
            while (i < s.size() && s.at(i) != QLatin1Char('\n')) ++i;
            if (i == s.size()) break;
        } else if (c == ' ' || c == '\t') { flush(); continue; }
        if (s.at(i) == QLatin1Char('\n') || QStringLiteral(";&|<>()").contains(c)) {
            // A digit immediately before a redirection is an IO number, not an argument.
            if ((c == QLatin1Char('>') || c == QLatin1Char('<')) && started && !word.quoted
                && QRegularExpression(QStringLiteral("^[0-9]+$")).match(word.text).hasMatch()) {
                word = {}; started = false;
            }
            flush();
            QString op(s.at(i));
            if (i + 1 < s.size()) {
                const QString pair = s.mid(i, 2);
                if (oneOf(pair, {"&&", "||", "|&", ">>", ">|", ">&", "<&", "<>", "<<", "&>"})) {
                    op = pair; ++i;
                    if ((op == "<<" && i + 1 < s.size() && s.at(i + 1) == '<')
                        || (op == "&>" && i + 1 < s.size() && s.at(i + 1) == '>')) op += s.at(++i);
                }
            }
            out.tokens.append({op, true});
            continue;
        }
        if (oneOf(QString(c), {"*", "?", "[", "{", "}", "~"})) word.dynamic = true;
        static const QRegularExpression variableName(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
        if (c == '=' && !word.quoted && variableName.match(word.text).hasMatch()) word.assignment = true;
        word.text += c;
        started = true;
    }
    if (!quote.isNull()) out.error = QStringLiteral("引号未闭合");
    flush();
    return out;
}

QString executableName(const QString &word)
{
    // Do not trust an arbitrary ./ls or /tmp/cat just because its basename matches.
    for (const char *dir : {"/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/"}) {
        const QString prefix = QLatin1String(dir);
        if (word.startsWith(prefix) && !word.mid(prefix.size()).contains('/'))
            return word.mid(prefix.size());
    }
    return word;
}

bool isAssignment(const QString &word)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*="));
    return re.match(word).hasMatch();
}

bool harmlessEnvironment(const QString &word)
{
    const QString key = word.section('=', 0, 0);
    return oneOf(key, {"LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE", "LC_MESSAGES", "LC_TIME",
                       "LC_NUMERIC", "LC_COLLATE", "LC_MONETARY", "TZ", "TERM", "COLUMNS", "LINES"});
}

// Parse known global options with explicit arity. Never find a subcommand by
// blindly skipping '-...' tokens: option values can themselves say 'status'.
QStringList afterOptions(const QStringList &args, const QSet<QString> &flags,
                         const QSet<QString> &valued, bool *ok)
{
    *ok = true;
    int i = 0;
    for (; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == "--") { ++i; break; }
        if (!arg.startsWith('-') || arg == "-") break;
        if (flags.contains(arg)) continue;
        const QString key = arg.section('=', 0, 0);
        if (valued.contains(key)) {
            if (!arg.contains('=') && ++i >= args.size()) { *ok = false; return {}; }
            continue;
        }
        *ok = false;
        return {};
    }
    return args.mid(i);
}

bool hasOption(const QStringList &args, std::initializer_list<const char *> longs,
               const QString &shorts = {})
{
    for (const auto &arg : args) {
        if (arg == "--") break;
        if (oneOf(arg.section('=', 0, 0), longs)) return true;
        if (arg.startsWith('-') && !arg.startsWith("--"))
            for (const QChar c : shorts) if (arg.mid(1).contains(c)) return true;
    }
    return false;
}

// GNU tools may parse options even after their script/filename operands.
// Extra program/extension options must not hide behind a verified first script.
bool onlyFileOperands(const QStringList &args)
{
    for (const auto &arg : args) {
        if (arg == "--") return true;
        if (arg.startsWith('-') && arg != "-") return false;
    }
    return true;
}

bool blockDevice(const QString &target)
{
    static const QRegularExpression re(QStringLiteral(
        "^/dev/(?:sd[a-z]|hd[a-z]|vd[a-z]|xvd[a-z]|nvme[0-9]|mmcblk[0-9]|disk[0-9]|rdisk[0-9]|mapper/|dm-[0-9]|md[0-9]|loop[0-9])"));
    return re.match(QDir::cleanPath(target)).hasMatch();
}

Assessment analyze(const QString &command, int depth);

Assessment assessCommand(QStringList words, int depth)
{
    Assessment result;
    const auto uncertain = [&](const QString &why) {
        result.raise(RiskLevel::Medium, why);
        return result;
    };
    if (depth > 8) return uncertain(QStringLiteral("命令嵌套过深，需人工确认"));
    // Peel only understood wrappers, preserving the highest risk they introduce.
    while (!words.isEmpty()) {
        if (isAssignment(words.first())) {
            if (!harmlessEnvironment(words.first()))
                result.raise(RiskLevel::Medium, QStringLiteral("环境变量可能改变命令执行行为：%1").arg(words.first().section('=', 0, 0)));
            words.removeFirst();
            continue;
        }
        const QString name = executableName(words.first());
        if (name == "sudo") {
            result.privileged = true;
            bool ok = false;
            words = afterOptions(words.mid(1), {"-n", "-S", "--non-interactive", "--stdin"},
                                 {"-u", "--user", "-g", "--group"}, &ok);
            if (!ok || words.isEmpty()) return uncertain(QStringLiteral("sudo 选项或执行目标需要人工确认"));
            continue;
        }
        if (name == "env") {
            words.removeFirst();
            if (!words.isEmpty() && words.first() == "--") words.removeFirst();
            if (!words.isEmpty() && words.first().startsWith('-'))
                return uncertain(QStringLiteral("env 选项可能改变执行环境"));
            continue;
        }
        if (name == "command") {
            words.removeFirst();
            if (!words.isEmpty() && oneOf(words.first(), {"-v", "-V"})) return result;
            if (!words.isEmpty() && words.first() == "--") words.removeFirst();
            continue;
        }
        if (name == "timeout" && words.size() > 2) {
            static const QRegularExpression duration(QStringLiteral("^[0-9]+(?:\\.[0-9]+)?[smhd]?$"));
            if (!duration.match(words.at(1)).hasMatch()) return uncertain(QStringLiteral("timeout 选项需要人工确认"));
            words = words.mid(2);
            continue;
        }
        break;
    }
    if (words.isEmpty()) return uncertain(QStringLiteral("仅设置环境或缺少执行目标"));
    const QString name = executableName(words.first());
    const QStringList args = words.mid(1);
    const auto risk = [&](RiskLevel level, const QString &why) {
        result.raise(level, QStringLiteral("%1：%2").arg(name, why));
        return result;
    };

    // Destructive rules operate on executable/argument boundaries, NOT prose or
    // filenames containing words such as 'reboot', 'mkfs' or 'shutdown'.
    if (name == "mkfs" || name.startsWith("mkfs.") || name == "wipefs")
        return risk(RiskLevel::Critical, QStringLiteral("格式化或擦除文件系统"));
    if (name == "dd") {
        for (const auto &arg : args)
            if (arg.startsWith("of=/dev/") && blockDevice(arg.mid(3)))
                return risk(RiskLevel::Critical, QStringLiteral("直接写入块设备，可能销毁磁盘数据"));
        return risk(RiskLevel::Medium, QStringLiteral("数据复制可能写入文件或设备"));
    }
    if (oneOf(name, {"rm", "chmod", "chown"})) {
        const bool recursive = hasOption(args, {"--recursive"}, name == "rm" ? "rR" : "R");
        for (const auto &arg : args) {
            if (arg.startsWith('-')) continue;
            const QString path = QDir::cleanPath(arg);
            if ((path == "/" || path == "/*") && (recursive || name == "chmod"))
                return risk(RiskLevel::Critical, QStringLiteral("删除根目录或修改根目录权限/所有者"));
        }
        return risk(RiskLevel::Medium, QStringLiteral("删除文件或修改权限/所有者"));
    }
    if (oneOf(name, {"shutdown", "reboot", "halt", "poweroff"}))
        return risk(RiskLevel::High, QStringLiteral("关机、停机或重启系统"));
    if (oneOf(name, {"sh", "bash", "dash", "zsh", "ksh"})) {
        if (args.size() >= 2 && oneOf(args.first(), {"-c", "-lc", "-ec"}))
            result.merge(analyze(args.at(1), depth + 1));
        return uncertain(QStringLiteral("Shell 脚本/启动环境需要人工确认"));
    }

    if (name == "find") {
        for (int i = 0; i < args.size(); ++i) {
            const auto &arg = args.at(i);
            if (!oneOf(arg, {"-delete", "-exec", "-execdir", "-ok", "-okdir", "-fprint", "-fprint0", "-fprintf", "-fls"})) continue;
            result.raise(RiskLevel::Medium, QStringLiteral("find 包含删除、写文件或执行其他命令的动作"));
            if (oneOf(arg, {"-exec", "-execdir", "-ok", "-okdir"})) {
                int end = i + 1;
                while (end < args.size() && !oneOf(args.at(end), {";", "+"})) ++end;
                result.merge(assessCommand(args.mid(i + 1, end - i - 1), depth + 1));
                i = end;
            }
        }
        return result;
    }
    if (name == "sort") {
        if (hasOption(args, {"--output", "--compress-program"}, "o"))
            return risk(RiskLevel::Medium, QStringLiteral("排序结果写文件或调用外部程序"));
        return result;
    }
    if (name == "uniq") {
        bool ok;
        const auto operands = afterOptions(args, {"-c", "-d", "-D", "-u", "-i", "-z", "--count", "--repeated", "--unique", "--ignore-case", "--zero-terminated"},
                                           {"-f", "-s", "-w", "--skip-fields", "--skip-chars", "--check-chars"}, &ok);
        if (ok && operands.size() <= 1) return result;
        return uncertain(QStringLiteral("uniq 可能指定了输出文件"));
    }
    if (name == "sed") {
        if (hasOption(args, {"--in-place"}, "i"))
            return risk(RiskLevel::Medium, QStringLiteral("sed 原地编辑文件"));
        bool ok;
        auto operands = afterOptions(args, {"-n", "--quiet", "--silent", "-E", "-r"}, {}, &ok);
        // Deliberately narrow: arbitrary sed supports e/w commands, including
        // suffix flags on s///. Unknown programs must not be labelled read-only.
        static const QRegularExpression printRange(QStringLiteral("^\\s*(?:(?:[0-9]+|\\$)(?:,(?:[0-9]+|\\$))?)?[p=q]\\s*$"));
        if (ok && !operands.isEmpty() && printRange.match(operands.first()).hasMatch()
            && onlyFileOperands(operands.mid(1))) return result;
        return uncertain(QStringLiteral("sed 脚本可能执行命令或写文件，未确认是只读形式"));
    }
    if (oneOf(name, {"awk", "gawk", "mawk"})) {
        bool ok;
        auto operands = afterOptions(args, {}, {"-F", "-v"}, &ok);
        static const QRegularExpression fields(QStringLiteral("^\\s*\\{\\s*print(?:\\s+\\$[0-9]+(?:\\s*,\\s*\\$[0-9]+)*)?\\s*;?\\s*\\}\\s*$"));
        if (ok && !operands.isEmpty() && fields.match(operands.first()).hasMatch()
            && onlyFileOperands(operands.mid(1))) return result;
        return uncertain(QStringLiteral("awk 脚本可能执行命令或写文件，未确认是只读形式"));
    }
    if (name == "date") {
        bool ok;
        const auto rest = afterOptions(args, {"-u", "--utc", "--universal", "-R", "--rfc-email", "-I", "--iso-8601"},
                                       {"-d", "--date", "-r", "--reference"}, &ok);
        if (ok && (rest.isEmpty() || (rest.size() == 1 && rest.first().startsWith('+')))) return result;
        return risk(RiskLevel::Medium, QStringLiteral("日期参数可能设置系统时间"));
    }
    if (name == "hostname") {
        for (const auto &arg : args)
            if (!oneOf(arg, {"-a", "-A", "-d", "-f", "-i", "-I", "-s", "--alias", "--all-fqdns", "--domain", "--fqdn", "--ip-address", "--all-ip-addresses", "--short"}))
                return risk(RiskLevel::Medium, QStringLiteral("参数可能修改主机名"));
        return result;
    }
    if (name == "ss") {
        if (hasOption(args, {"--kill"}, "K")) return risk(RiskLevel::High, QStringLiteral("强制关闭网络连接"));
        if (hasOption(args, {"--diag"}, "D")) return risk(RiskLevel::Medium, QStringLiteral("将 socket 信息写入文件"));
        return result;
    }
    if (name == "file") {
        if (hasOption(args, {"--compile"}, "C")) return risk(RiskLevel::Medium, QStringLiteral("编译并写入 magic 文件"));
        return result;
    }
    if (name == "systemctl") {
        bool ok;
        auto rest = afterOptions(args, {"--no-pager", "--no-legend", "--plain", "--all", "-a", "--full", "-l", "--failed", "--user", "--system", "--quiet", "-q"},
                                 {"--type", "-t", "--state", "--property", "-p"}, &ok);
        if (!ok || rest.isEmpty()) return uncertain(QStringLiteral("systemctl 子命令或选项未识别"));
        if (oneOf(rest.first(), {"reboot", "poweroff", "halt", "kexec", "rescue", "emergency", "isolate"}))
            return risk(RiskLevel::High, QStringLiteral("系统电源或运行模式变更"));
        if (oneOf(rest.first(), {"status", "show", "cat", "is-active", "is-enabled", "is-failed", "is-system-running", "list-units", "list-unit-files", "list-timers", "list-sockets", "list-jobs", "list-dependencies", "get-default"})) return result;
        return risk(RiskLevel::Medium, QStringLiteral("服务管理或配置操作"));
    }
    if (name == "service") {
        if (args.size() == 2 && args.at(1) == "status") return result;
        return risk(RiskLevel::Medium, QStringLiteral("服务管理操作"));
    }
    if (name == "journalctl") {
        if (hasOption(args, {"--rotate", "--flush", "--sync", "--relinquish-var", "--smart-relinquish-var", "--vacuum-size", "--vacuum-time", "--vacuum-files", "--setup-keys", "--update-catalog"}))
            return risk(RiskLevel::Medium, QStringLiteral("修改或清理系统日志"));
        return result;
    }
    if (name == "ip") {
        bool ok;
        const auto rest = afterOptions(args, {"-4", "-6", "-br", "-brief", "--brief", "-o", "-oneline", "-s", "-stats", "-d", "-details", "-j", "-json", "-p", "-pretty", "-c"}, {}, &ok);
        if (!ok || rest.isEmpty()) return uncertain(QStringLiteral("ip 选项或对象未识别"));
        if (rest.size() > 1 && oneOf(rest.at(1), {"add", "del", "delete", "set", "replace", "change", "append", "flush", "restore"}))
            return risk(RiskLevel::High, QStringLiteral("网络链路、地址或路由配置修改"));
        if (oneOf(rest.first(), {"addr", "address", "a", "route", "r", "link", "l", "rule", "neigh", "neighbor", "neighbour"})
            && (rest.size() == 1 || oneOf(rest.at(1), {"show", "list", "lst", "get"}))) return result;
        return uncertain(QStringLiteral("ip 子命令未确认是只读查询"));
    }
    if (name == "ifconfig") {
        if (args.size() <= 1) return result;
        return risk(RiskLevel::High, QStringLiteral("网络接口配置修改"));
    }
    if (oneOf(name, {"iptables", "ip6tables"})) {
        bool query = false;
        for (int i = 0; i < args.size(); ++i) {
            const QString &arg = args.at(i);
            if (oneOf(arg, {"-L", "-S", "--list", "--list-rules"})) { query = true; continue; }
            if (oneOf(arg, {"-t", "--table"})) { if (++i >= args.size()) query = false; continue; }
            if (oneOf(arg, {"--numeric", "--verbose", "--exact", "--line-numbers"})) continue;
            if (arg.startsWith('-')) {
                static const QRegularExpression readFlags(QStringLiteral("^-[LSnvx]+$"));
                if (!readFlags.match(arg).hasMatch()) return risk(RiskLevel::High, QStringLiteral("防火墙修改或未识别的选项"));
                query |= arg.contains('L') || arg.contains('S');
            }
        }
        if (query) return result;
        return risk(RiskLevel::High, QStringLiteral("防火墙规则修改"));
    }
    if (name == "ufw") {
        if (!args.isEmpty() && oneOf(args.first(), {"status", "show", "version"})) return result;
        return risk(RiskLevel::High, QStringLiteral("防火墙配置修改"));
    }
    if (name == "nmcli") {
        bool ok;
        auto rest = afterOptions(args, {"-t", "--terse", "-p", "--pretty"}, {"-f", "--fields", "-g", "--get-values"}, &ok);
        if (ok && rest.size() >= 2 && oneOf(rest.first(), {"general", "device", "dev", "connection", "con"})
            && oneOf(rest.at(1), {"show", "status"})) return result;
        return risk(RiskLevel::High, QStringLiteral("NetworkManager 配置或连接操作"));
    }
    if (name == "docker") {
        // Global options (host/context/config) are deliberately not guessed.
        if (!args.isEmpty() && oneOf(args.first(), {"ps", "images", "inspect", "logs", "stats", "top", "port", "diff", "version", "info", "history"})) return result;
        if (args.size() >= 2) {
            const QString scope = args.first(), verb = args.at(1);
            if (oneOf(scope, {"container", "image", "network", "volume"}) && oneOf(verb, {"ls", "inspect"})) return result;
            if (scope == "compose" && oneOf(verb, {"ps", "logs", "images", "top", "ls", "version"})) return result;
        }
        return uncertain(QStringLiteral("Docker 操作未确认是只读查询"));
    }
    if (name == "kubectl") {
        bool ok;
        auto rest = afterOptions(args, {}, {"-n", "--namespace", "--context"}, &ok);
        if (ok && !rest.isEmpty() && oneOf(rest.first(), {"get", "describe", "logs", "top", "api-resources", "api-versions", "version"})
            && !hasOption(rest, {"--raw"})) return result;
        return uncertain(QStringLiteral("Kubernetes 操作未确认是只读查询"));
    }
    if (name == "git") {
        bool ok;
        auto rest = afterOptions(args, {"--no-pager"}, {"-C"}, &ok);
        if (ok && !rest.isEmpty() && oneOf(rest.first(), {"status", "rev-parse", "rev-list", "ls-files", "ls-tree", "count-objects"})) return result;
        return uncertain(QStringLiteral("Git 操作可能修改仓库或调用外部程序"));
    }
    if (name == "cd") {
        bool ok;
        const auto rest = afterOptions(args, {"-L", "-P", "-e"}, {}, &ok);
        if (ok && rest.size() <= 1) return risk(RiskLevel::Low, QStringLiteral("仅切换当前工作目录，不修改文件"));
        return uncertain(QStringLiteral("cd 选项未识别"));
    }
    if (name == "top") {
        if (hasOption(args, {"--batch"}, "b")) return result;
        return uncertain(QStringLiteral("交互式进程管理需要确认；只读采样请使用 top -b"));
    }
    if (name == "dmesg") {
        if (hasOption(args, {"--clear", "--read-clear", "--console-off", "--console-on", "--console-level"}, "cCDEn"))
            return risk(RiskLevel::Medium, QStringLiteral("清理内核日志或修改控制台日志设置"));
        return result;
    }
    if (name == "printf") {
        // Bash's %n assigns a shell variable; -v captures output into a variable.
        bool ok;
        const auto rest = afterOptions(args, {}, {}, &ok);
        static const QRegularExpression assignsVariable(QStringLiteral("%[-+ #0]*[0-9]*(?:\\.[0-9]+)?n"));
        if (!ok || (!rest.isEmpty() && assignsVariable.match(rest.first()).hasMatch()))
            return uncertain(QStringLiteral("printf 参数可能修改 Shell 变量"));
        return result;
    }
    static const QSet<QString> queries = {
        "ls", "cat", "head", "tail", "grep", "egrep", "fgrep", "wc", "df", "du", "free",
        "ps", "uptime", "uname", "whoami", "id", "groups", "pwd", "lsblk", "lscpu", "lsusb", "lspci",
        "lsmod", "vmstat", "iostat", "mpstat",
        "stat", "readlink", "realpath", "basename", "dirname", "which", "type", "printenv", "getconf",
        "netstat", "ping", "traceroute", "dig", "nslookup", "echo", "true", "false", ":"
    };
    if (queries.contains(name)) return result;
    return uncertain(QStringLiteral("未确认“%1”的命令及参数是只读操作，需人工确认").arg(name));
}

Assessment analyze(const QString &command, int depth)
{
    Assessment result;
    if (depth > 8 || command.size() > 65536) {
        result.raise(RiskLevel::Medium, QStringLiteral("命令过长或嵌套过深，需人工确认"));
        return result;
    }
    static const QRegularExpression forkBomb(QStringLiteral("^\\s*:\\(\\)\\s*\\{\\s*:\\|:\\s*&\\s*\\}\\s*;\\s*:\\s*$"));
    if (forkBomb.match(command).hasMatch()) {
        result.raise(RiskLevel::Critical, QStringLiteral("Fork 炸弹，将耗尽系统资源"));
        return result;
    }
    const auto parsed = tokenize(command);
    if (!parsed.error.isEmpty()) result.raise(RiskLevel::Medium, parsed.error);
    for (const auto &script : parsed.nestedScripts) result.merge(analyze(script, depth + 1));

    QStringList words;
    bool shellPrefix = true;
    bool redirectionOnly = false;
    QString previousSeparator;
    const auto flush = [&] {
        if (!words.isEmpty()) result.merge(assessCommand(words, depth));
        words.clear();
        shellPrefix = true;
        redirectionOnly = false;
    };
    for (int i = 0; i < parsed.tokens.size(); ++i) {
        const auto &token = parsed.tokens.at(i);
        if (token.dynamic) result.raise(RiskLevel::Medium, QStringLiteral("包含变量、通配符或动态展开，无法静态确认实际执行内容"));
        if (!token.op) {
            if (shellPrefix && isAssignment(token.text)) {
                if (!token.assignment)
                    result.raise(RiskLevel::Medium, QStringLiteral("带引号或转义的命令名不能当作环境赋值"));
            } else shellPrefix = false;
            words.append(token.text);
            previousSeparator.clear();
            continue;
        }
        if (oneOf(token.text, {">", ">>", ">|", "&>", "&>>", ">&", "<", "<&", "<>", "<<", "<<<"})) {
            redirectionOnly = true;
            if (++i >= parsed.tokens.size() || parsed.tokens.at(i).op) {
                result.raise(RiskLevel::Medium, QStringLiteral("重定向缺少静态目标"));
                --i;
                continue;
            }
            const auto &target = parsed.tokens.at(i);
            const bool fd = oneOf(token.text, {">&", "<&"});
            static const QRegularExpression descriptor(QStringLiteral("^(?:[0-9]+|-)$"));
            if (target.dynamic) result.raise(RiskLevel::Medium, QStringLiteral("重定向目标包含动态展开"));
            // Heredocs are intentionally not parsed as shell commands: their body
            // is data, not a list of executable statements.
            if (oneOf(token.text, {"<<", "<<<"})) {
                flush();
                result.raise(RiskLevel::Medium, QStringLiteral("Here-document/Here-string 需要人工确认"));
                return result;
            }
            if (token.text == "<" || (fd && descriptor.match(target.text).hasMatch() && !target.dynamic)) continue;
            if (!target.dynamic && blockDevice(target.text))
                result.raise(RiskLevel::Critical, QStringLiteral("重定向覆写块设备：%1").arg(target.text));
            else if (target.dynamic || target.text != "/dev/null")
                result.raise(RiskLevel::Medium, QStringLiteral("重定向写入文件：%1").arg(target.text));
            continue;
        }
        if (words.isEmpty() && !redirectionOnly && token.text != "\n")
            result.raise(RiskLevel::Medium, QStringLiteral("Shell 命令结构不完整或不受支持"));
        if (oneOf(token.text, {"&", "(", ")"}))
            result.raise(RiskLevel::Medium, QStringLiteral("后台执行或复合 Shell 结构需要人工确认"));
        flush();
        if (token.text != "\n" || previousSeparator.isEmpty()) previousSeparator = token.text;
    }
    flush();
    if (oneOf(previousSeparator, {"|", "|&", "&&", "||"}))
        result.raise(RiskLevel::Medium, QStringLiteral("管道或条件命令不完整"));
    return result;
}

} // namespace

SafetyCheckResult CommandSafetyChecker::check(const QString &command) const
{
    const Assessment assessment = analyze(command, 0);
    SafetyCheckResult result;
    result.riskLevel = assessment.level;
    result.isAllowed = assessment.level != RiskLevel::Critical;
    result.warnings = assessment.warnings;
    switch (assessment.level) {
    case RiskLevel::Safe:
        result.reason = command.trimmed().isEmpty() ? QStringLiteral("空命令")
            : assessment.privileged ? QStringLiteral("sudo 执行已识别的只读查询，自动执行")
                                    : QStringLiteral("已识别的只读查询命令，自动执行");
        break;
    case RiskLevel::Low:
        result.reason = QStringLiteral("低风险操作：%1").arg(result.warnings.join(QStringLiteral("；")));
        break;
    case RiskLevel::Medium:
        result.reason = QStringLiteral("中等风险操作或只读性未确认，需用户确认：%1").arg(result.warnings.join(QStringLiteral("；")));
        break;
    case RiskLevel::High:
        result.reason = QStringLiteral("高风险操作，需二次确认：%1").arg(result.warnings.join(QStringLiteral("；")));
        break;
    case RiskLevel::Critical:
        result.reason = QStringLiteral("命令被阻止：%1").arg(result.warnings.join(QStringLiteral("；")));
        break;
    }
    return result;
}

QList<SafetyCheckResult> CommandSafetyChecker::checkBatch(const QStringList &commands) const
{
    QList<SafetyCheckResult> results;
    results.reserve(commands.size());
    for (const auto &command : commands) results.append(check(command));
    return results;
}

} // namespace cubeshell
