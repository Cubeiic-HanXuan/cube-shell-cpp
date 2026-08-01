#pragma once

// CommandConfirmDialog.h — user confirmation dialogs shown before running
// AI-suggested shell commands.
//
// 对应Python: core/ai/confirm_dialog.py
//   - CommandConfirmDialog       批量确认（全部执行/逐条确认/取消）
//   - SingleCommandConfirmDialog 逐条审批模式中的单步弹窗
//
// 风险颜色/中文标签复用 CommandSafetyChecker.h 的
// riskLevelColor()/riskLevelLabel()；RiskLevel enum 值即排序权重
// （对应 Python _RISK_ORDER），可直接比较取最大值。

#include "CommandSafetyChecker.h"

#include <QDialog>
#include <QList>

class QLabel;
class QLineEdit;
class QListWidget;
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
    void populateCommands();          // 对应Python: _populate_commands
    void showHighRiskConfirm();       // 对应Python: _show_high_risk_confirm
    RiskLevel computeMaxRisk() const; // 对应Python: _compute_max_risk
    QString riskReason() const;       // 对应Python: _get_risk_reason

    QList<AiCommand> m_commands;
    RiskLevel m_maxRisk = RiskLevel::Safe;

    QListWidget *m_listWidget = nullptr;
    QWidget *m_confirmWidget = nullptr;
    QLineEdit *m_confirmInput = nullptr;
    QPushButton *m_btnExecute = nullptr;
};

// 单条命令确认对话框 - 逐条审批模式中的单步弹窗。
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
