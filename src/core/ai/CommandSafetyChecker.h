#pragma once

// CommandSafetyChecker.h — AI 候选 Shell 命令的保守静态风险评估。
//
// 对应Python: core/ai/safety.py（RiskLevel / SafetyCheckResult / CommandSafetyChecker）
// 同时定义 AiCommand，作为 SshAiAgent、确认对话框和执行线程之间的数据契约。
// 对应Python: ssh_agent.py 中的命令 dict 加 safety 检查结果。

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace cubeshell {

// 等级按风险递增排列；复合命令和批次可直接比较并取最大值。
// 对应Python: safety.py::RiskLevel
// 是否自动执行还受上层审批流程控制，这里仅定义分类与确认门槛。
enum class RiskLevel {
    Safe,       // 已理解的只读查询，不要求人工确认
    Low,        // 低风险，不要求人工确认
    Medium,     // 有副作用或静态分析不确定，需要用户确认
    High,       // 高风险，确认界面要求二次确认
    Critical,   // 禁止执行，用户确认也不能放行
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
    bool isAllowed = true;      // 是否允许执行，与“是否需要确认”分开判断
    QString reason;             // 检查结论摘要
    QStringList warnings;       // 去重后的具体判定依据，用于确认界面的详情区

    // 自动执行判断、审批列表筛选和逐条确认共用同一门槛；禁止项也必须在审核列表中可见。
    bool requiresConfirmation() const { return !isAllowed || riskLevel >= RiskLevel::Medium; }
};

// 一条 AI 候选命令及其风险结果。显示时可折行/高亮，但执行与复制始终使用原始 cmd。
// 对应Python: ssh_agent.py 的命令 dict（cmd/description/allow_failure/interactive）+ safety 字段
struct AiCommand {
    QString cmd;
    QString description;
    bool allowFailure = false; // 已知的非零退出后能否继续批次，不允许忽略“结果未知”
    bool interactive = false; // 保留工具协议中的交互提示，实际执行后端由控制器选择
    SafetyCheckResult safety;
    quint64 stepId = 0;          // 本地执行身份，不发送给模型；审批与结果回写以此关联
    QString toolCallId; // 同一工具调用可以展开为多条命令，回传时按此分组
};

// 无状态分析器，只理解有限 Shell 语法并区分引号、展开与操作符。
// 管道/命令列表取各组成部分的最高风险；未知语法/命令至少为 Medium，不自动降为 Low。
// 这不是沙箱：假设远端程序、别名和配置可信；Safe 不保证输出无敏感数据或资源消耗为零。
class CommandSafetyChecker {
public:
    SafetyCheckResult check(const QString &command) const;
    QList<SafetyCheckResult> checkBatch(const QStringList &commands) const; // 保持输入顺序逐条评估
};

} // namespace cubeshell

// 跨线程 QueuedConnection 信号传递所需（声明必须紧随类型定义，先于任何
// moc 单元的隐式实例化）。
Q_DECLARE_METATYPE(cubeshell::AiCommand)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommand>)
