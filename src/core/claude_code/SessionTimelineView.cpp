// SessionTimelineView.cpp — 逻辑行生成、可见行按需实例化与变高布局校准。
// 不为整份历史创建控件，也不构造巨型富文本文档，控制长会话的界面开销。

#include "claude_code/SessionTimelineView.h"
#include "ai/MarkdownRenderer.h"
#include <QLayout>
#include <QEvent>
#include <QApplication>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>

namespace cubeshell {
using namespace sessionui;

SessionTimelineView::SessionTimelineView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("sessionTimeline"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    verticalScrollBar()->setSingleStep(42);
    setFocusPolicy(Qt::StrongFocus);
    applyTheme();
}
void SessionTimelineView::applyTheme()
{
    const auto next = colors(QApplication::palette());
    if (next.canvas == m_colors.canvas && next.accent == m_colors.accent && m_layoutFont == font()) return;
    m_colors = next; m_layoutFont = font();
    const auto *focused = QApplication::focusWidget();
    const auto focusName = focused && isAncestorOf(focused) ? focused->objectName() : QString();
    setStyleSheet(QStringLiteral(
        "QAbstractScrollArea#sessionTimeline { background:%1; border:0; }"
        "QScrollBar:vertical { background:%1; width:8px; margin:3px 0; }"
        "QScrollBar::handle:vertical { background:%2; border-radius:4px; min-height:40px; }"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }")
        .arg(css(m_colors.canvas), css(m_colors.border)));
    auto pal = viewport()->palette(); pal.setColor(QPalette::Window, m_colors.canvas);
    viewport()->setPalette(pal); viewport()->setAutoFillBackground(true);
    m_heights.clear();
    clearWidgets();
    refreshVisible();
    if (!focusName.isEmpty()) {
        if (auto *target = findChild<QWidget *>(focusName)) target->setFocus(Qt::OtherFocusReason);
    }
}
void SessionTimelineView::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (!m_themePending && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) {
        m_themePending = true;
        QTimer::singleShot(0, this, [this] { m_themePending = false; applyTheme(); });
    }
}
void SessionTimelineView::clearWidgets()
{
    ++m_generation;
    qDeleteAll(m_widgets);
    m_widgets.clear();
}
void SessionTimelineView::clear()
{
    m_data = std::make_shared<ClaudeSessionData>();
    m_turns.clear(); m_rows.clear(); m_positions.clear(); m_heights.clear();
    m_state = {}; m_state.expandAll = m_filter == Changes || m_filter == Failures;
    m_disclosures.clear(); m_complete = true; m_moreRequested = false; m_currentTurn = -1;
    clearWidgets(); verticalScrollBar()->setRange(0, 0); viewport()->update();
}
void SessionTimelineView::setSnapshot(const ClaudeSessionSnapshot &snapshot, bool complete)
{
    m_data = snapshot ? snapshot : std::make_shared<ClaudeSessionData>();
    m_turns = m_data->presentationTurns();
    m_complete = complete;
    m_moreRequested = false;
    // 结果可能在后续页才到达，已有展开行也会增高；旧高度缓存必须全部失效。
    m_heights.clear();
    rebuildRows();
}
int SessionTimelineView::contentWidth() const
{
    const int margin = viewport()->width() < 760 ? 14 : 32;
    return qMax(200, qMin(960, viewport()->width() - margin * 2));
}
int SessionTimelineView::estimate(const Row &row) const
{
    switch (row.kind) {
    case Row::UserMessage: return 156;
    case Row::AssistantHeading: return 68;
    case Row::ProcessHeading: return 42;
    case Row::ReplyMessage: return 130;
    case Row::EmptyReply: return 60;
    case Row::ProcessEntry: {
        const auto &entry = m_data->entries()[row.entry];
        if (entry.kind == ClaudeSessionEntry::Assistant) return 76;
        return (m_state.expandAll != m_state.expandedEntries.contains(row.entry)) ? 260 : 42;
    }
    }
    return 42;
}
SessionTimelineView::Anchor SessionTimelineView::anchor() const
{
    const int index = rowAt(verticalScrollBar()->value());
    if (index < 0) return {};
    return {m_rows[index].key(), verticalScrollBar()->value() - m_positions[index]};
}
void SessionTimelineView::restoreAnchor(const Anchor &saved)
{
    if (!saved.key) return;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].key() == saved.key) {
            verticalScrollBar()->setValue(m_positions[i] + saved.offset);
            return;
        }
    }
}
// 先生成轻量逻辑行，不直接创建控件；筛选 / 折叠只改变行映射，不修改历史快照。
void SessionTimelineView::rebuildRows(bool preserveAnchor)
{
    const auto saved = preserveAnchor ? anchor() : Anchor{};
    const auto *focused = QApplication::focusWidget();
    const auto focusName = focused && isAncestorOf(focused) ? focused->objectName() : QString();
    m_layouting = true;
    clearWidgets(); m_rows.clear();
    for (int t = 0; t < m_turns.size(); ++t) {
        const auto &turn = m_turns[t];
        const int firstBody = turn.firstEntry + (turn.userEntry >= 0 ? 1 : 0);
        if (m_filter == Changes || m_filter == Failures) {
            QList<int> included;
            for (int i = firstBody; i < turn.endEntry; ++i) {
                const auto &entry = m_data->entries()[i];
                if ((m_filter == Changes && entry.isChange())
                    || (m_filter == Failures && entry.kind == ClaudeSessionEntry::Tool && entry.failed))
                    included.append(i);
            }
            if (included.isEmpty()) continue;
            if (turn.userEntry >= 0) m_rows.append({Row::UserMessage, turn.userEntry, t});
            m_rows.append({Row::AssistantHeading, included.first(), t});
            for (int i : included) m_rows.append({Row::ProcessEntry, i, t});
            continue;
        }
        if (turn.userEntry >= 0) m_rows.append({Row::UserMessage, turn.userEntry, t});
        m_rows.append({Row::AssistantHeading, qMin(firstBody, turn.endEntry - 1), t});
        if (m_filter == All && turn.steps > 0) {
            m_rows.append({Row::ProcessHeading, turn.firstEntry, t});
            if (!m_state.collapsedTurns.contains(turn.firstEntry)) {
                for (int i = firstBody; i < turn.endEntry; ++i) {
                    if (turn.firstReply >= 0 && i >= turn.firstReply && m_data->entries()[i].kind == ClaudeSessionEntry::Assistant)
                        continue;
                    m_rows.append({Row::ProcessEntry, i, t});
                }
            }
        }
        if (turn.firstReply >= 0) {
            for (int i = turn.firstReply; i < turn.endEntry; ++i) {
                if (m_data->entries()[i].kind == ClaudeSessionEntry::Assistant)
                    m_rows.append({Row::ReplyMessage, i, t});
            }
        } else m_rows.append({Row::EmptyReply, turn.firstEntry, t});
        // 未记录最终回复时，对话视图仍保留中途的助手说明，避免把已有文本一起隐藏。
        if (m_filter == Conversation && turn.firstReply < 0) {
            for (int i = firstBody; i < turn.endEntry; ++i) {
                if (m_data->entries()[i].kind == ClaudeSessionEntry::Assistant)
                    m_rows.insert(m_rows.size() - 1, {Row::ReplyMessage, i, t});
            }
        }
    }
    updatePositions(); restoreAnchor(saved);
    m_layouting = false;
    refreshVisible();
    if (!focusName.isEmpty()) {
        if (auto *target = findChild<QWidget *>(focusName)) target->setFocus(Qt::OtherFocusReason);
    }
}
void SessionTimelineView::updatePositions()
{
    m_positions.clear();
    int position = 8;
    for (const auto &row : m_rows) {
        m_positions.append(position);
        position += m_heights.value(row.key(), estimate(row));
    }
    position += 40;
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, qMax(0, position - viewport()->height()));
}
int SessionTimelineView::rowAt(int position) const
{
    if (m_rows.isEmpty() || m_positions.isEmpty()) return -1;
    auto it = std::upper_bound(m_positions.cbegin(), m_positions.cend(), position);
    return qBound(0, static_cast<int>(it - m_positions.cbegin()) - 1, static_cast<int>(m_rows.size()) - 1);
}
void SessionTimelineView::scheduleLayout()
{
    if (m_layoutScheduled) return;
    m_layoutScheduled = true;
    QTimer::singleShot(0, this, [this] { m_layoutScheduled = false; refreshVisible(); });
}
void SessionTimelineView::refreshVisible()
{
    if (m_layouting || m_rows.isEmpty()) { viewport()->update(); return; }
    m_layouting = true;
    const int width = contentWidth();
    if (width != m_contentWidth) {
        m_contentWidth = width; m_heights.clear();
    }
    const auto saved = anchor();
    const int x = (viewport()->width() - width) / 2;
    // 视口上下各预加载少量内容，以实测高度替换估值；每轮恢复逻辑锚点，避免正文跳动。
    // 限制校准次数，防止复杂长消息在一次滚动事件中反复布局。
    for (int pass = 0; pass < 4; ++pass) {
        const int start = rowAt(qMax(0, verticalScrollBar()->value() - 180));
        const int finish = rowAt(verticalScrollBar()->value() + viewport()->height() + 180);
        bool changed = false;
        for (int i = start; i <= finish; ++i) {
            const auto &row = m_rows[i];
            auto *widget = m_widgets.value(row.key());
            if (!widget) {
                widget = new SessionRowWidget(row, m_data, m_turns[row.turn], m_state, m_colors, viewport());
                m_widgets.insert(row.key(), widget);
                // 延迟到点击信号返回后再重建，避免销毁仍在发信号的按钮；代次校验屏蔽旧行操作。
                const auto generation = m_generation;
                connect(widget, &SessionRowWidget::actionRequested, this, [this, generation](int action, int entry, const QString &key) {
                    QTimer::singleShot(0, this, [this, generation, action, entry, key] {
                        if (m_generation == generation) onAction(action, entry, key);
                    });
                });
                connect(widget, &SessionRowWidget::metricsChanged, this, &SessionTimelineView::scheduleLayout);
                connect(widget, &SessionRowWidget::copied, this, &SessionTimelineView::copied);
                connect(widget, &SessionRowWidget::externalLinkRequested, this, &SessionTimelineView::externalLinkRequested);
                widget->show();
                if (m_state.highlightedEntry == row.entry) widget->highlight(m_state.query);
            }
            const int height = widget->preferredHeight(width);
            if (m_heights.value(row.key(), estimate(row)) != height) changed = true;
            m_heights.insert(row.key(), height);
        }
        if (!changed) break;
        updatePositions(); restoreAnchor(saved);
    }
    const int start = rowAt(qMax(0, verticalScrollBar()->value() - 180));
    const int finish = rowAt(verticalScrollBar()->value() + viewport()->height() + 180);
    QSet<quint64> visible;
    for (int i = start; i <= finish; ++i) {
        const auto &row = m_rows[i]; visible.insert(row.key());
        if (auto *widget = m_widgets.value(row.key()))
            widget->setGeometry(x, m_positions[i] - verticalScrollBar()->value(), width, m_heights.value(row.key(), estimate(row)));
    }
    for (auto it = m_widgets.begin(); it != m_widgets.end();) {
        if (!visible.contains(it.key())) { delete it.value(); it = m_widgets.erase(it); }
        else ++it;
    }
    const int current = rowAt(verticalScrollBar()->value() + 55);
    if (current >= 0 && m_currentTurn != m_rows[current].turn) {
        m_currentTurn = m_rows[current].turn;
        emit currentTurnChanged(m_currentTurn);
    }
    m_layouting = false;
    viewport()->update();
    if (!m_complete && !m_moreRequested && verticalScrollBar()->maximum() - verticalScrollBar()->value() < 220) {
        m_moreRequested = true;
        emit needMore();
    }
}
void SessionTimelineView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    scheduleLayout();
}
void SessionTimelineView::scrollContentsBy(int, int) { refreshVisible(); }
void SessionTimelineView::paintEvent(QPaintEvent *)
{
    if (!m_rows.isEmpty()) return;
    QPainter painter(viewport()); painter.setPen(m_colors.muted);
    painter.drawText(viewport()->rect().adjusted(30, 30, -30, -30), Qt::AlignCenter | Qt::TextWordWrap,
        m_complete ? tr("此视图没有可展示的内容") : tr("正在读取会话…"));
}
// 各筛选模式保存自己的工具展开例外；切换回来时不会丢掉用户手动收起的步骤。
void SessionTimelineView::setFilter(Filter filter)
{
    if (m_filter == filter) return;
    m_disclosures.insert(m_filter, {m_state.expandAll, m_state.expandedEntries});
    const auto disclosure = m_disclosures.value(filter, {filter == Changes || filter == Failures, {}});
    m_state.expandAll = disclosure.first; m_state.expandedEntries = disclosure.second;
    m_filter = filter; m_heights.clear(); rebuildRows(false); verticalScrollBar()->setValue(0);
}
void SessionTimelineView::setStepsExpanded(bool expanded)
{
    m_state.expandAll = expanded; m_state.expandedEntries.clear();
    if (expanded) m_state.collapsedTurns.clear();
    m_heights.clear(); rebuildRows();
}
void SessionTimelineView::setProcessesCollapsed(bool collapsed)
{
    m_state.collapsedTurns.clear();
    if (collapsed) for (const auto &turn : m_turns) m_state.collapsedTurns.insert(turn.firstEntry);
    rebuildRows();
}
void SessionTimelineView::onAction(int action, int entry, const QString &key)
{
    const auto toggle = [](auto &set, const auto &value) { if (set.contains(value)) set.remove(value); else set.insert(value); };
    switch (action) {
    case ToggleEntry: toggle(m_state.expandedEntries, entry); break;
    case ToggleTurn: toggle(m_state.collapsedTurns, entry); break;
    case ToggleParameters: toggle(m_state.parameters, entry); break;
    case ToggleMessage: toggle(m_state.fullMessages, entry); break;
    case ToggleRaw: toggle(m_state.rawMessages, entry); break;
    case ToggleCode: toggle(m_state.expandedCode, key); break;
    default: return;
    }
    m_heights.clear(); rebuildRows();
}
void SessionTimelineView::scrollToEntry(int entry, const QString &query)
{
    if (entry < 0 || entry >= m_data->entries().size()) return;
    for (const auto &turn : m_turns) {
        if (entry >= turn.firstEntry && entry < turn.endEntry) m_state.collapsedTurns.remove(turn.firstEntry);
    }
    if (!query.isEmpty()) {
        // 定位前先展开所有可能隐藏匹配内容的层级，否则只能滚到折叠标题，用户看不到命中。
        m_state.highlightedEntry = entry; m_state.query = query;
        m_state.fullMessages.insert(entry); m_state.rawMessages.insert(entry); m_state.parameters.insert(entry);
        if (m_state.expandAll) m_state.expandedEntries.remove(entry); else m_state.expandedEntries.insert(entry);
        for (const auto &suffix : {"input", "output", "before", "after", "content"})
            m_state.expandedCode.insert(QStringLiteral("%1:%2").arg(entry).arg(QLatin1String(suffix)));
        int blockIndex = 0;
        for (const auto &block : MarkdownRenderer::splitBlocks(m_data->entries()[entry].text)) {
            if (block.trimmed().startsWith(QLatin1String("```")) && block.contains(query, Qt::CaseInsensitive))
                m_state.expandedCode.insert(QStringLiteral("%1:fence:%2").arg(entry).arg(blockIndex));
            ++blockIndex;
        }
    }
    rebuildRows();
    for (int i = 0; i < m_rows.size(); ++i) {
        const auto &row = m_rows[i];
        if (row.entry != entry || row.kind == Row::AssistantHeading || row.kind == Row::ProcessHeading) continue;
        verticalScrollBar()->setValue(qMax(0, m_positions[i] - 16));
        refreshVisible();
        // 刚实例化的消息可能修正了估算高度，重新定位后再按行内匹配区域精确滚动。
        verticalScrollBar()->setValue(qMax(0, m_positions[i] - 16));
        if (!query.isEmpty()) {
            if (auto *widget = m_widgets.value(row.key())) {
                widget->layout()->activate();
                const auto match = widget->highlight(query);
                if (match.isValid()) verticalScrollBar()->setValue(qMax(0, m_positions[i] + match.top() - 48));
            }
        }
        break;
    }
}
void SessionTimelineView::scrollToTurn(int turn)
{
    if (turn >= 0 && turn < m_turns.size()) scrollToEntry(m_turns[turn].firstEntry);
}
void SessionTimelineView::scrollToLatest()
{
    // 首次滚到末尾会实例化新行并修正总高度，再滚一次才能贴住校准后的真正底部。
    verticalScrollBar()->setValue(verticalScrollBar()->maximum()); refreshVisible();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum()); refreshVisible();
}
} // namespace cubeshell
