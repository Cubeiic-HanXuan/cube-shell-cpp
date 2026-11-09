// CommandConfirmDialog.cpp — 原生命令审批界面，批量与逐条审批复用同一命令卡片。
// 用途说明、原始 Shell 文本和安全依据分层展示；复制/批准保留原命令，不使用展示摘要。
#include "CommandConfirmDialog.h"
#include "widgets/ConversationWidgets.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QFontInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

namespace cubeshell {
namespace {
using namespace sessionui;

QColor blend(const QColor &base, const QColor &tint, qreal weight)
{
    return QColor::fromRgbF(base.redF() * (1 - weight) + tint.redF() * weight,
                           base.greenF() * (1 - weight) + tint.greenF() * weight,
                           base.blueF() * (1 - weight) + tint.blueF() * weight);
}

QLabel *label(const QString &text, const QColor &color, QWidget *parent,
              qreal scale = 1.0, bool bold = false)
{
    auto *value = new QLabel(text, parent);
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    value->setStyleSheet(QStringLiteral("background:transparent; color:%1; border:none;").arg(css(color)));
    QFont font = parent->font();
    // 按继承的界面字体缩放，不固定平台相关的磅值，确保高密度屏幕文字尺寸一致。
    font.setPixelSize(qMax(10, qRound(QFontInfo(font).pixelSize() * scale)));
    font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    value->setFont(font);
    return value;
}

QColor riskColor(RiskLevel level, const Colors &c)
{
    switch (level) {
    case RiskLevel::Safe: return c.success;
    case RiskLevel::Low: return c.accent;
    case RiskLevel::Medium: return c.canvas.lightness() < 128 ? QColor("#e6b66b") : QColor("#946000");
    case RiskLevel::High:
    case RiskLevel::Critical: return c.error;
    }
    return c.muted;
}

QString riskText(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Safe: return QStringLiteral("只读查询");
    case RiskLevel::Low: return QStringLiteral("低风险");
    case RiskLevel::Medium: return QStringLiteral("需人工确认");
    case RiskLevel::High: return QStringLiteral("高风险 · 二次确认");
    case RiskLevel::Critical: return QStringLiteral("禁止执行");
    }
    return QStringLiteral("待审核");
}

QLabel *riskBadge(RiskLevel level, const Colors &c, QWidget *parent)
{
    const QColor tone = riskColor(level, c);
    auto *badge = label(riskText(level), tone, parent, .9, true);
    badge->setObjectName(QStringLiteral("aiRiskBadge"));
    badge->setToolTip(QStringLiteral("风险等级：%1").arg(riskLevelLabel(level)));
    badge->setWordWrap(false);
    badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    badge->setStyleSheet(QStringLiteral("color:%1; background:%2; border:1px solid %3; border-radius:5px; padding:2px 6px;")
                            .arg(css(tone), css(blend(c.surface, tone, .10)), css(blend(c.surface, tone, .24))));
    return badge;
}

void configureDialog(QDialog *dialog, const QSize &preferred)
{
    const Colors c = colors(dialog->palette());
    dialog->setObjectName(QStringLiteral("aiCommandConfirm"));
    dialog->setModal(true);
    QFont base = dialog->font();
#ifdef CUBESHELL_PLATFORM_OHOS
    // OHOS 的磅值换算缺少屏幕密度缩放，使用像素字号并显式考虑 devicePixelRatio。
    base.setPixelSize(qMax(QFontInfo(base).pixelSize(), qRound(18 * dialog->devicePixelRatioF())));
#else
    base.setPixelSize(qMax(14, QFontInfo(base).pixelSize()));
#endif
    dialog->setFont(base);
    dialog->setMinimumSize(580, 420);
    // 按当前屏幕可用区域限制尺寸，避免小屏笔记本上对话框按钮被挤到工作区之外。
    const auto available = dialog->screen()->availableGeometry().size() - QSize(48, 80);
    dialog->setMinimumSize(dialog->minimumSize().boundedTo(available));
    dialog->resize(preferred.boundedTo(available));
    const QColor primary = c.accent.darker(c.canvas.lightness() < 128 ? 140 : 115);
    dialog->setStyleSheet(QStringLiteral(
        "QDialog#aiCommandConfirm { background:%1; color:%2; }"
        "QFrame#aiCommandCard { background:%3; border:1px solid %4; border-radius:10px; }"
        "QFrame#aiCommandCodePanel { background:%5; border:1px solid %4; border-radius:7px; }"
        "QWidget#aiSafetyDetails { background:%5; border-radius:6px; }"
        "QFrame#aiConfirmBox { background:%3; border:1px solid %6; border-radius:8px; }"
        "QPushButton { color:%2; background:%3; border:1px solid %4; border-radius:7px; padding:8px 16px; font-weight:500; }"
        "QPushButton:hover { background:%7; border-color:%8; }"
        "QPushButton:focus { border:2px solid %8; padding:7px 15px; }"
        "QPushButton[primary=\"true\"] { background:%9; border-color:%9; color:white; }"
        "QPushButton[primary=\"true\"]:hover { background:%8; border-color:%8; }"
        "QPushButton:disabled { color:%10; background:%7; border-color:%4; }"
        "QLineEdit { background:%5; color:%2; border:1px solid %4; border-radius:6px; padding:8px; }"
        "QLineEdit:focus { border-color:%8; }")
        .arg(css(c.canvas), css(c.text), css(c.surface), css(c.border), css(c.code),
             css(c.error), css(c.raised), css(c.accent), css(primary)).arg(css(c.muted)));
}

QPushButton *actionButton(const QString &text, QWidget *parent, bool primary = false)
{
    auto *button = new QPushButton(text, parent);
    button->setProperty("primary", primary);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoDefault(false); // 预览区按 Enter 不得因默认按钮机制意外批准批次
    button->setMinimumHeight(36);
    return button;
}

QFrame *separator(const Colors &c, QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:%1; border:none;").arg(css(c.border)));
    return line;
}

// 复制完整原命令，不含用途说明、风险标签等装饰文本；高亮和软换行只影响显示。
QToolButton *copyButton(const QString &source, const Colors &c, QWidget *parent)
{
    auto *copy = iconButton(Glyph::Copy, QStringLiteral("复制原始命令"), c, parent);
    copy->setObjectName(QStringLiteral("aiCopyCommand"));
    copy->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    copy->setText(QStringLiteral("复制"));
    copy->setFixedSize(copy->fontMetrics().horizontalAdvance(QStringLiteral("已复制")) + 36,
                       qMax(28, copy->fontMetrics().height() + 8));
    auto *reset = new QTimer(copy);
    reset->setSingleShot(true);
    QObject::connect(reset, &QTimer::timeout, copy, [copy, c] {
        copy->setText(QStringLiteral("复制")); copy->setIcon(icon(Glyph::Copy, c.muted));
    });
    QObject::connect(copy, &QToolButton::clicked, copy, [copy, source, c, reset] {
        QApplication::clipboard()->setText(source);
        copy->setText(QStringLiteral("已复制")); copy->setIcon(icon(Glyph::Check, c.success));
        reset->start(1600);
    });
    return copy;
}

class CommandCard : public QFrame {
public:
    CommandCard(int index, const AiCommand &command, const Colors &c, QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("aiCommandCard"));
        setFont(parent->window()->font());
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        QStringList evidence = command.safety.warnings;
        evidence.removeDuplicates();
        if (evidence.isEmpty()) evidence.append(command.safety.reason);

        auto *heading = new QHBoxLayout;
        heading->setSpacing(8);
        auto *number = label(QStringLiteral("%1").arg(index, 2, 10, QLatin1Char('0')), c.muted, this, .95, true);
        number->setObjectName(QStringLiteral("aiCommandNumber"));
        number->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        number->setFixedWidth(24);
        heading->addWidget(number);
        auto *purpose = new ElidedLabel(command.description.isEmpty() ? QStringLiteral("Shell 命令") : command.description, this);
        purpose->setObjectName(QStringLiteral("aiCommandPurpose"));
        purpose->setFont(font());
        purpose->setStyleSheet(QStringLiteral("color:%1; background:transparent; border:none;").arg(css(c.text)));
        purpose->setToolTip(QStringLiteral("<qt>%1</qt>").arg(purpose->text().toHtmlEscaped()));
        heading->addWidget(purpose, 1);
        heading->addWidget(riskBadge(command.safety.riskLevel, c, this));
        m_toggle = disclosureButton(QStringLiteral("依据"), c, this);
        m_toggle->setObjectName(QStringLiteral("aiToggleSafetyDetails"));
        m_toggle->setToolTip(QStringLiteral("展开完整执行说明与安全判定依据"));
        m_toggle->setMinimumSize(m_toggle->fontMetrics().horizontalAdvance(QStringLiteral("收起 (99)")) + 20, 26);
        heading->addWidget(m_toggle);
        auto *copy = copyButton(command.cmd, c, this);
        copy->setToolButtonStyle(Qt::ToolButtonIconOnly);
        copy->setFixedSize(26, 26);
        heading->addWidget(copy);
        layout->addLayout(heading);

        auto *codePanel = new QFrame(this);
        codePanel->setFont(font());
        codePanel->setObjectName(QStringLiteral("aiCommandCodePanel"));
        auto *codeLayout = new QVBoxLayout(codePanel);
        codeLayout->setContentsMargins(4, 0, 4, 0);
        codeLayout->setSpacing(0);
        auto *code = codeView(command.cmd, c, codePanel, QStringLiteral("bash"));
        QFont mono = code->font();
        mono.setPixelSize(qMax(14, QFontInfo(window()->font()).pixelSize()));
        code->setFont(mono);
        code->setTabChangesFocus(true);
        code->setObjectName(QStringLiteral("aiSafetyCommand"));
        code->setAccessibleName(QStringLiteral("命令 %1：原始 Shell 命令，只读预览").arg(index));
        code->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        code->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        code->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        code->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // QPlainTextDocumentLayout 的高度以显示行数计；随软换行增高，超过八行后内部滚动。
        const auto fitCode = [code] {
            const qreal lines = code->document()->documentLayout()->documentSize().height();
            // 保留文档/视口内边距，避免仅一行内容也因高度不足出现多余滚动条。
            const int height = qCeil(qBound<qreal>(1, lines, 8) * code->fontMetrics().lineSpacing()) + 20;
            if (code->height() != height) code->setFixedHeight(height);
        };
        QObject::connect(code->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                         code, [fitCode](const QSizeF &) { fitCode(); });
        QTimer::singleShot(0, code, fitCode);
        codeLayout->addWidget(code);
        layout->addWidget(codePanel);

        // 不另占一行重复风险提示，通过标题栏的“依据”展开详情；高风险/禁止项默认展开。

        m_details = new QWidget(this);
        m_details->setFont(font());
        m_details->setObjectName(QStringLiteral("aiSafetyDetails"));
        auto *detailsLayout = new QVBoxLayout(m_details);
        detailsLayout->setContentsMargins(10, 8, 10, 8);
        if (!command.description.isEmpty()) {
            auto *description = label(command.description, c.text, m_details, .92);
            description->setObjectName(QStringLiteral("aiFullCommandPurpose"));
            description->setTextInteractionFlags(Qt::TextSelectableByMouse);
            detailsLayout->addWidget(description);
        }
        auto *detailsCaption = label(QStringLiteral("判定依据 · %1 项").arg(evidence.size()), c.muted, m_details, .85, true);
        detailsLayout->addWidget(detailsCaption);
        QStringList lines;
        for (int i = 0; i < evidence.size(); ++i)
            lines.append(QStringLiteral("%1.  %2").arg(i + 1).arg(evidence.at(i)));
        auto *reason = label(lines.join(QLatin1Char('\n')), c.text, m_details, .92);
        reason->setObjectName(QStringLiteral("aiSafetyReason"));
        reason->setTextInteractionFlags(Qt::TextSelectableByMouse);
        reason->setToolTip(QStringLiteral("<qt>%1</qt>").arg(command.safety.reason.toHtmlEscaped()));
        detailsLayout->addWidget(reason);
        layout->addWidget(m_details);
        const int count = evidence.size();
        const auto updateDetails = [this, c, count](bool expanded) {
            m_details->setVisible(expanded);
            m_toggle->setText(QStringLiteral("%1 (%2)").arg(expanded ? QStringLiteral("收起") : QStringLiteral("依据")).arg(count));
            m_toggle->setAccessibleName(m_toggle->text());
            m_toggle->setIcon(icon(expanded ? Glyph::Down : Glyph::Chevron, c.muted));
        };
        QObject::connect(m_toggle, &QToolButton::toggled, this, updateDetails);
        const bool expanded = command.safety.riskLevel >= RiskLevel::High;
        m_toggle->setChecked(expanded);
        updateDetails(expanded); // 初始即为 false 时 setChecked 不发信号，仍需主动同步可见性
    }

    void setDetailsExpanded(bool expanded) { m_toggle->setChecked(expanded); }
    bool detailsExpanded() const { return m_toggle->isChecked(); }

private:
    QToolButton *m_toggle = nullptr;
    QWidget *m_details = nullptr;
};

// reviewOnly 仅减少需要构建的卡片，不修改原批次；审批后隐藏的查询仍按原顺序执行。
// 筛选共用 requiresConfirmation，禁止项不能被当成“不需确认”而从审核界面消失。
QScrollArea *commandArea(const QList<AiCommand> &commands, const Colors &c, QWidget *parent,
                         int firstIndex = 1, bool reviewOnly = false)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("aiCommandScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#aiCommandScroll { background:transparent; border:none; }"));
    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("aiCommandList"));
    content->setStyleSheet(QStringLiteral("QWidget#aiCommandList { background:transparent; }"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(8);
    int displayed = 0;
    for (const auto &command : commands) {
        if (reviewOnly && !command.safety.requiresConfirmation()) continue;
        layout->addWidget(new CommandCard(firstIndex + displayed++, command, c, content));
    }
    if (!displayed)
        layout->addWidget(label(QStringLiteral("没有需要人工确认的命令"), c.muted, content));
    layout->addStretch();
    scroll->setWidget(content);
    return scroll;
}

void addHeader(QVBoxLayout *layout, const QString &title, const QString &subtitle,
               RiskLevel risk, const Colors &c, QWidget *parent)
{
    auto *heading = new QHBoxLayout;
    heading->setSpacing(14);
    auto *mark = label(QStringLiteral(">_"), c.accent, parent, 1.3, true);
    mark->setObjectName(QStringLiteral("aiReviewIcon"));
    mark->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    mark->setFixedSize(42, 42);
    mark->setAlignment(Qt::AlignCenter);
    mark->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:10px;").arg(css(c.accent), css(c.accentSoft)));
    heading->addWidget(mark, 0, Qt::AlignTop);
    auto *text = new QVBoxLayout;
    text->setSpacing(5);
    auto *titleLabel = label(title, c.text, parent, 1.35, true);
    titleLabel->setObjectName(QStringLiteral("aiReviewTitle"));
    text->addWidget(titleLabel);
    text->addWidget(label(subtitle, c.muted, parent, .92));
    heading->addLayout(text, 1);
    heading->addWidget(riskBadge(risk, c, parent), 0, Qt::AlignTop);
    layout->addLayout(heading);
}

QWidget *confirmationBox(QLineEdit **input, const Colors &c, QWidget *parent)
{
    auto *box = new QFrame(parent);
    box->setObjectName(QStringLiteral("aiConfirmBox"));
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    layout->addWidget(label(QStringLiteral("高风险操作需要二次确认"), c.error, box, 1.0, true));
    layout->addWidget(label(QStringLiteral("请核对命令与判定依据，输入 CONFIRM 后才能执行。"), c.muted, box, .92));
    *input = new QLineEdit(box);
    (*input)->setObjectName(QStringLiteral("aiHighRiskConfirm"));
    (*input)->setAccessibleName(QStringLiteral("高风险二次确认：输入 CONFIRM"));
    (*input)->setPlaceholderText(QStringLiteral("输入 CONFIRM"));
    (*input)->setClearButtonEnabled(true);
    auto *entryRow = new QHBoxLayout;
    entryRow->setObjectName(QStringLiteral("aiConfirmInputRow"));
    entryRow->addWidget(*input, 1);
    layout->addLayout(entryRow);
    return box;
}
} // namespace

CommandConfirmDialog::CommandConfirmDialog(const QList<AiCommand> &commands, QWidget *parent)
    : QDialog(parent), m_commands(commands)
{
    // 以原始命令重新评估，不能因为调用方附带了较低的安全等级就省略审批。
    const CommandSafetyChecker checker;
    for (auto &command : m_commands) command.safety = checker.check(command.cmd);
    m_maxRisk = computeMaxRisk();
    setupUi();
}

void CommandConfirmDialog::setupUi()
{
    setWindowTitle(QStringLiteral("AI 命令确认"));
    configureDialog(this, QSize(820, 600));
    const Colors c = colors(palette());
    auto *main = new QVBoxLayout(this);
    main->setContentsMargins(18, 16, 18, 14);
    main->setSpacing(10);
    addHeader(main, QStringLiteral("审核执行计划"), QStringLiteral("仅展示需确认命令；隐藏的查询将在批准后按原顺序执行。"), m_maxRisk, c, this);

    int queries = 0, needsApproval = 0, blocked = 0;
    for (const auto &command : m_commands) {
        if (!command.safety.isAllowed) ++blocked;
        else if (command.safety.requiresConfirmation()) ++needsApproval;
        else ++queries;
    }
    QString summaryText = QStringLiteral("需确认 %1 条").arg(needsApproval);
    if (queries) summaryText += QStringLiteral("  ·  已隐藏只读/低风险 %1 条").arg(queries);
    if (blocked) summaryText += QStringLiteral("  ·  已阻止 %1 条").arg(blocked);
    auto *summary = label(summaryText, c.muted, this, .9);
    summary->setObjectName(QStringLiteral("aiSafetySummary"));
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(summary, 1);
    auto *expand = disclosureButton(QStringLiteral("展开全部依据"), c, this);
    expand->setObjectName(QStringLiteral("aiExpandAllSafety"));
    expand->setIcon(icon(Glyph::Expand, c.muted));
    expand->setMinimumSize(expand->fontMetrics().horizontalAdvance(QStringLiteral("展开全部依据")) + 28, 28);
    toolbar->addWidget(expand);
    main->addLayout(toolbar);
    auto *scroll = commandArea(m_commands, c, this, 1, true);
    main->addWidget(scroll, 1);
    QList<CommandCard *> cards;
    for (auto *child : scroll->widget()->children())
        if (auto *card = dynamic_cast<CommandCard *>(child)) cards.append(card);
    connect(expand, &QToolButton::toggled, this, [cards](bool expanded) {
        for (auto *card : cards) card->setDetailsExpanded(expanded);
    });
    const auto syncDisclosure = [cards, expand, c] {
        bool allExpanded = !cards.isEmpty();
        for (auto *card : cards) allExpanded &= card->detailsExpanded();
        // 汇总子卡片状态时不反向触发“展开全部”，避免一个卡片的变动改写其余卡片。
        const QSignalBlocker guard(expand);
        expand->setChecked(allExpanded);
        expand->setEnabled(!cards.isEmpty());
        expand->setText(allExpanded ? QStringLiteral("收起全部依据") : QStringLiteral("展开全部依据"));
        expand->setAccessibleName(expand->text());
        expand->setIcon(icon(allExpanded ? Glyph::Collapse : Glyph::Expand, c.muted));
    };
    for (auto *toggle : scroll->findChildren<QToolButton *>(QStringLiteral("aiToggleSafetyDetails")))
        connect(toggle, &QToolButton::toggled, this, syncDisclosure);
    syncDisclosure();

    m_confirmWidget = confirmationBox(&m_confirmInput, c, this);
    auto *confirm = actionButton(QStringLiteral("确认执行"), m_confirmWidget, true);
    confirm->setObjectName(QStringLiteral("aiConfirmHighRisk"));
    confirm->setEnabled(false);
    m_confirmWidget->findChild<QHBoxLayout *>(QStringLiteral("aiConfirmInputRow"))->addWidget(confirm);
    connect(m_confirmInput, &QLineEdit::textChanged, this, [confirm](const QString &text) {
        confirm->setEnabled(text.trimmed() == QStringLiteral("CONFIRM"));
    });
    connect(m_confirmInput, &QLineEdit::returnPressed, this, &CommandConfirmDialog::onConfirmInput);
    connect(confirm, &QPushButton::clicked, this, &CommandConfirmDialog::onConfirmInput);
    m_confirmWidget->hide();
    main->addWidget(m_confirmWidget);
    main->addWidget(separator(c, this));

    auto *footer = new QHBoxLayout;
    footer->setSpacing(10);
    auto *hint = label(blocked ? QStringLiteral("包含禁止执行的命令\n请取消并调整计划")
                              : QStringLiteral("尚未执行任何命令\n批准后执行本批 %1 条").arg(m_commands.size()),
                       blocked ? c.error : c.muted, this, .87);
    footer->addWidget(hint, 1);
    auto *cancel = actionButton(QStringLiteral("取消"), this);
    cancel->setObjectName(QStringLiteral("aiCancelCommands"));
    cancel->setFocus(Qt::OtherFocusReason);
    connect(cancel, &QPushButton::clicked, this, &CommandConfirmDialog::onCancel);
    footer->addWidget(cancel);
    auto *step = actionButton(QStringLiteral("逐条确认"), this);
    step->setObjectName(QStringLiteral("aiStepCommands"));
    step->setEnabled(!blocked && !m_commands.isEmpty());
    connect(step, &QPushButton::clicked, this, &CommandConfirmDialog::onStepMode);
    footer->addWidget(step);
    m_btnExecute = actionButton(m_maxRisk == RiskLevel::High ? QStringLiteral("继续确认")
                                                            : QStringLiteral("批准并执行"), this, true);
    m_btnExecute->setObjectName(QStringLiteral("aiExecuteAll"));
    m_btnExecute->setEnabled(!blocked && !m_commands.isEmpty());
    connect(m_btnExecute, &QPushButton::clicked, this, &CommandConfirmDialog::onExecuteAll);
    footer->addWidget(m_btnExecute);
    main->addLayout(footer);
}

void CommandConfirmDialog::onExecuteAll()
{
    if (m_maxRisk == RiskLevel::Critical || m_commands.isEmpty()) return;
    if (m_maxRisk == RiskLevel::High) showHighRiskConfirm();
    else { emit commandsApproved(m_commands); accept(); }
}

void CommandConfirmDialog::onStepMode()
{
    if (m_maxRisk == RiskLevel::Critical || m_commands.isEmpty()) return;
    emit commandStep(m_commands);
    accept();
}

void CommandConfirmDialog::onCancel()
{
    emit commandsCancelled();
    reject();
}

void CommandConfirmDialog::showHighRiskConfirm()
{
    m_confirmWidget->show();
    m_confirmInput->setFocus();
    m_btnExecute->setEnabled(false);
}

// 除校验输入内容外还检查当前风险和确认框可见性，隐藏确认框时的旧信号不能批准执行。
void CommandConfirmDialog::onConfirmInput()
{
    if (m_maxRisk != RiskLevel::High || m_confirmWidget->isHidden()) return;
    if (m_confirmInput->text().trimmed() == QStringLiteral("CONFIRM")) {
        emit commandsApproved(m_commands);
        accept();
    } else {
        m_confirmInput->clear();
        m_confirmInput->setPlaceholderText(QStringLiteral("输入不匹配，请输入 CONFIRM"));
        m_confirmInput->setFocus();
    }
}

RiskLevel CommandConfirmDialog::computeMaxRisk() const
{
    RiskLevel risk = RiskLevel::Safe;
    for (const auto &command : m_commands) risk = qMax(risk, command.safety.riskLevel);
    return risk;
}

SingleCommandConfirmDialog::SingleCommandConfirmDialog(const AiCommand &command, int index, int total, QWidget *parent)
    : QDialog(parent), m_cmdInfo(command)
{
    m_cmdInfo.safety = CommandSafetyChecker().check(m_cmdInfo.cmd);
    setupUi(index, total);
}

void SingleCommandConfirmDialog::setupUi(int index, int total)
{
    setWindowTitle(QStringLiteral("逐条确认 (%1/%2)").arg(index).arg(total));
    configureDialog(this, QSize(760, 460));
    const Colors c = colors(palette());
    const RiskLevel level = m_cmdInfo.safety.riskLevel;
    auto *main = new QVBoxLayout(this);
    main->setContentsMargins(18, 16, 18, 14);
    main->setSpacing(10);
    addHeader(main, QStringLiteral("审核当前命令"), QStringLiteral("第 %1 / %2 条 · 批准项将在审核完成后按原顺序执行。")
                  .arg(index).arg(total), level, c, this);
    main->addWidget(separator(c, this));
    main->addWidget(commandArea({m_cmdInfo}, c, this, index), 1);
    QLineEdit *confirm = nullptr;
    if (level == RiskLevel::High) main->addWidget(confirmationBox(&confirm, c, this));
    main->addWidget(separator(c, this));
    auto *footer = new QHBoxLayout;
    footer->setSpacing(10);
    auto *abort = actionButton(QStringLiteral("终止全部"), this);
    abort->setObjectName(QStringLiteral("aiAbortCommands"));
    connect(abort, &QPushButton::clicked, this, [this] { m_abortAll = true; reject(); });
    footer->addWidget(abort);
    footer->addStretch();
    auto *skip = actionButton(QStringLiteral("跳过此条"), this);
    skip->setObjectName(QStringLiteral("aiSkipCommand"));
    connect(skip, &QPushButton::clicked, this, &QDialog::reject);
    footer->addWidget(skip);
    auto *execute = actionButton(QStringLiteral("批准此条"), this, true);
    execute->setObjectName(QStringLiteral("aiExecuteSingle"));
    execute->setEnabled(m_cmdInfo.safety.isAllowed && level != RiskLevel::High);
    if (confirm) {
        connect(confirm, &QLineEdit::textChanged, this, [execute](const QString &text) {
            execute->setEnabled(text.trimmed() == QStringLiteral("CONFIRM"));
        });
    }
    connect(execute, &QPushButton::clicked, this, [this, confirm] {
        if (!m_cmdInfo.safety.isAllowed) return;
        if (confirm && confirm->text().trimmed() != QStringLiteral("CONFIRM")) return;
        accept();
    });
    footer->addWidget(execute);
    main->addLayout(footer);
}

} // namespace cubeshell
