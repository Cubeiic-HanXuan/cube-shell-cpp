// CommandConfirmDialog.cpp — see CommandConfirmDialog.h.
//
// 对应Python: core/ai/confirm_dialog.py

#include "CommandConfirmDialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

// 单条命令的自定义列表项 Widget。
// 对应Python: confirm_dialog.py::_CommandItemWidget
class CommandItemWidget : public QWidget {
public:
    CommandItemWidget(int index, const AiCommand &cmdInfo,
                      QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);

        // 序号
        auto *indexLabel = new QLabel(QStringLiteral("%1.").arg(index), this);
        indexLabel->setFixedWidth(24);
        layout->addWidget(indexLabel);

        // 命令文本（等宽字体）
        auto *cmdLabel = new QLabel(cmdInfo.cmd, this);
        QFont monoFont(QStringLiteral("Courier New"), 11);
        monoFont.setStyleHint(QFont::Monospace);
        cmdLabel->setFont(monoFont);
        cmdLabel->setWordWrap(true);
        layout->addWidget(cmdLabel, 1);

        // 风险等级标签
        const RiskLevel level = cmdInfo.safety.riskLevel;
        auto *riskLabel = new QLabel(
            QStringLiteral("[%1]").arg(riskLevelLabel(level)), this);
        riskLabel->setStyleSheet(
            QStringLiteral("color: %1; font-weight: bold; padding: 0 4px;")
                .arg(riskLevelColor(level)));
        layout->addWidget(riskLabel);
    }

    // 提供合理的尺寸提示。对应Python: _CommandItemWidget.sizeHint
    QSize sizeHint() const override
    {
        QSize hint = QWidget::sizeHint();
        hint.setHeight(qMax(hint.height(), 36));
        return hint;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// CommandConfirmDialog
// ---------------------------------------------------------------------------

CommandConfirmDialog::CommandConfirmDialog(const QList<AiCommand> &commands,
                                           QWidget *parent)
    : QDialog(parent)
    , m_commands(commands)
{
    m_maxRisk = computeMaxRisk();
    setupUi();
}

// 对应Python: CommandConfirmDialog._setup_ui
void CommandConfirmDialog::setupUi()
{
    setWindowTitle(QStringLiteral("AI 命令确认"));
    setMinimumWidth(500);
    setModal(true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    // 顶部提示
    auto *header = new QLabel(QStringLiteral("⚠️  AI 建议执行以下命令:"), this);
    QFont headerFont;
    headerFont.setPointSize(13);
    headerFont.setBold(true);
    header->setFont(headerFont);
    mainLayout->addWidget(header);

    // 命令列表
    m_listWidget = new QListWidget(this);
    m_listWidget->setAlternatingRowColors(true);
    populateCommands();
    mainLayout->addWidget(m_listWidget);

    // 整体风险等级
    auto *riskInfo = new QLabel(
        QStringLiteral(
            "风险等级: <span style=\"color:%1; font-weight:bold;\">●%2</span>"
            "  (%3)")
            .arg(riskLevelColor(m_maxRisk), riskLevelLabel(m_maxRisk),
                 riskReason()),
        this);
    riskInfo->setTextFormat(Qt::RichText);
    mainLayout->addWidget(riskInfo);

    // 描述信息（如有）— 取前 3 条 description 用 "; " 连接
    QStringList descriptions;
    for (const AiCommand &cmd : m_commands) {
        if (!cmd.description.isEmpty())
            descriptions.append(cmd.description);
    }
    if (!descriptions.isEmpty()) {
        const QString descText =
            QStringList(descriptions.mid(0, 3)).join(QStringLiteral("; "));
        auto *descLabel =
            new QLabel(QStringLiteral("说明: %1").arg(descText), this);
        descLabel->setStyleSheet(QStringLiteral("color: gray;"));
        descLabel->setWordWrap(true);
        mainLayout->addWidget(descLabel);
    }

    // 二次确认区域（初始隐藏）
    m_confirmWidget = new QWidget(this);
    auto *confirmLayout = new QHBoxLayout(m_confirmWidget);
    confirmLayout->setContentsMargins(0, 8, 0, 0);
    auto *confirmHint = new QLabel(
        QStringLiteral("⚠️ 高风险操作！请输入 \"CONFIRM\" 以确认执行:"),
        m_confirmWidget);
    confirmHint->setStyleSheet(
        QStringLiteral("color: #F44336; font-weight: bold;"));
    confirmLayout->addWidget(confirmHint);
    m_confirmInput = new QLineEdit(m_confirmWidget);
    m_confirmInput->setPlaceholderText(QStringLiteral("输入 CONFIRM"));
    m_confirmInput->setMaximumWidth(140);
    connect(m_confirmInput, &QLineEdit::returnPressed,
            this, &CommandConfirmDialog::onConfirmInput);
    confirmLayout->addWidget(m_confirmInput);
    auto *confirmBtn = new QPushButton(QStringLiteral("确认"), m_confirmWidget);
    connect(confirmBtn, &QPushButton::clicked,
            this, &CommandConfirmDialog::onConfirmInput);
    confirmLayout->addWidget(confirmBtn);
    m_confirmWidget->setVisible(false);
    mainLayout->addWidget(m_confirmWidget);

    // 底部按钮
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(12);

    m_btnExecute = new QPushButton(QStringLiteral("全部执行"), this);
    m_btnExecute->setMinimumHeight(32);
    m_btnExecute->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #4CAF50; color: white; "
        "border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover { background-color: #388E3C; }"));
    connect(m_btnExecute, &QPushButton::clicked,
            this, &CommandConfirmDialog::onExecuteAll);
    btnLayout->addWidget(m_btnExecute);

    auto *btnStep = new QPushButton(QStringLiteral("逐条确认"), this);
    btnStep->setMinimumHeight(32);
    btnStep->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #2196F3; color: white; "
        "border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover { background-color: #1976D2; }"));
    connect(btnStep, &QPushButton::clicked,
            this, &CommandConfirmDialog::onStepMode);
    btnLayout->addWidget(btnStep);

    auto *btnCancel = new QPushButton(QStringLiteral("取消"), this);
    btnCancel->setMinimumHeight(32);
    btnCancel->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #999; border-radius: 4px; "
        "padding: 4px 16px; }"
        "QPushButton:hover { background-color: #eee; }"));
    connect(btnCancel, &QPushButton::clicked,
            this, &CommandConfirmDialog::onCancel);
    btnLayout->addWidget(btnCancel);

    mainLayout->addLayout(btnLayout);
}

// 对应Python: CommandConfirmDialog._populate_commands
void CommandConfirmDialog::populateCommands()
{
    for (int i = 0; i < m_commands.size(); ++i) {
        auto *item = new QListWidgetItem(m_listWidget);
        auto *widget = new CommandItemWidget(i + 1, m_commands.at(i));
        item->setSizeHint(widget->sizeHint());
        m_listWidget->addItem(item);
        m_listWidget->setItemWidget(item, widget);
    }
}

// 对应Python: CommandConfirmDialog._on_execute_all
void CommandConfirmDialog::onExecuteAll()
{
    if (m_maxRisk == RiskLevel::High) {
        showHighRiskConfirm();
    } else {
        emit commandsApproved(m_commands);
        accept();
    }
}

// 对应Python: CommandConfirmDialog._on_step_mode
void CommandConfirmDialog::onStepMode()
{
    emit commandStep(m_commands);
    accept();
}

// 对应Python: CommandConfirmDialog._on_cancel
void CommandConfirmDialog::onCancel()
{
    emit commandsCancelled();
    reject();
}

// 对应Python: CommandConfirmDialog._show_high_risk_confirm
void CommandConfirmDialog::showHighRiskConfirm()
{
    m_confirmWidget->setVisible(true);
    m_confirmInput->setFocus();
    m_btnExecute->setEnabled(false);
}

// 对应Python: CommandConfirmDialog._on_confirm_input
void CommandConfirmDialog::onConfirmInput()
{
    if (m_confirmInput->text().trimmed() == QStringLiteral("CONFIRM")) {
        emit commandsApproved(m_commands);
        accept();
    } else {
        m_confirmInput->setStyleSheet(
            QStringLiteral("border: 1px solid #F44336;"));
        m_confirmInput->setPlaceholderText(QStringLiteral("请输入 CONFIRM"));
        m_confirmInput->clear();
    }
}

// 对应Python: CommandConfirmDialog._compute_max_risk
// RiskLevel enum 底层值即 _RISK_ORDER 排序权重，直接取最大。
RiskLevel CommandConfirmDialog::computeMaxRisk() const
{
    RiskLevel maxRisk = RiskLevel::Safe;
    for (const AiCommand &cmd : m_commands) {
        if (static_cast<int>(cmd.safety.riskLevel)
            > static_cast<int>(maxRisk))
            maxRisk = cmd.safety.riskLevel;
    }
    return maxRisk;
}

// 对应Python: CommandConfirmDialog._get_risk_reason
QString CommandConfirmDialog::riskReason() const
{
    for (const AiCommand &cmd : m_commands) {
        if (cmd.safety.riskLevel == m_maxRisk) {
            if (!cmd.safety.reason.isEmpty())
                return cmd.safety.reason;
            if (!cmd.description.isEmpty())
                return cmd.description;
        }
    }
    return QStringLiteral("AI 建议操作");
}

// ---------------------------------------------------------------------------
// SingleCommandConfirmDialog
// ---------------------------------------------------------------------------

SingleCommandConfirmDialog::SingleCommandConfirmDialog(const AiCommand &cmdInfo,
                                                       int index, int total,
                                                       QWidget *parent)
    : QDialog(parent)
    , m_cmdInfo(cmdInfo)
{
    setupUi(index, total);
}

// 对应Python: SingleCommandConfirmDialog._setup_ui
void SingleCommandConfirmDialog::setupUi(int index, int total)
{
    setWindowTitle(QStringLiteral("逐条确认 (%1/%2)").arg(index).arg(total));
    setMinimumWidth(450);
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 16, 16, 16);

    // 进度提示
    auto *progressLabel = new QLabel(
        QStringLiteral("命令 %1 / %2").arg(index).arg(total), this);
    progressLabel->setStyleSheet(
        QStringLiteral("color: gray; font-size: 12px;"));
    layout->addWidget(progressLabel);

    // 命令内容（等宽、可选中、带边框）
    auto *cmdLabel = new QLabel(m_cmdInfo.cmd, this);
    QFont monoFont(QStringLiteral("Courier New"), 12);
    monoFont.setStyleHint(QFont::Monospace);
    cmdLabel->setFont(monoFont);
    cmdLabel->setWordWrap(true);
    cmdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cmdLabel->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid); "
        "border-radius: 4px; padding: 8px;"));
    layout->addWidget(cmdLabel);

    // 描述
    if (!m_cmdInfo.description.isEmpty()) {
        auto *descLabel = new QLabel(
            QStringLiteral("说明: %1").arg(m_cmdInfo.description), this);
        descLabel->setStyleSheet(QStringLiteral("color: gray;"));
        descLabel->setWordWrap(true);
        layout->addWidget(descLabel);
    }

    // 风险等级
    const RiskLevel level = m_cmdInfo.safety.riskLevel;
    auto *riskLabel = new QLabel(
        QStringLiteral(
            "风险: <span style=\"color:%1; font-weight:bold;\">●%2</span>")
            .arg(riskLevelColor(level), riskLevelLabel(level)),
        this);
    riskLabel->setTextFormat(Qt::RichText);
    layout->addWidget(riskLabel);

    // 安全检查警告（前 3 条）
    const QStringList &warnings = m_cmdInfo.safety.warnings;
    for (int i = 0; i < warnings.size() && i < 3; ++i) {
        auto *warnLabel = new QLabel(
            QStringLiteral("⚠️ %1").arg(warnings.at(i)), this);
        warnLabel->setStyleSheet(
            QStringLiteral("color: #FF9800; font-size: 11px;"));
        warnLabel->setWordWrap(true);
        layout->addWidget(warnLabel);
    }

    // 按钮区域
    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    auto *btnExecute = new QPushButton(QStringLiteral("✓ 执行此条"), this);
    btnExecute->setMinimumHeight(32);
    btnExecute->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #4CAF50; color: white; "
        "border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover { background-color: #388E3C; }"));
    connect(btnExecute, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(btnExecute);

    auto *btnSkip = new QPushButton(QStringLiteral("⏭ 跳过"), this);
    btnSkip->setMinimumHeight(32);
    btnSkip->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #FF9800; color: white; "
        "border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover { background-color: #F57C00; }"));
    connect(btnSkip, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnSkip);

    auto *btnAbort = new QPushButton(QStringLiteral("✕ 终止全部"), this);
    btnAbort->setMinimumHeight(32);
    btnAbort->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #F44336; color: #F44336; "
        "border-radius: 4px; padding: 4px 16px; }"
        "QPushButton:hover { background-color: #FFEBEE; }"));
    // 对应Python: _on_abort — abort_all = True 后 reject
    connect(btnAbort, &QPushButton::clicked, this, [this]() {
        m_abortAll = true;
        reject();
    });
    btnLayout->addWidget(btnAbort);

    layout->addLayout(btnLayout);
}

} // namespace cubeshell
