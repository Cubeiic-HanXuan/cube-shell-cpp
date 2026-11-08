#include "AiConversationView.h"
#include "MarkdownView.h"
#include "ChatHistory.h"
#include "widgets/ConversationWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace cubeshell {
namespace {
using namespace sessionui;

QLabel *plainLabel(const QString &text, QWidget *parent, bool bold = false)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    auto font = label->font(); font.setBold(bold); label->setFont(font);
    return label;
}

void copyFeedback(QToolButton *button, const QString &text)
{
    QApplication::clipboard()->setText(text);
    button->setIcon(icon(Glyph::Check, colors(button->palette()).success));
    const QString tooltip = button->accessibleName();
    button->setToolTip(QObject::tr("已复制"));
    QTimer::singleShot(1600, button, [button, tooltip] {
        button->setIcon(icon(Glyph::Copy, colors(button->palette()).muted));
        button->setToolTip(tooltip);
    });
}

class OutputBlock : public QWidget {
public:
    OutputBlock(const QString &title, QWidget *parent, const QString &language = QStringLiteral("text")) : QWidget(parent)
    {
        const auto c = colors(palette());
        setObjectName(QStringLiteral("aiOutputBlock"));
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral("QWidget#aiOutputBlock { background:%1; border:1px solid %2; border-radius:8px; }")
                          .arg(css(c.code), css(c.border)));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 4, 10, 8); layout->setSpacing(2);
        auto *bar = new QHBoxLayout;
        m_title = new ElidedLabel(title, this); m_heading = title;
        bar->addWidget(m_title, 1);
        auto *copy = iconButton(Glyph::Copy, tr("复制完整内容"), c, this);
        copy->setObjectName(QStringLiteral("aiCopyOutput"));
        connect(copy, &QToolButton::clicked, this, [this, copy] { copyFeedback(copy, m_text); });
        bar->addWidget(copy); layout->addLayout(bar);
        m_editor = codeView({}, c, this, language); m_editor->setObjectName(QStringLiteral("aiOutputText"));
        layout->addWidget(m_editor);
    }
    void setText(const QString &text)
    {
        if (m_text == text) return;
        const int position = m_editor->verticalScrollBar()->value();
        if (text.startsWith(m_text)) {
            QTextCursor cursor(m_editor->document()); cursor.movePosition(QTextCursor::End);
            cursor.insertText(text.mid(m_text.size()));
        } else m_editor->setPlainText(text);
        m_text = text;
        m_editor->verticalScrollBar()->setValue(position);
        const int lines = text.isEmpty() ? 0 : text.count(QLatin1Char('\n')) + 1;
        m_editor->setFixedHeight(qBound(1, lines, 18) * m_editor->fontMetrics().lineSpacing() + 26);
        m_title->setText(tr("%1 · %2 行").arg(m_heading).arg(lines));
    }
private:
    QString m_text, m_heading;
    ElidedLabel *m_title;
    QPlainTextEdit *m_editor;
};

class EntryWidget : public QWidget {
public:
    EntryWidget(AiConversationModel *model, quint64 turnId, const AiConversationEntry &entry, QWidget *parent)
        : QWidget(parent), m_model(model), m_turnId(turnId), m_id(entry.id), m_kind(entry.kind)
    {
        setObjectName(QStringLiteral("aiEntry_%1").arg(entry.id));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 5, 0, 5); layout->setSpacing(5);
        const auto c = colors(palette());
        if (m_kind == AiConversationEntry::Command || m_kind == AiConversationEntry::Activity) {
            auto *bar = new QHBoxLayout;
            bar->setSpacing(5);
            const bool command = m_kind == AiConversationEntry::Command;
            m_toggle = disclosureButton(command ? QStringLiteral("SSH") : aiActivityLabel(entry.activity.kind), c, this);
            m_toggle->setObjectName(QStringLiteral("ai%1Toggle_%2").arg(command ? "Command" : "Activity").arg(m_id));
            m_toggle->setToolTip(command ? tr("展开命令及完整输出") : tr("展开调用参数、返回内容和关联信息"));
            bar->addWidget(m_toggle);
            m_summary = new ElidedLabel(command ? entry.command.cmd.simplified() : activitySummary(entry.activity), this);
            auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            if (font().pixelSize() > 0) mono.setPixelSize(font().pixelSize());
            else mono.setPointSizeF(font().pointSizeF());
            if (command) m_summary->setFont(mono);
            bar->addWidget(m_summary, 1);
            m_status = plainLabel({}, this); bar->addWidget(m_status);
            m_duration = plainLabel({}, this); bar->addWidget(m_duration);
            auto *copy = iconButton(Glyph::Copy, command ? tr("复制命令") : tr("复制调用详情"), c, this);
            copy->setObjectName(QStringLiteral("aiCopyEntry_%1").arg(m_id));
            connect(copy, &QToolButton::clicked, this, [this, copy] { copyFeedback(copy, m_copyText); });
            bar->addWidget(copy);
            layout->addLayout(bar);
            m_context = new ElidedLabel({}, this);
            m_context->setObjectName(QStringLiteral("aiActivityContext_%1").arg(m_id));
            m_context->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
            layout->addWidget(m_context);
            connect(m_toggle, &QToolButton::clicked, this, [this](bool expanded) {
                m_model->setEntryExpanded(m_turnId, m_id, expanded);
            });
        } else {
            auto *messageBar = new QHBoxLayout;
            messageBar->setContentsMargins(0, 0, 0, 0);
            auto *label = plainLabel(m_kind == AiConversationEntry::Notice ? tr("系统提示") : tr("助手说明"), this);
            label->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
            messageBar->addWidget(label); messageBar->addStretch();
            auto *copy = iconButton(Glyph::Copy, tr("复制消息"), c, this);
            connect(copy, &QToolButton::clicked, this, [this, copy] { copyFeedback(copy, m_copyText); });
            messageBar->addWidget(copy);
            m_messageHeader = new QWidget(this);
            // The header is owned separately so a reasoning-only entry does not
            // leave an empty message label above its disclosure.
            m_messageHeader->setLayout(messageBar); layout->addWidget(m_messageHeader);
            m_body = new MarkdownView(this);
            m_body->setObjectName(QStringLiteral("aiMessage_%1").arg(m_id));
            layout->addWidget(m_body);
            m_reasonToggle = disclosureButton(tr("思考摘要"), c, this);
            m_reasonToggle->setObjectName(QStringLiteral("aiReasoningToggle_%1").arg(m_id));
            layout->addWidget(m_reasonToggle, 0, Qt::AlignLeft);
            m_reason = richText({}, c, this);
            m_reason->setObjectName(QStringLiteral("aiReasoning_%1").arg(m_id));
            layout->addWidget(m_reason);
            connect(m_reasonToggle, &QToolButton::clicked, this, [this](bool expanded) {
                m_model->setEntryExpanded(m_turnId, m_id, expanded);
            });
        }
    }
    void sync(const AiConversationEntry &entry, bool expanded)
    {
        if (entry.revision == m_revision && expanded == m_expanded) return;
        m_revision = entry.revision; m_expanded = expanded;
        const auto c = colors(palette());
        m_dot = c.muted;
        layout()->setContentsMargins(entry.parentEntryId ? 38 : 24, 5, 0, 5);
        if (m_kind == AiConversationEntry::Command || m_kind == AiConversationEntry::Activity) {
            const bool command = m_kind == AiConversationEntry::Command;
            const auto &activity = entry.activity;
            const QString identity = command ? entry.command.toolCallId : activityIdentity(activity);
            m_copyText = command ? entry.command.cmd : identity + QStringLiteral("\n\n") + activity.input
                + QStringLiteral("\n\n") + activity.output;
            const QString context = command ? entry.command.description
                : activity.summary.isEmpty() ? identity : activity.name + QStringLiteral(" · ") + identity;
            m_context->setText(context); m_context->setToolTip(context); m_context->setVisible(!expanded && !context.isEmpty());
            m_summary->setToolTip(command ? entry.command.cmd : activitySummary(activity));
            m_toggle->setChecked(expanded);
            m_toggle->setIcon(icon(expanded ? Glyph::Chevron : Glyph::Next, c.muted));
            QString status = aiStepLabel(entry.state);
            if (entry.state == AiStepState::Succeeded) m_dot = c.success;
            else if (entry.state == AiStepState::Failed) {
                m_dot = c.error;
                if (command && entry.result.resultKnown && entry.result.exitCode >= 0)
                    status += QStringLiteral(" exit %1").arg(entry.result.exitCode);
            } else if (entry.state == AiStepState::Running) m_dot = c.accent;
            else if (entry.state == AiStepState::AllowedFailure || entry.state == AiStepState::Unknown)
                m_dot = c.canvas.lightness() < 128 ? QColor("#edbd8b") : QColor("#925513");
            m_status->setText(entry.state == AiStepState::AllowedFailure ? tr("允许失败") : aiStepLabel(entry.state));
            m_status->setToolTip(status);
            m_status->setStyleSheet(QStringLiteral("color:%1;").arg(css(m_dot)));
            const auto duration = command ? entry.result.durationMs : activity.durationMs;
            m_duration->setText(duration > 0 ? aiDurationLabel(duration) : QString());
            m_duration->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
            if (expanded && !m_details) {
                m_details = new QWidget(this);
                auto *details = new QVBoxLayout(m_details); details->setContentsMargins(0, 4, 0, 4);
                m_description = plainLabel({}, m_details); m_description->setWordWrap(true);
                m_description->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                m_description->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
                details->addWidget(m_description);
                const bool communication = !command && activity.kind == AiActivityKind::AgentMessage;
                m_command = new OutputBlock(command ? tr("命令 · shell") : communication ? tr("发送消息") : tr("输入参数"),
                                            m_details, command ? QStringLiteral("bash") : activity.inputLanguage);
                details->addWidget(m_command);
                m_stdout = new OutputBlock(command ? tr("输出 stdout / 终端") : communication ? tr("回执 / 回复") : tr("返回内容"),
                                           m_details, command ? QStringLiteral("text") : activity.outputLanguage);
                details->addWidget(m_stdout);
                m_stderr = new OutputBlock(tr("错误 stderr"), m_details); details->addWidget(m_stderr);
                layout()->addWidget(m_details);
            }
            if (m_details) {
                m_details->setVisible(expanded);
                if (expanded) {
                    m_description->setText((command ? entry.command.description : activity.name)
                        + (identity.isEmpty() ? QString() : QStringLiteral("\n") + identity)
                        + QStringLiteral("\n") + status
                        + (duration > 0 ? QStringLiteral(" · ") + aiDurationLabel(duration) : QString())
                        + (entry.text.isEmpty() ? QString() : QStringLiteral("\n") + entry.text));
                    m_command->setText(command ? entry.command.cmd : activity.input);
                    m_command->setVisible(command || !activity.input.isEmpty());
                    m_stdout->setText(ChatHistory::stripAnsi(command ? entry.result.stdoutText : activity.output));
                    m_stderr->setText(ChatHistory::stripAnsi(entry.result.stderrText));
                    m_stdout->setVisible(command ? !entry.result.stdoutText.isEmpty() : !activity.output.isEmpty());
                    m_stderr->setVisible(command && !entry.result.stderrText.isEmpty());
                }
            }
        } else {
            const bool showBody = !entry.finalAnswer && !entry.text.isEmpty();
            m_copyText = entry.text;
            m_body->setVisible(showBody); m_messageHeader->setVisible(showBody);
            if (showBody) m_body->setMarkdownText(entry.text);
            m_reasonToggle->setVisible(!entry.reasoning.isEmpty());
            m_reasonToggle->setChecked(expanded);
            m_reasonToggle->setIcon(icon(expanded ? Glyph::Chevron : Glyph::Next, c.muted));
            m_reason->setVisible(expanded && !entry.reasoning.isEmpty());
            if (expanded) { m_reason->setPlainText(entry.reasoning); m_reason->updateGeometry(); }
            setVisible(showBody || !entry.reasoning.isEmpty());
        }
        fitMetadata();
        updateGeometry(); update();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        fitMetadata();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const auto c = colors(palette());
        const qreal y = 5 + fontMetrics().height() / 2.0 + 5;
        painter.setPen(QPen(c.border, 1)); painter.drawLine(QPointF(9, 0), QPointF(9, height()));
        painter.setPen(Qt::NoPen); painter.setBrush(c.canvas); painter.drawEllipse(QPointF(9, y), 6, 6);
        painter.setBrush(m_dot); painter.drawEllipse(QPointF(9, y), 3.3, 3.3);
    }
private:
    static QString activitySummary(const AiActivity &activity)
    {
        if (activity.kind == AiActivityKind::AgentMessage)
            return activity.agent + QStringLiteral(" → ") + activity.recipient;
        if (activity.kind == AiActivityKind::Mcp)
            return activity.server + QStringLiteral(" / ") + activity.name;
        if (activity.kind == AiActivityKind::Skill)
            return QStringLiteral("/") + activity.name + (activity.summary.isEmpty() ? QString() : QStringLiteral(" · ") + activity.summary);
        return activity.summary.isEmpty() ? activity.name : activity.summary;
    }
    static QString activityIdentity(const AiActivity &activity)
    {
        QStringList parts;
        if (!activity.agent.isEmpty()) parts << tr("Agent %1").arg(activity.agent);
        if (!activity.recipient.isEmpty()) parts << tr("接收方 %1").arg(activity.recipient);
        if (!activity.server.isEmpty()) parts << tr("服务器 %1").arg(activity.server);
        if (!activity.callId.isEmpty()) parts << tr("调用 %1").arg(activity.callId);
        if (!activity.parentCallId.isEmpty()) parts << tr("父调用 %1").arg(activity.parentCallId);
        return parts.join(QStringLiteral(" · "));
    }
    void fitMetadata()
    {
        if (!m_duration) return;
        const int required = m_toggle->sizeHint().width() + m_status->sizeHint().width()
            + m_duration->sizeHint().width() + fontMetrics().horizontalAdvance(QStringLiteral("MMMM")) + 86;
        m_duration->setVisible(width() >= required);
    }
    AiConversationModel *m_model;
    quint64 m_turnId, m_id;
    AiConversationEntry::Kind m_kind;
    int m_revision = -1;
    bool m_expanded = false;
    QColor m_dot;
    QString m_copyText;
    QToolButton *m_toggle = nullptr, *m_reasonToggle = nullptr;
    ElidedLabel *m_summary = nullptr, *m_context = nullptr;
    QLabel *m_status = nullptr, *m_duration = nullptr, *m_description = nullptr;
    MarkdownView *m_body = nullptr;
    QTextBrowser *m_reason = nullptr;
    QWidget *m_details = nullptr, *m_messageHeader = nullptr;
    OutputBlock *m_command = nullptr, *m_stdout = nullptr, *m_stderr = nullptr;
};
} // namespace

class AiTurnWidget : public QWidget {
public:
    AiTurnWidget(AiConversationModel *model, const AiConversationTurn &turn, QWidget *parent)
        : QWidget(parent), m_model(model), m_id(turn.id)
    {
        using namespace sessionui;
        setObjectName(QStringLiteral("aiTurn_%1").arg(m_id));
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 8, 0, 20); layout->setSpacing(10);
        const auto c = colors(palette());
        auto *user = new QFrame(this); user->setObjectName(QStringLiteral("aiUserBubble"));
        user->setStyleSheet(QStringLiteral("QFrame#aiUserBubble { background:%1; border:1px solid %2; border-radius:12px; }").arg(css(c.user), css(c.userBorder)));
        auto *userLayout = new QVBoxLayout(user); userLayout->setContentsMargins(14, 10, 14, 12);
        auto *userHeader = new QHBoxLayout;
        userHeader->addWidget(plainLabel(tr("你"), user, true)); userHeader->addStretch();
        auto *copyQuestion = iconButton(Glyph::Copy, tr("复制提问"), c, user);
        connect(copyQuestion, &QToolButton::clicked, this, [copyQuestion, text = turn.userText] { copyFeedback(copyQuestion, text); });
        userHeader->addWidget(copyQuestion); userLayout->addLayout(userHeader);
        auto *question = richText(turn.userText.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")), c, user);
        userLayout->addWidget(question); layout->addWidget(user);
        auto *header = new QHBoxLayout;
        auto *avatar = plainLabel({}, this); avatar->setPixmap(icon(Glyph::Cube, c.accent).pixmap(24, 24));
        avatar->setFixedSize(28, 28); header->addWidget(avatar);
        header->addWidget(plainLabel(tr("AI 助手"), this, true));
        auto *modelLabel = new ElidedLabel(turn.model, this);
        modelLabel->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
        header->addWidget(modelLabel, 1); layout->addLayout(header);

        m_process = new QWidget(this);
        auto *processLayout = new QVBoxLayout(m_process); processLayout->setContentsMargins(0, 0, 0, 0); processLayout->setSpacing(4);
        auto *groupBar = new QHBoxLayout;
        m_toggle = disclosureButton(tr("执行过程"), c, m_process);
        m_toggle->setObjectName(QStringLiteral("aiProcessToggle_%1").arg(m_id));
        groupBar->addWidget(m_toggle);
        m_stats = new ElidedLabel({}, m_process); m_stats->setObjectName(QStringLiteral("aiProcessStats_%1").arg(m_id));
        groupBar->addWidget(m_stats, 1);
        m_failure = plainLabel({}, m_process); groupBar->addWidget(m_failure);
        processLayout->addLayout(groupBar);
        m_entries = new QWidget(m_process);
        m_entryLayout = new QVBoxLayout(m_entries); m_entryLayout->setContentsMargins(8, 2, 0, 0); m_entryLayout->setSpacing(0);
        processLayout->addWidget(m_entries); layout->addWidget(m_process);
        connect(m_toggle, &QToolButton::clicked, this, [this](bool expanded) { m_model->setProcessCollapsed(m_id, !expanded); });

        m_phase = plainLabel({}, this); m_phase->setWordWrap(true);
        m_phase->setObjectName(QStringLiteral("aiPhase_%1").arg(m_id)); layout->addWidget(m_phase);
        m_result = new QWidget(this);
        auto *resultLayout = new QVBoxLayout(m_result); resultLayout->setContentsMargins(2, 4, 2, 0); resultLayout->setSpacing(7);
        auto *resultHeader = new QHBoxLayout;
        m_resultTitle = plainLabel(tr("最终结果"), m_result, true); resultHeader->addWidget(m_resultTitle, 1);
        m_copy = iconButton(Glyph::Copy, tr("复制 Markdown 原文"), c, m_result);
        m_copy->setObjectName(QStringLiteral("aiCopyResult_%1").arg(m_id));
        connect(m_copy, &QToolButton::clicked, this, [this] { copyFeedback(m_copy, m_resultText); });
        resultHeader->addWidget(m_copy);
        auto *more = iconButton(Glyph::More, tr("更多复制选项"), c, m_result);
        auto *menu = new QMenu(more);
        menu->addAction(tr("复制 Markdown 原文"), this, [this] { copyFeedback(m_copy, m_resultText); });
        auto *plain = menu->addAction(tr("复制纯文本"), this, [this] {
            class SafeDocument : public QTextDocument {
                QVariant loadResource(int, const QUrl &) override { return {}; }
            } doc;
            doc.setMarkdown(m_resultText, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
            copyFeedback(m_copy, doc.toPlainText());
        });
        plain->setObjectName(QStringLiteral("aiCopyPlainText_%1").arg(m_id));
        more->setMenu(menu); more->setPopupMode(QToolButton::InstantPopup);
        resultHeader->addWidget(more); resultLayout->addLayout(resultHeader);
        m_body = new MarkdownView(m_result);
        m_body->setObjectName(QStringLiteral("aiFinalReport_%1").arg(m_id)); resultLayout->addWidget(m_body);
        layout->addWidget(m_result);
        sync(turn);
    }
    void sync(const AiConversationTurn &turn)
    {
        using namespace sessionui;
        const auto c = colors(palette());
        int commands = 0, failures = 0, steps = 0, messages = 0, notices = 0;
        QMap<AiActivityKind, int> activities;
        bool reasoning = false;
        QString preview;
        QSet<quint64> commandParents;
        for (const auto &entry : turn.entries)
            if (entry.kind == AiConversationEntry::Command && entry.parentEntryId) commandParents.insert(entry.parentEntryId);
        for (const auto &entry : turn.entries) {
            commands += entry.kind == AiConversationEntry::Command;
            failures += entry.kind == AiConversationEntry::Command && entry.state == AiStepState::Failed;
            messages += entry.kind == AiConversationEntry::Message;
            notices += entry.kind == AiConversationEntry::Notice;
            if (entry.kind == AiConversationEntry::Activity) {
                ++activities[entry.activity.kind];
                if (!commandParents.contains(entry.id) && entry.state == AiStepState::Failed) ++failures;
            }
            reasoning |= !entry.reasoning.isEmpty();
            if (!entry.finalAnswer && (!entry.text.isEmpty() || !entry.reasoning.isEmpty()
                                       || entry.kind == AiConversationEntry::Command || entry.kind == AiConversationEntry::Activity)) ++steps;
            if (entry.kind == AiConversationEntry::Message && !entry.finalAnswer) preview = entry.text;
        }
        const bool process = commands > 0 || messages > 1 || reasoning || notices > 0 || !activities.isEmpty();
        // Plain replies render once, in the reply area. Instantiate process
        // rows only when the turn actually acquires an execution timeline.
        if (process) for (const auto &entry : turn.entries) {
            auto *widget = m_rows.value(entry.id);
            if (!widget) { widget = new EntryWidget(m_model, turn.id, entry, m_entries); m_rows.insert(entry.id, widget); m_entryLayout->addWidget(widget); }
            widget->sync(entry, turn.expandedEntries.contains(entry.id));
        }
        m_process->setVisible(process);
        m_toggle->setChecked(!turn.processCollapsed);
        m_toggle->setIcon(icon(turn.processCollapsed ? Glyph::Next : Glyph::Chevron, c.muted));
        m_entries->setVisible(!turn.processCollapsed);
        QStringList counts{tr("%1 步").arg(steps)};
        if (commands) counts << tr("%1 个命令").arg(commands);
        for (auto it = activities.cbegin(); it != activities.cend(); ++it)
            counts << tr("%1 %2").arg(it.value()).arg(aiActivityLabel(it.key()));
        const QString stats = counts.join(QStringLiteral(" · "));
        m_stats->setText(stats); m_stats->setToolTip(stats);
        m_stats->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
        m_failure->setText(failures ? tr("%1 次失败").arg(failures) : QString());
        m_failure->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.error)));
        refreshPhase(turn);
        const bool finished = turn.phase == AiPhase::Finished;
        const QString body = finished ? turn.finalText : process ? QString() : preview;
        m_result->setVisible(!body.isEmpty());
        m_resultTitle->setVisible(finished); m_copy->setVisible(!body.isEmpty());
        m_resultTitle->setText(turn.localReport ? tr("本地执行摘要") : commands ? tr("最终结果") : tr("回复"));
        if (body != m_resultText) {
            m_resultText = body;
            m_body->setMarkdownText(body);
        }
        updateGeometry();
    }
    void refreshPhase(const AiConversationTurn &turn)
    {
        const auto c = sessionui::colors(palette());
        QString phase = aiPhaseLabel(turn.phase);
        if (turn.phase == AiPhase::Finished) {
            switch (turn.outcome) {
            case AiTurnOutcome::Complete: phase = tr("回复已完成"); break;
            case AiTurnOutcome::Incomplete: phase = tr("流程已结束 · 存在未验证项"); break;
            case AiTurnOutcome::Failed: phase = tr("未取得完整报告"); break;
            case AiTurnOutcome::Cancelled: phase = tr("已中断"); break;
            }
        }
        m_phase->setText(tr("%1 · 本轮 %2").arg(phase, aiDurationLabel(turn.phase == AiPhase::Finished ? turn.durationMs : turn.clock.elapsed())));
        m_phase->setStyleSheet(QStringLiteral("color:%1;").arg(sessionui::css(turn.phase == AiPhase::Finished ? c.muted : c.accent)));
    }
private:
    AiConversationModel *m_model;
    quint64 m_id;
    QWidget *m_process, *m_entries, *m_result;
    QVBoxLayout *m_entryLayout;
    QToolButton *m_toggle, *m_copy;
    sessionui::ElidedLabel *m_stats;
    QLabel *m_failure, *m_phase, *m_resultTitle;
    MarkdownView *m_body;
    QString m_resultText;
    QHash<quint64, EntryWidget *> m_rows;
};

AiConversationView::AiConversationView(QWidget *parent) : QScrollArea(parent)
{
    setObjectName(QStringLiteral("aiConversationView"));
    setFrameShape(QFrame::NoFrame); setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget(this);
    m_layout = new QVBoxLayout(m_content); m_layout->setContentsMargins(14, 12, 14, 28); m_layout->setSpacing(6);
    m_empty = new QLabel(tr("AI 运维助手\n\n描述你想完成的任务\n执行过程可随时展开，结果会单独呈现"), m_content);
    m_empty->setAlignment(Qt::AlignCenter); m_empty->setWordWrap(true);
    m_layout->addWidget(m_empty); m_layout->addStretch(); setWidget(m_content);
    m_jump = new QToolButton(viewport()); m_jump->setText(tr("↓ 回到底部"));
    m_jump->setObjectName(QStringLiteral("aiJumpToLatest")); m_jump->setAutoRaise(false);
    m_jump->setFocusPolicy(Qt::StrongFocus); m_jump->hide();
    connect(m_jump, &QToolButton::clicked, this, &AiConversationView::scrollToLatest);
    m_renderTimer.setInterval(60); m_renderTimer.setSingleShot(true);
    connect(&m_renderTimer, &QTimer::timeout, this, &AiConversationView::flush);
    m_clockTimer.setInterval(1000);
    connect(&m_clockTimer, &QTimer::timeout, this, [this] {
        if (!m_model || !m_model->isBusy()) return;
        const auto *turn = m_model->currentTurn();
        if (auto *widget = m_turnWidgets.value(turn->id)) widget->refreshPhase(*turn);
    });
    m_clockTimer.start();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_adjusting) m_following = verticalScrollBar()->maximum() - verticalScrollBar()->value() < 24;
        updateJumpButton();
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
        if (m_following) QTimer::singleShot(0, this, [this] { if (m_following) scrollToLatest(); });
        updateJumpButton();
    });
}

void AiConversationView::clearWidgets()
{
    m_renderTimer.stop(); m_dirty.clear();
    qDeleteAll(m_turnWidgets); m_turnWidgets.clear();
}

void AiConversationView::setModel(AiConversationModel *model)
{
    if (m_model == model) return;
    if (m_model) {
        m_positions.insert(m_model, verticalScrollBar()->value());
    }
    disconnect(m_changeConnection);
    disconnect(m_resetConnection);
    clearWidgets(); m_model = model;
    m_following = !m_positions.contains(model);
    if (model) {
        m_changeConnection = connect(model, &AiConversationModel::changed, this, [this](quint64 id) {
            m_dirty.insert(id);
            if (!m_model->isBusy()) flush();
            else if (!m_renderTimer.isActive()) m_renderTimer.start();
        });
        m_resetConnection = connect(model, &AiConversationModel::reset, this, [this] { clearWidgets(); m_empty->show(); m_following = true; });
        if (!m_observedModels.contains(model)) {
            m_observedModels.insert(model);
            connect(model, &QObject::destroyed, this, [this, model] {
                m_positions.remove(model); m_observedModels.remove(model);
                if (!m_model) { clearWidgets(); m_empty->show(); }
            });
        }
        for (const auto &turn : model->turns()) m_dirty.insert(turn.id);
    }
    flush();
    const int position = m_positions.value(model, -1);
    QTimer::singleShot(0, this, [this, guard = QPointer<AiConversationModel>(model), position] {
        if (m_model != guard) return;
        if (position < 0) scrollToLatest();
        else { m_adjusting = true; verticalScrollBar()->setValue(position); m_adjusting = false; updateJumpButton(); }
    });
}

void AiConversationView::flush()
{
    m_renderTimer.stop();
    m_empty->setVisible(!m_model || m_model->turns().isEmpty());
    if (!m_model) return;
    const int position = verticalScrollBar()->value();
    const bool following = m_following;
    m_adjusting = true;
    for (const auto &turn : m_model->turns()) {
        if (!m_dirty.contains(turn.id)) continue;
        auto *widget = m_turnWidgets.value(turn.id);
        if (!widget) {
            widget = new AiTurnWidget(m_model, turn, m_content);
            m_turnWidgets.insert(turn.id, widget);
            m_layout->insertWidget(m_layout->count() - 1, widget);
        } else widget->sync(turn);
    }
    m_dirty.clear(); m_layout->activate();
    if (!following) verticalScrollBar()->setValue(position);
    m_following = following; m_adjusting = false;
    updateJumpButton();
    if (following) QTimer::singleShot(0, this, [this] { if (m_following) scrollToLatest(); });
}

void AiConversationView::scrollToLatest()
{
    m_following = true; m_adjusting = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_adjusting = false; updateJumpButton();
}

void AiConversationView::updateJumpButton()
{
    m_jump->adjustSize();
    m_jump->move(qMax(4, (viewport()->width() - m_jump->width()) / 2), qMax(0, viewport()->height() - m_jump->height() - 12));
    m_jump->setVisible(!m_following && verticalScrollBar()->maximum() > 0);
    m_jump->raise();
}

void AiConversationView::changeEvent(QEvent *event)
{
    QScrollArea::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange) {
        QTimer::singleShot(0, this, [this] {
            const int position = verticalScrollBar()->value();
            clearWidgets();
            if (m_model) for (const auto &turn : m_model->turns()) m_dirty.insert(turn.id);
            flush();
            if (!m_following) verticalScrollBar()->setValue(position);
        });
    }
}

void AiConversationView::resizeEvent(QResizeEvent *event)
{
    QScrollArea::resizeEvent(event); updateJumpButton();
}
} // namespace cubeshell
