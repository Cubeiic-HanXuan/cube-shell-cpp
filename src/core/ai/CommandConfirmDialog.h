#pragma once

// CommandConfirmDialog.h — AI 候选 Shell 命令执行前的批量/逐条确认界面。
// 对话框只收集审批结果，不直接执行；构造时重新计算安全等级，不信任传入的旧判定。
//
// 对应Python: core/ai/confirm_dialog.py
//   - CommandConfirmDialog       批量确认（全部执行/逐条确认/取消）
//   - SingleCommandConfirmDialog 逐条审批模式中的单步弹窗
//
// 中文风险标签复用 riskLevelLabel()，卡片颜色适配当前主题。
// RiskLevel 枚举值按风险递增（对应 Python _RISK_ORDER），可直接比较取最大值。
// High 必须输入 CONFIRM；Critical 不能通过二次确认绕过禁止规则。

#include "CommandSafetyChecker.h"

#include <QDialog>
#include <QList>

class QLineEdit;
class QPushButton;
class QWidget;

namespace cubeshell {

// 命令确认对话框 - 用户确认 AI 建议的命令。
// 对应Python: confirm_dialog.py::CommandConfirmDialog
class CommandConfirmDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommandConfirmDialog(const QList<AiCommand> &commands,
                                  QWidget *parent = nullptr);

    RiskLevel maxRisk() const { return m_maxRisk; }

signals:
    // 对应Python: commands_approved / command_step / commands_cancelled
    // 批量界面可隐藏无需确认的查询，但批准/逐条模式仍返回完整有序批次，不丢失隐藏项。
    void commandsApproved(const QList<cubeshell::AiCommand> &commands);
    void commandStep(const QList<cubeshell::AiCommand> &commands);
    void commandsCancelled();

private slots:
    void onExecuteAll();      // 对应Python: _on_execute_all
    void onStepMode();        // 对应Python: _on_step_mode
    void onCancel();          // 对应Python: _on_cancel
    void onConfirmInput();    // 对应Python: _on_confirm_input

private:
    void setupUi();
    void showHighRiskConfirm();
    RiskLevel computeMaxRisk() const;

    QList<AiCommand> m_commands; // 原始批次及本次复核的安全结果；筛选仅影响展示
    RiskLevel m_maxRisk = RiskLevel::Safe;

    QWidget *m_confirmWidget = nullptr;
    QLineEdit *m_confirmInput = nullptr;
    QPushButton *m_btnExecute = nullptr;
};

// 单条命令确认对话框 - 逐条审批模式中的单步弹窗。
// Accepted 表示批准，Rejected 表示跳过或终止全部；调用方通过 abortAll 区分后两者。
// 对应Python: confirm_dialog.py::SingleCommandConfirmDialog
class SingleCommandConfirmDialog : public QDialog {
    Q_OBJECT
public:
    SingleCommandConfirmDialog(const AiCommand &cmdInfo, int index, int total,
                               QWidget *parent = nullptr);

    // 用户是否点击了「终止全部」。对应Python: self.abort_all
    bool abortAll() const { return m_abortAll; }

private:
    void setupUi(int index, int total);

    AiCommand m_cmdInfo;
    bool m_abortAll = false;
};

} // namespace cubeshell
