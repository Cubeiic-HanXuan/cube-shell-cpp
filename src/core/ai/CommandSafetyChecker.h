#pragma once

// CommandSafetyChecker.h — static risk assessment for shell commands about
// to run in the remote SSH session.
//
// 对应Python: core/ai/safety.py (RiskLevel / SafetyCheckResult /
//             CommandSafetyChecker)
//
// Also defines struct AiCommand — the unit passed between SshAiAgent,
// CommandConfirmDialog and the executor thread.
// 对应Python: ssh_agent.py 中 dict {cmd, description, allow_failure,
//             interactive} + 附带的 safety 检查结果。

#include <QList>
#include <QMetaType>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace cubeshell {

// 对应Python: safety.py::RiskLevel
enum class RiskLevel {
    Safe,       // 只读查询，自动执行
    Low,        // 低风险，提示后自动执行
    Medium,     // 中等风险，需要用户确认
    High,       // 高风险，强制确认 + 二次确认
    Critical,   // 危险操作，直接阻止
};

// "safe"/"low"/"medium"/"high"/"critical" — 与 Python enum value 一致。
QString riskLevelName(RiskLevel level);
// 中文标签：安全/低/中/高/危险（confirm_dialog.py::_RISK_LABELS）。
QString riskLevelLabel(RiskLevel level);
// UI 颜色（confirm_dialog.py::_RISK_COLORS）。
QString riskLevelColor(RiskLevel level);

// 对应Python: safety.py::SafetyCheckResult (dataclass)
struct SafetyCheckResult {
    RiskLevel riskLevel = RiskLevel::Safe;
    bool isAllowed = true;      // 是否允许执行
    QString reason;             // 检查结论说明
    QStringList warnings;       // 警告信息
};

// One command proposed by the AI agent, plus its safety verdict.
// 对应Python: ssh_agent.py 的命令 dict（cmd/description/allow_failure/
//             interactive）+ safety 字段
struct AiCommand {
    QString cmd;
    QString description;
    bool allowFailure = false;
    bool interactive = false;
    SafetyCheckResult safety;
};

// Stateless rule engine; all patterns are pre-compiled at construction so an
// instance can be shared freely.
// 对应Python: safety.py::CommandSafetyChecker
class CommandSafetyChecker {
public:
    CommandSafetyChecker();

    // 对应Python: CommandSafetyChecker.check
    SafetyCheckResult check(const QString &command) const;

    // 对应Python: CommandSafetyChecker.check_batch
    QList<SafetyCheckResult> checkBatch(const QStringList &commands) const;

private:
    // 预编译正则 + 中文说明（对应 Python 的 (re.Pattern, str) 元组）
    struct Rule {
        QRegularExpression pattern;
        QString description;
    };

    // 对应Python: CommandSafetyChecker._is_safe_command
    bool isSafeCommand(const QString &cmd) const;
    // 对应Python: CommandSafetyChecker._strip_env_prefix
    static QString stripEnvPrefix(const QString &cmd);

    QList<Rule> m_criticalRules;    // _CRITICAL_PATTERNS
    QList<Rule> m_highRules;        // _HIGH_PATTERNS
    QList<Rule> m_mediumRules;      // _MEDIUM_PATTERNS
    QStringList m_safePrefixes;     // _SAFE_PREFIXES
};

} // namespace cubeshell

// 跨线程 QueuedConnection 信号传递所需（声明必须紧随类型定义，先于任何
// moc 单元的隐式实例化）。
Q_DECLARE_METATYPE(cubeshell::AiCommand)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommand>)
