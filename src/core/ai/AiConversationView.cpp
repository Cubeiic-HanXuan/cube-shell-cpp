#include "AiConversationView.h"
#include "MarkdownRenderer.h"
#include "ChatHistory.h"
#include "widgets/ConversationWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
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

void wireLinks(QTextBrowser *browser)
{
    QObject::connect(browser, &QTextBrowser::anchorClicked, browser, [](const QUrl &url) {
        if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
            QDesktopServices::openUrl(url);
    });
}

class OutputBlock : public QWidget {
public:
    OutputBlock(const QString &title, QWidget *parent) : QWidget(parent)
    {
        const auto c = colors(palette());
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 4, 10, 8); layout->setSpacing(2);
        auto *bar = new QHBoxLayout;
        m_title = new ElidedLabel(title, this); m_heading = title;
        bar->addWidget(m_title, 1);
        auto *copy = iconButton(Glyph::Copy, tr("复制完整内容"), c, this);
        copy->setObjectName(QStringLiteral("aiCopyOutput"));
        connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_text); });
        bar->addWidget(copy); layout->addLayout(bar);
        m_editor = codeView({}, c, this); m_editor->setObjectName(QStringLiteral("aiOutputText"));
        layout->addWidget(m_editor);
    }
    void setText(const QString &text)
    {
        if (m_text == text) return;
        m_text = text;
        const int position = m_editor->verticalScrollBar()->value();
        m_editor->setPlainText(text);
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
        if (m_kind == AiConversationEntry::Command) {
            auto *bar = new QHBoxLayout;
            bar->setSpacing(5);
            m_toggle = disclosureButton(QStringLiteral("SSH"), c, this);
            m_toggle->setObjectName(QStringLiteral("aiCommandToggle_%1").arg(m_id));
            m_toggle->setToolTip(tr("展开命令及完整输出"));
            bar->addWidget(m_toggle);
            m_summary = new ElidedLabel(entry.command.cmd.simplified(), this);
            auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            if (font().pixelSize() > 0) mono.setPixelSize(font().pixelSize());
            else mono.setPointSizeF(font().pointSizeF());
            m_summary->setFont(mono);
            bar->addWidget(m_summary, 1);
            m_status = plainLabel({}, this); bar->addWidget(m_status);
            m_duration = plainLabel({}, this); bar->addWidget(m_duration);
            layout->addLayout(bar);
            connect(m_toggle, &QToolButton::clicked, this, [this](bool expanded) {
                m_model->setEntryExpanded(m_turnId, m_id, expanded);
            });
        } else {
            m_body = richText({}, c, this); wireLinks(m_body);
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
        if (m_kind == AiConversationEntry::Command) {
            m_toggle->setChecked(expanded);
            m_toggle->setIcon(icon(expanded ? Glyph::Chevron : Glyph::Next, c.muted));
            QString status = aiStepLabel(entry.state);
            if (entry.state == AiStepState::Succeeded) m_dot = c.success;
            else if (entry.state == AiStepState::Failed) {
                m_dot = c.error; status += QStringLiteral(" exit %1").arg(entry.result.exitCode);
            } else if (entry.state == AiStepState::Running) m_dot = c.accent;
            m_status->setText(entry.state == AiStepState::AllowedFailure ? tr("允许失败") : status);
            m_status->setToolTip(status);
            m_status->setStyleSheet(QStringLiteral("color:%1;").arg(css(m_dot)));
            m_duration->setText(entry.result.durationMs > 0 ? aiDurationLabel(entry.result.durationMs) : QString());
            m_duration->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
            if (expanded && !m_details) {
                m_details = new QWidget(this);
                auto *details = new QVBoxLayout(m_details); details->setContentsMargins(0, 4, 0, 4);
                m_description = plainLabel({}, m_details); m_description->setWordWrap(true);
                details->addWidget(m_description);
                m_command = new OutputBlock(tr("命令"), m_details); details->addWidget(m_command);
                m_stdout = new OutputBlock(tr("输出 stdout / 终端"), m_details); details->addWidget(m_stdout);
                m_stderr = new OutputBlock(tr("错误 stderr"), m_details); details->addWidget(m_stderr);
                layout()->addWidget(m_details);
            }
            if (m_details) {
                m_details->setVisible(expanded);
                if (expanded) {
                    m_description->setText(entry.command.description + QStringLiteral("\n") + status
                        + (entry.result.durationMs > 0 ? QStringLiteral(" · ") + aiDurationLabel(entry.result.durationMs) : QString())
                        + (entry.text.isEmpty() ? QString() : QStringLiteral("\n") + entry.text));
                    m_command->setText(entry.command.cmd);
                    m_stdout->setText(ChatHistory::stripAnsi(entry.result.stdoutText));
                    m_stderr->setText(ChatHistory::stripAnsi(entry.result.stderrText));
                    m_stdout->setVisible(!entry.result.stdoutText.isEmpty());
                    m_stderr->setVisible(!entry.result.stderrText.isEmpty());
                }
            }
        } else {
            const bool showBody = !entry.finalAnswer && !entry.text.isEmpty();
            m_body->setVisible(showBody);
            if (showBody) { m_body->setHtml(m_renderer.render(entry.text)); m_body->updateGeometry(); }
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
    void fitMetadata()
    {
        if (!m_duration) return;
        const int required = m_toggle->sizeHint().width() + m_status->sizeHint().width()
            + m_duration->sizeHint().width() + fontMetrics().horizontalAdvance(QStringLiteral("MMMM")) + 42;
        m_duration->setVisible(width() >= required);
    }
    AiConversationModel *m_model;
    quint64 m_turnId, m_id;
    AiConversationEntry::Kind m_kind;
    int m_revision = -1;
    bool m_expanded = false;
    QColor m_dot;
    MarkdownRenderer m_renderer;
    QToolButton *m_toggle = nullptr, *m_reasonToggle = nullptr;
    ElidedLabel *m_summary = nullptr;
    QLabel *m_status = nullptr, *m_duration = nullptr, *m_description = nullptr;
    QTextBrowser *m_body = nullptr, *m_reason = nullptr;
    QWidget *m_details = nullptr;
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
        connect(copyQuestion, &QToolButton::clicked, this, [text = turn.userText] { QApplication::clipboard()->setText(text); });
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
        m_copy = iconButton(Glyph::Copy, tr("复制结果"), c, m_result);
        m_copy->setObjectName(QStringLiteral("aiCopyResult_%1").arg(m_id));
        connect(m_copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_resultText); });
        resultHeader->addWidget(m_copy); resultLayout->addLayout(resultHeader);
        m_body = richText({}, c, m_result); wireLinks(m_body);
        m_body->setObjectName(QStringLiteral("aiFinalReport_%1").arg(m_id)); resultLayout->addWidget(m_body);
        layout->addWidget(m_result);
        sync(turn);
    }
    void sync(const AiConversationTurn &turn)
    {
        using namespace sessionui;
        const auto c = colors(palette());
        int commands = 0, failures = 0, steps = 0, messages = 0;
        bool reasoning = false;
        QString preview;
        for (const auto &entry : turn.entries) {
            commands += entry.kind == AiConversationEntry::Command;
            failures += entry.kind == AiConversationEntry::Command && entry.state == AiStepState::Failed;
            messages += entry.kind == AiConversationEntry::Message;
            reasoning |= !entry.reasoning.isEmpty();
            if (!entry.finalAnswer && (!entry.text.isEmpty() || !entry.reasoning.isEmpty()
                                       || entry.kind == AiConversationEntry::Command)) ++steps;
            if (entry.kind == AiConversationEntry::Message && !entry.finalAnswer) preview = entry.text;
            auto *widget = m_rows.value(entry.id);
            if (!widget) { widget = new EntryWidget(m_model, turn.id, entry, m_entries); m_rows.insert(entry.id, widget); m_entryLayout->addWidget(widget); }
            widget->sync(entry, turn.expandedEntries.contains(entry.id));
        }
        const bool process = commands > 0 || messages > 1 || reasoning;
        m_process->setVisible(process);
        m_toggle->setChecked(!turn.processCollapsed);
        m_toggle->setIcon(icon(turn.processCollapsed ? Glyph::Next : Glyph::Chevron, c.muted));
        m_entries->setVisible(!turn.processCollapsed);
        const QString stats = tr("%1 步 · %2 个命令").arg(steps).arg(commands);
        m_stats->setText(stats); m_stats->setToolTip(stats);
        m_stats->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
        m_failure->setText(failures ? tr("%1 次失败").arg(failures) : QString());
        m_failure->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.error)));
        refreshPhase(turn);
        const bool finished = turn.phase == AiPhase::Finished;
        const QString body = finished ? turn.finalText : process ? QString() : preview;
        m_result->setVisible(!body.isEmpty());
        m_resultTitle->setVisible(finished); m_copy->setVisible(finished);
        m_resultTitle->setText(turn.localReport ? tr("本地执行摘要") : commands ? tr("最终结果") : tr("回复"));
        if (body != m_resultText) {
            m_resultText = body;
            m_body->setHtml(m_renderer.render(body)); m_body->updateGeometry();
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
    QTextBrowser *m_body;
    QString m_resultText;
    MarkdownRenderer m_renderer;
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
