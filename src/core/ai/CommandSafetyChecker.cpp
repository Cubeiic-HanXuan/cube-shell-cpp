// CommandSafetyChecker.cpp — rule tables ported 1:1 from safety.py.
//
// 对应Python: core/ai/safety.py

#include "CommandSafetyChecker.h"

namespace cubeshell {

// ---------------------------------------------------------------------------
// RiskLevel helpers
// ---------------------------------------------------------------------------

// 对应Python: RiskLevel.value ("safe"/"low"/...)
QString riskLevelName(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("safe");
    case RiskLevel::Low:      return QStringLiteral("low");
    case RiskLevel::Medium:   return QStringLiteral("medium");
    case RiskLevel::High:     return QStringLiteral("high");
    case RiskLevel::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("low");
}

// 对应Python: confirm_dialog.py::_RISK_LABELS
QString riskLevelLabel(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("安全");
    case RiskLevel::Low:      return QStringLiteral("低");
    case RiskLevel::Medium:   return QStringLiteral("中");
    case RiskLevel::High:     return QStringLiteral("高");
    case RiskLevel::Critical: return QStringLiteral("危险");
    }
    return QStringLiteral("低");
}

// 对应Python: confirm_dialog.py::_RISK_COLORS
QString riskLevelColor(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe:     return QStringLiteral("#4CAF50");
    case RiskLevel::Low:      return QStringLiteral("#2196F3");
    case RiskLevel::Medium:   return QStringLiteral("#FF9800");
    case RiskLevel::High:     return QStringLiteral("#F44336");
    case RiskLevel::Critical: return QStringLiteral("#B71C1C");
    }
    return QStringLiteral("#2196F3");
}

// ---------------------------------------------------------------------------
// CommandSafetyChecker
// ---------------------------------------------------------------------------

CommandSafetyChecker::CommandSafetyChecker()
{
    const auto rule = [](const char *pattern, const char *desc) {
        Rule r;
        r.pattern = QRegularExpression(QString::fromUtf8(pattern));
        r.description = QString::fromUtf8(desc);
        return r;
    };

    // 对应Python: _CRITICAL_PATTERNS（黑名单，绝对阻止）
    m_criticalRules = {
        rule(R"(\brm\s+(-[a-zA-Z]*f[a-zA-Z]*\s+)?-[a-zA-Z]*r[a-zA-Z]*\s+/(\s|$|\*))",
             "递归强制删除根目录"),
        rule(R"(\brm\s+(-[a-zA-Z]*r[a-zA-Z]*\s+)?-[a-zA-Z]*f[a-zA-Z]*\s+/(\s|$|\*))",
             "递归强制删除根目录"),
        rule(R"(\bdd\s+.*if=.*of=/dev/)",
             "直接写入块设备，可能销毁磁盘数据"),
        rule(R"(\bmkfs\b)",
             "格式化文件系统"),
        rule(R"(\bwipefs\b)",
             "擦除文件系统签名"),
        rule(R"(:\(\)\s*\{\s*:\|:\s*&\s*\}\s*;\s*:)",
             "Fork 炸弹，将耗尽系统资源"),
        rule(R"(\bchmod\s+(-[a-zA-Z]*R[a-zA-Z]*\s+)?777\s+/(\s|$))",
             "递归修改根目录权限为 777"),
        rule(R"(\bchown\s+.*-[a-zA-Z]*R[a-zA-Z]*\s+.*\s+/(\s|$))",
             "递归修改根目录所有者"),
        rule(R"(>\s*/dev/sd[a-z])",
             "覆写块设备"),
    };

    // 对应Python: _HIGH_PATTERNS（需确认但不直接阻止）
    m_highRules = {
        rule(R"(\bshutdown\b)",   "关机命令"),
        rule(R"(\breboot\b)",     "重启命令"),
        rule(R"(\bhalt\b)",       "停机命令"),
        rule(R"(\bpoweroff\b)",   "关机命令"),
        rule(R"(\biptables\b)",   "防火墙规则修改"),
        rule(R"(\bufw\b)",        "防火墙配置修改"),
        rule(R"(\bifconfig\b.*\b(up|down)\b)", "网络接口配置修改"),
        rule(R"(\bip\s+link\s+set\b)",         "网络链路配置修改"),
        rule(R"(\bip\s+addr\s+(add|del)\b)",   "网络地址配置修改"),
        rule(R"(\bip\s+route\s+(add|del)\b)",  "路由表修改"),
        rule(R"(\bnmcli\b.*\bmod)",            "NetworkManager 配置修改"),
    };

    // 对应Python: _MEDIUM_PATTERNS
    m_mediumRules = {
        rule(R"(\bsudo\b)",
             "使用 sudo 提权"),
        // 写操作
        rule(R"([^|]\s*>\s*[^>])",
             "输出重定向（覆写文件）"),
        rule(R"(>>)",
             "输出追加重定向"),
        rule(R"(\btee\b)",
             "tee 写文件"),
        rule(R"(\bsed\s+.*-i\b)",
             "sed 原地编辑文件"),
        // 服务管理
        rule(R"(\bsystemctl\s+(stop|restart|disable)\b)",
             "服务管理操作"),
        rule(R"(\bservice\s+\S+\s+(stop|restart)\b)",
             "服务管理操作"),
        // 包管理
        rule(R"(\b(apt|apt-get|yum|dnf|pacman|zypper)\s+.*(install|remove|purge)\b)",
             "包管理操作"),
        rule(R"(\bpip\s+install\b)",
             "Python 包安装"),
        rule(R"(\bnpm\s+install\b)",
             "Node.js 包安装"),
    };

    // 对应Python: _SAFE_PREFIXES（只读命令前缀白名单）
    m_safePrefixes = {
        // 文件查看
        QStringLiteral("ls"), QStringLiteral("cat"), QStringLiteral("head"),
        QStringLiteral("tail"), QStringLiteral("grep"), QStringLiteral("find"),
        // 磁盘与内存
        QStringLiteral("df"), QStringLiteral("du"), QStringLiteral("free"),
        // 进程与系统
        QStringLiteral("top"), QStringLiteral("htop"), QStringLiteral("ps"),
        QStringLiteral("uptime"), QStringLiteral("uname"),
        QStringLiteral("whoami"), QStringLiteral("hostname"),
        QStringLiteral("date"),
        // 文本处理（只读）
        QStringLiteral("wc"), QStringLiteral("sort"), QStringLiteral("uniq"),
        // 网络查看
        QStringLiteral("ss"), QStringLiteral("netstat"),
        QStringLiteral("ip addr"), QStringLiteral("ip route"),
        QStringLiteral("ping"), QStringLiteral("traceroute"),
        QStringLiteral("dig"), QStringLiteral("nslookup"),
        // 服务状态查看
        QStringLiteral("systemctl status"), QStringLiteral("journalctl"),
        // 命令查询
        QStringLiteral("which"), QStringLiteral("type"), QStringLiteral("file"),
    };
}

// 对应Python: CommandSafetyChecker.check
SafetyCheckResult CommandSafetyChecker::check(const QString &command) const
{
    SafetyCheckResult result;
    const QString cmd = command.trimmed();
    if (cmd.isEmpty()) {
        result.riskLevel = RiskLevel::Safe;
        result.isAllowed = true;
        result.reason = QStringLiteral("空命令");
        return result;
    }

    // 1) CRITICAL 黑名单 —— 直接阻止
    for (const Rule &r : m_criticalRules) {
        if (r.pattern.match(cmd).hasMatch()) {
            result.riskLevel = RiskLevel::Critical;
            result.isAllowed = false;
            result.reason = QStringLiteral("命令被阻止：%1").arg(r.description);
            result.warnings << QStringLiteral("匹配危险模式: %1").arg(r.description);
            return result;
        }
    }

    // 2) HIGH 风险 —— 允许但强制确认
    QStringList highWarnings;
    for (const Rule &r : m_highRules) {
        if (r.pattern.match(cmd).hasMatch())
            highWarnings << r.description;
    }
    if (!highWarnings.isEmpty()) {
        result.riskLevel = RiskLevel::High;
        result.isAllowed = true;
        result.reason = QStringLiteral("高风险操作，需二次确认：%1")
                            .arg(highWarnings.join(QStringLiteral("; ")));
        result.warnings = highWarnings;
        return result;
    }

    // 3) SAFE 白名单 —— 自动放行
    if (isSafeCommand(cmd)) {
        result.riskLevel = RiskLevel::Safe;
        result.isAllowed = true;
        result.reason = QStringLiteral("只读查询命令，自动执行");
        return result;
    }

    // 4) MEDIUM 风险规则
    QStringList mediumWarnings;
    for (const Rule &r : m_mediumRules) {
        if (r.pattern.match(cmd).hasMatch())
            mediumWarnings << r.description;
    }
    if (!mediumWarnings.isEmpty()) {
        result.riskLevel = RiskLevel::Medium;
        result.isAllowed = true;
        result.reason = QStringLiteral("中等风险操作，需用户确认：%1")
                            .arg(mediumWarnings.join(QStringLiteral("; ")));
        result.warnings = mediumWarnings;
        return result;
    }

    // 5) 默认归为 LOW —— 提示后自动执行
    result.riskLevel = RiskLevel::Low;
    result.isAllowed = true;
    result.reason = QStringLiteral("未匹配已知风险模式，按低风险处理");
    return result;
}

// 对应Python: CommandSafetyChecker.check_batch
QList<SafetyCheckResult> CommandSafetyChecker::checkBatch(
    const QStringList &commands) const
{
    QList<SafetyCheckResult> results;
    results.reserve(commands.size());
    for (const QString &cmd : commands)
        results.append(check(cmd));
    return results;
}

// 对应Python: CommandSafetyChecker._is_safe_command
bool CommandSafetyChecker::isSafeCommand(const QString &cmd) const
{
    const QString effective = stripEnvPrefix(cmd);
    for (const QString &prefix : m_safePrefixes) {
        if (effective == prefix
            || effective.startsWith(prefix + QLatin1Char(' ')))
            return true;
    }
    return false;
}

// 对应Python: CommandSafetyChecker._strip_env_prefix
// 例如 "LC_ALL=C LANG=en_US ls -la" → "ls -la"
QString CommandSafetyChecker::stripEnvPrefix(const QString &cmd)
{
    const QStringList parts = cmd.split(QRegularExpression(QStringLiteral("\\s+")),
                                        Qt::SkipEmptyParts);
    int idx = 0;
    for (const QString &part : parts) {
        if (part.contains(QLatin1Char('=')) && !part.startsWith(QLatin1Char('=')))
            ++idx;
        else
            break;
    }
    if (idx >= parts.size())
        return cmd;
    return parts.mid(idx).join(QLatin1Char(' '));
}

} // namespace cubeshell
