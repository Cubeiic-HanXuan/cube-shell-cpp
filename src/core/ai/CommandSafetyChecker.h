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

    // Shared by automatic execution, review-list filtering and step approval.
    bool requiresConfirmation() const { return !isAllowed || riskLevel >= RiskLevel::Medium; }
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
    quint64 stepId = 0;          // 本地执行身份，不发送给模型
    QString toolCallId;
};

// Stateless, quote-aware assessment of a deliberately limited shell grammar.
// Pipelines/lists take the highest component risk; unknown syntax/commands need
// confirmation (Medium), never automatic Low. Not a sandbox: remote binaries,
// aliases and configuration are assumed trusted. Safe means an understood query,
// not a guarantee against sensitive output or resource consumption.
class CommandSafetyChecker {
public:
    SafetyCheckResult check(const QString &command) const;
    QList<SafetyCheckResult> checkBatch(const QStringList &commands) const;
};

} // namespace cubeshell

// 跨线程 QueuedConnection 信号传递所需（声明必须紧随类型定义，先于任何
// moc 单元的隐式实例化）。
Q_DECLARE_METATYPE(cubeshell::AiCommand)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommand>)
