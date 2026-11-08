// SessionWidgets.cpp — 原生消息气泡、执行步骤、只读代码块及目录卡片的绘制。
// 显示文本与复制原文分离；外链只上报请求，不在富文本控件内自动加载资源。

#include "claude_code/SessionWidgets.h"
#include "claude_code/ClaudeCodeRenderer.h"
#include "ai/MarkdownRenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace cubeshell {
namespace sessionui {
QString timeLabel(qint64 ms, bool date)
{
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().toString(
        date ? QStringLiteral("MM-dd HH:mm") : QStringLiteral("HH:mm")) : QString();
}
QString durationLabel(qint64 ms)
{
    if (ms < 0) return {};
    if (ms < 1000) return QStringLiteral("%1 ms").arg(ms);
    if (ms < 60000) return QStringLiteral("%1 s").arg(ms / 1000);
    return QStringLiteral("%1m %2s").arg(ms / 60000).arg((ms / 1000) % 60);
}
QString toolSummary(const ClaudeSessionEntry &entry)
{
    QString text = entry.input.value(QStringLiteral("command")).toString();
    if (text.isEmpty()) text = entry.filePath();
    if (text.isEmpty()) text = entry.input.value(QStringLiteral("pattern")).toString();
    if (text.isEmpty()) text = entry.text;
    if (text.isEmpty()) text = entry.input.value(QStringLiteral("prompt")).toString();
    return text.simplified();
}
QString messagePreview(const QString &text)
{
    // 与消息正文共用转义 / 格式化规则，再提取纯文本，列表摘要不直接显示 Markdown 标记。
    QTextDocument document;
    document.setHtml(ClaudeCodeRenderer::toHtml(ClaudeSessionData::cleanMessage(text)));
    return document.toPlainText().simplified();
}
} // namespace sessionui

namespace {
using namespace sessionui;
class Surface : public QWidget {
public:
    Surface(QColor fill, QColor border, QWidget *parent) : QWidget(parent), m_fill(fill), m_border(border)
    { setObjectName(QStringLiteral("sessionSurface")); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(m_border); p.setBrush(m_fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 12, 12);
    }
private:
    QColor m_fill, m_border;
};

QLabel *label(const QString &text, const Colors &c, QWidget *parent, bool muted = false, bool bold = false)
{
    auto *widget = new QLabel(text, parent);
    widget->setTextFormat(Qt::PlainText);
    widget->setStyleSheet(QStringLiteral("background:transparent; border:0; color:%1;").arg(css(muted ? c.muted : c.text)));
    auto font = widget->font(); font.setBold(bold); widget->setFont(font);
    return widget;
}
QToolButton *textButton(const QString &text, const Colors &c, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text); button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { color:%1; background:transparent; border:1px solid transparent; border-radius:5px; padding:4px 5px; text-align:left; }"
        "QToolButton:hover { color:%2; background:%3; } QToolButton:pressed { background:%4; } QToolButton:focus { border-color:%2; }")
        .arg(css(c.muted), css(c.accent), css(c.raised), css(c.accentSoft)));
    return button;
}
void wrapped(QPainter *p, const QString &text, const QFont &font, const QColor &color, const QRectF &rect, int lines)
{
    QTextLayout layout(text, font);
    QTextOption opt; opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); layout.setTextOption(opt);
    layout.beginLayout();
    qreal y = 0;
    for (int i = 0; i < lines; ++i) {
        auto line = layout.createLine(); if (!line.isValid()) break;
        line.setLineWidth(rect.width()); line.setPosition(QPointF(0, y)); y += line.height();
    }
    layout.endLayout();
    p->save(); p->setClipRect(rect); p->setPen(color); p->setFont(font);
    for (int i = 0; i < layout.lineCount(); ++i) {
        const auto line = layout.lineAt(i);
        if (i == lines - 1) {
            const auto remaining = text.mid(line.textStart()).simplified();
            p->drawText(rect.topLeft() + QPointF(0, line.y() + line.ascent()),
                        QFontMetrics(font).elidedText(remaining, Qt::ElideRight, static_cast<int>(rect.width())));
        } else line.draw(p, rect.topLeft());
    }
    p->restore();
}
} // namespace

SessionRowWidget::SessionRowWidget(const sessionui::Row &row, const ClaudeSessionSnapshot &data,
                                   const ClaudeSessionTurn &turn, const sessionui::ViewState &state,
                                   const sessionui::Colors &colors, QWidget *parent)
    : QWidget(parent), m_row(row), m_data(data), m_turn(turn), m_state(state), m_colors(colors)
{
    using namespace sessionui;
    setObjectName(QStringLiteral("sessionRow"));
    setStyleSheet(QStringLiteral("QWidget#sessionRow, QWidget#sessionSurface, QWidget#sessionContent { background:transparent; border:0; } QLabel { background:transparent; border:0; }"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(48, 0, 4, 0);
    m_layout->setSpacing(8);
    const auto &entry = data->entries()[row.entry];
    if (row.kind == Row::UserMessage || row.kind == Row::ReplyMessage) {
        buildMessage(entry, row.kind == Row::UserMessage);
    } else if (row.kind == Row::AssistantHeading) {
        m_layout->setContentsMargins(0, 22, 0, 8);
        auto *header = new QHBoxLayout;
        auto *avatar = label(QString(), colors, this);
        avatar->setPixmap(icon(Glyph::Cube, colors.accent).pixmap(30, 30));
        avatar->setFixedSize(36, 36);
        header->addWidget(avatar); header->addSpacing(10);
        header->addWidget(label(QStringLiteral("Claude Code"), colors, this, false, true));
        header->addWidget(label(timeLabel(entry.timestampMs, true), colors, this, true));
        header->addStretch();
        m_layout->addLayout(header);
    } else if (row.kind == Row::ProcessHeading) {
        m_layout->setContentsMargins(46, 3, 0, 5);
        auto *bar = new QHBoxLayout;
        auto *toggle = textButton(tr("执行过程"), colors, this);
        toggle->setObjectName(QStringLiteral("sessionTurnToggle_%1").arg(turn.firstEntry));
        const bool collapsed = state.collapsedTurns.contains(turn.firstEntry);
        toggle->setCheckable(true); toggle->setChecked(!collapsed);
        toggle->setToolTip(collapsed ? tr("展开本轮执行过程") : tr("收起本轮执行过程"));
        toggle->setIcon(icon(collapsed ? Glyph::Next : Glyph::Chevron, colors.muted));
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        auto font = toggle->font(); font.setBold(true); toggle->setFont(font);
        connect(toggle, &QToolButton::clicked, this, [this] { emit actionRequested(ToggleTurn, m_turn.firstEntry, {}); });
        bar->addWidget(toggle);
        QString summary = tr("%1 步").arg(turn.steps);
        if (turn.commands) summary += tr("  ·  %1 个命令").arg(turn.commands);
        if (turn.changedFiles) summary += tr("  ·  %1 个文件已改动").arg(turn.changedFiles);
        auto *stats = new ElidedLabel(summary, this);
        stats->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(css(colors.muted)));
        bar->addWidget(stats, 1);
        if (turn.failures)
            bar->addWidget(label(tr("%1 次失败").arg(turn.failures), Colors{colors.canvas, colors.surface, colors.raised, colors.code, colors.error, colors.error}, this));
        m_layout->addLayout(bar);
    } else if (row.kind == Row::EmptyReply) {
        m_layout->setContentsMargins(49, 10, 0, 28);
        m_layout->addWidget(label(tr("本轮未记录最终回复"), colors, this, true));
    } else {
        buildStep(entry);
    }
}

int SessionRowWidget::preferredHeight(int width)
{
    return qMax(24, m_layout->hasHeightForWidth() ? m_layout->totalHeightForWidth(width) : m_layout->totalSizeHint().height());
}
void SessionRowWidget::paintEvent(QPaintEvent *)
{
    using namespace sessionui;
    if (m_row.kind != Row::ProcessEntry)
        return;
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_colors.border, 1));
    p.drawLine(QPointF(48, 0), QPointF(48, height()));
    const auto &entry = m_data->entries()[m_row.entry];
    const QColor dot = entry.kind == ClaudeSessionEntry::Tool
        ? (entry.failed ? m_colors.error : entry.hasResult ? m_colors.success : m_colors.muted) : m_colors.muted;
    p.setPen(Qt::NoPen); p.setBrush(m_colors.canvas); p.drawEllipse(QPointF(48, 21), 7, 7);
    p.setBrush(dot); p.drawEllipse(QPointF(48, 21), 3.5, 3.5);
}
void SessionRowWidget::buildMessage(const ClaudeSessionEntry &entry, bool user)
{
    using namespace sessionui;
    m_layout->setContentsMargins(user ? 0 : 49, user ? 24 : 6, 0, user ? 2 : 24);
    QVBoxLayout *bodyLayout = m_layout;
    if (user) {
        auto *align = new QHBoxLayout;
        align->setContentsMargins(0, 0, 0, 0);
        align->addStretch(1);
        auto *column = new QWidget(this); column->setObjectName(QStringLiteral("sessionContent"));
        column->setMaximumWidth(720);
        auto *columnLayout = new QVBoxLayout(column); columnLayout->setContentsMargins(0, 0, 0, 0); columnLayout->setSpacing(8);
        auto *header = new QHBoxLayout; header->addStretch();
        header->addWidget(label(tr("你  ·  %1").arg(timeLabel(entry.timestampMs, true)), m_colors, column, true));
        auto *copy = iconButton(Glyph::Copy, tr("复制提问"), m_colors, column);
        connect(copy, &QToolButton::clicked, this, [this, text = entry.text] { QApplication::clipboard()->setText(text); emit copied(); });
        header->addWidget(copy); columnLayout->addLayout(header);
        auto *bubble = new Surface(m_colors.user, m_colors.userBorder, column);
        bubble->setObjectName(QStringLiteral("sessionSurface"));
        bodyLayout = new QVBoxLayout(bubble); bodyLayout->setContentsMargins(20, 15, 20, 15); bodyLayout->setSpacing(8);
        columnLayout->addWidget(bubble);
        align->addWidget(column, 5);
        m_layout->addLayout(align);
    }
    // 默认隐藏用户附加上下文并截取长消息用于预览；展开 / 复制始终能取得未裁剪原文。
    const QString cleaned = user ? ClaudeSessionData::cleanMessage(entry.text) : entry.text;
    const bool rawMessage = m_state.rawMessages.contains(m_row.entry);
    const QString clean = rawMessage ? entry.text : cleaned;
    const bool full = m_state.fullMessages.contains(m_row.entry);
    addMarkdown(bodyLayout, full ? clean : clean.left(6000), m_row.entry);
    if (clean.size() > 6000) {
        auto *more = textButton(full ? tr("收起长消息") : tr("展开完整消息 · %1 字符").arg(clean.size()), m_colors, this);
        connect(more, &QToolButton::clicked, this, [this] { emit actionRequested(ToggleMessage, m_row.entry, {}); });
        bodyLayout->addWidget(more, 0, Qt::AlignLeft);
    }
    if (user && cleaned != entry.text) {
        auto *raw = textButton(rawMessage ? tr("收起附加上下文") : tr("附加上下文已收起 · 查看原始消息"), m_colors, this);
        raw->setObjectName(QStringLiteral("sessionRawToggle_%1").arg(m_row.entry));
        raw->setCheckable(true); raw->setChecked(rawMessage);
        connect(raw, &QToolButton::clicked, this, [this] { emit actionRequested(ToggleRaw, m_row.entry, {}); });
        bodyLayout->addWidget(raw, 0, Qt::AlignLeft);
    }
    if (!user) {
        auto *copy = iconButton(Glyph::Copy, tr("复制回复"), m_colors, this);
        connect(copy, &QToolButton::clicked, this, [this, text = entry.text] { QApplication::clipboard()->setText(text); emit copied(); });
        bodyLayout->addWidget(copy, 0, Qt::AlignLeft);
    }
}
// 普通段落交给富文本浏览器，围栏代码拆成可复制 / 展开的原生代码卡片。
// 代码块键包含原条目与块序号，滚动回收或搜索展开后仍能恢复对应状态。
void SessionRowWidget::addMarkdown(QVBoxLayout *layout, const QString &text, int entry)
{
    QString prose;
    int blockIndex = 0;
    const auto flush = [&] {
        if (prose.trimmed().isEmpty()) return;
        auto *browser = richText(ClaudeCodeRenderer::toHtml(prose), m_colors, this);
        browser->setObjectName(QStringLiteral("sessionMessage_%1").arg(entry));
        connect(browser, &QTextBrowser::anchorClicked, this, &SessionRowWidget::externalLinkRequested);
        layout->addWidget(browser); prose.clear();
    };
    for (const auto &block : MarkdownRenderer::splitBlocks(text)) {
        if (block.trimmed().startsWith(QLatin1String("```"))) {
            flush();
            auto lines = block.trimmed().split(QLatin1Char('\n'));
            const QString language = lines.takeFirst().mid(3).trimmed();
            if (!lines.isEmpty() && lines.last().trimmed().startsWith(QLatin1String("```"))) lines.removeLast();
            addCode(layout, language.isEmpty() ? tr("代码") : language, lines.join(QLatin1Char('\n')), language,
                    QStringLiteral("%1:fence:%2").arg(entry).arg(blockIndex));
        } else prose += block;
        ++blockIndex;
    }
    flush();
}
// 预览最多取 10 行 / 8000 字符；完整文本仍被复制按钮捕获，不能复制截断后的 preview。
void SessionRowWidget::addCode(QVBoxLayout *layout, const QString &title, const QString &text,
                                const QString &language, const QString &key)
{
    using namespace sessionui;
    const bool full = m_state.expandedCode.contains(key);
    const int lines = text.isEmpty() ? 0 : text.count(QLatin1Char('\n')) + 1;
    QString preview = text;
    if (!full) {
        int end = 0;
        for (int line = 0; line < 10; ++line) {
            end = text.indexOf(QLatin1Char('\n'), end);
            if (end < 0) { end = text.size(); break; }
            ++end;
        }
        preview = text.left(qMin(end, 8000));
        if (preview.size() < text.size() && preview.endsWith(QLatin1Char('\n'))) preview.chop(1);
    }
    auto *card = new Surface(m_colors.code, m_colors.border, this);
    auto *cardLayout = new QVBoxLayout(card); cardLayout->setContentsMargins(10, 5, 10, 6); cardLayout->setSpacing(2);
    auto *bar = new QHBoxLayout;
    auto *heading = new ElidedLabel(title, card);
    heading->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(css(m_colors.muted)));
    bar->addWidget(heading, 1);
    bar->addWidget(label(tr("%1 行").arg(lines), m_colors, card, true));
    auto *copy = iconButton(Glyph::Copy, tr("复制完整内容"), m_colors, card);
    copy->setObjectName(QStringLiteral("sessionCodeCopy_%1").arg(key));
    connect(copy, &QToolButton::clicked, this, [this, text] { QApplication::clipboard()->setText(text); emit copied(); });
    bar->addWidget(copy); cardLayout->addLayout(bar);
    auto *editor = codeView(preview, m_colors, card, language);
    editor->setProperty("codeKey", key);
    editor->setProperty("language", language);
    cardLayout->addWidget(editor);
    if (preview.size() < text.size() || full) {
        auto *more = textButton(full ? tr("收起预览") : tr("显示全部 · %1 行").arg(lines), m_colors, card);
        more->setObjectName(QStringLiteral("sessionCodeExpand_%1").arg(key));
        connect(more, &QToolButton::clicked, this, [this, key] { emit actionRequested(ToggleCode, m_row.entry, key); });
        cardLayout->addWidget(more, 0, Qt::AlignLeft);
    }
    layout->addWidget(card);
}
void SessionRowWidget::buildStep(const ClaudeSessionEntry &entry)
{
    using namespace sessionui;
    m_layout->setContentsMargins(65, 2, 4, 4);
    // expandedEntries 是默认状态的例外集，不能直接用 contains 判断是否展开。
    const bool expanded = m_state.expandAll != m_state.expandedEntries.contains(m_row.entry);
    if (entry.kind == ClaudeSessionEntry::Assistant || entry.kind == ClaudeSessionEntry::Attachment) {
        m_layout->setContentsMargins(65, 9, 4, 10);
        addMarkdown(m_layout, entry.text.left(m_state.fullMessages.contains(m_row.entry) ? entry.text.size() : 6000), m_row.entry);
        return;
    }
    auto *bar = new QHBoxLayout;
    const bool tool = entry.kind == ClaudeSessionEntry::Tool;
    const QString title = tool ? entry.shortToolName() : entry.kind == ClaudeSessionEntry::Thinking ? tr("思考摘要") : tr("系统事件");
    auto *toggle = textButton(title, m_colors, this);
    toggle->setObjectName(QStringLiteral("sessionStepToggle_%1").arg(m_row.entry));
    toggle->setCheckable(true); toggle->setChecked(expanded);
    toggle->setToolTip(expanded ? tr("收起步骤") : tr("展开步骤"));
    auto font = toggle->font(); font.setBold(tool); toggle->setFont(font);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setIcon(icon(expanded ? Glyph::Chevron : Glyph::Next, m_colors.muted));
    connect(toggle, &QToolButton::clicked, this, [this] { emit actionRequested(ToggleEntry, m_row.entry, {}); });
    bar->addWidget(toggle);
    auto *summary = new ElidedLabel(tool ? toolSummary(entry) : entry.text.simplified(), this);
    if (tool) {
        auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (font.pixelSize() > 0) mono.setPixelSize(font.pixelSize()); else mono.setPointSizeF(font.pointSizeF());
        summary->setFont(mono);
    }
    summary->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(css(m_colors.muted)));
    bar->addWidget(summary, 1);
    if (tool) {
        QString status = entry.failed ? tr("失败") : !entry.hasResult ? tr("未记录结果")
            : tr("%1 行").arg(entry.result.isEmpty() ? 0 : entry.result.count(QLatin1Char('\n')) + 1);
        auto *badge = label(status, m_colors, this, !entry.failed);
        if (entry.failed) badge->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(css(m_colors.error)));
        bar->addWidget(badge);
        if (entry.resultTimestampMs >= entry.timestampMs && entry.timestampMs > 0) {
            auto *duration = label(durationLabel(entry.resultTimestampMs - entry.timestampMs), m_colors, this, true);
            duration->setToolTip(tr("工具调用与结果的日志时间间隔")); bar->addWidget(duration);
        }
    }
    m_layout->addLayout(bar);
    if (!expanded) return;
    if (!tool) { addMarkdown(m_layout, entry.text, m_row.entry); return; }
    if (entry.isChange()) {
        if (entry.input.contains(QStringLiteral("old_string"))) {
            addCode(m_layout, tr("修改前 · %1").arg(entry.filePath()), entry.input.value(QStringLiteral("old_string")).toString(), {}, QStringLiteral("%1:before").arg(m_row.entry));
            addCode(m_layout, tr("修改后 · %1").arg(entry.filePath()), entry.input.value(QStringLiteral("new_string")).toString(), {}, QStringLiteral("%1:after").arg(m_row.entry));
        } else if (entry.input.contains(QStringLiteral("content"))) {
            addCode(m_layout, entry.filePath(), entry.input.value(QStringLiteral("content")).toString(), {}, QStringLiteral("%1:content").arg(m_row.entry));
        }
    }
    const bool showParameters = m_state.parameters.contains(m_row.entry);
    auto *parameters = textButton(showParameters ? tr("收起参数") : tr("参数"), m_colors, this);
    parameters->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    parameters->setIcon(icon(showParameters ? Glyph::Chevron : Glyph::Next, m_colors.muted));
    parameters->setCheckable(true); parameters->setChecked(showParameters);
    parameters->setObjectName(QStringLiteral("sessionParameters_%1").arg(m_row.entry));
    connect(parameters, &QToolButton::clicked, this, [this] { emit actionRequested(ToggleParameters, m_row.entry, {}); });
    m_layout->addWidget(parameters, 0, Qt::AlignLeft);
    if (m_state.parameters.contains(m_row.entry))
        addCode(m_layout, tr("输入参数 · JSON"), QString::fromUtf8(QJsonDocument(entry.input).toJson(QJsonDocument::Indented)).trimmed(),
                QStringLiteral("json"), QStringLiteral("%1:input").arg(m_row.entry));
    if (entry.shortToolName() == QLatin1String("Agent") || entry.shortToolName() == QLatin1String("Task")) {
        auto *note = label(tr("主会话中的子代理调用记录，不包含独立子代理日志。"), m_colors, this, true);
        note->setWordWrap(true); m_layout->addWidget(note);
    }
    if (entry.hasResult)
        addCode(m_layout, entry.failed ? tr("工具返回 · 失败") : tr("工具返回"), entry.result, {}, QStringLiteral("%1:output").arg(m_row.entry));
    else m_layout->addWidget(label(tr("日志尚未记录此调用的结果。"), m_colors, this, true));
}
// 将各子控件的匹配位置映射回行坐标，供时间线滚动定位；富文本高亮数量设上限避免卡顿。
QRect SessionRowWidget::highlight(const QString &query)
{
    QRect match;
    if (query.isEmpty()) return match;
    const auto remember = [this, &match](QWidget *viewport, const QRect &rect) {
        const QRect mapped(viewport->mapTo(this, rect.topLeft()), rect.size());
        if (!match.isValid() || mapped.top() < match.top()) match = mapped;
    };
    for (auto *browser : findChildren<QTextBrowser *>()) {
        QList<QTextEdit::ExtraSelection> selections;
        QTextCursor cursor(browser->document());
        while (!(cursor = browser->document()->find(query, cursor)).isNull() && selections.size() < 200) {
            if (selections.isEmpty()) remember(browser->viewport(), browser->cursorRect(cursor));
            QTextEdit::ExtraSelection selection; selection.cursor = cursor;
            selection.format.setBackground(m_colors.accentSoft); selection.format.setForeground(m_colors.accent);
            selections.append(selection);
        }
        browser->setExtraSelections(selections);
    }
    for (auto *editor : findChildren<QPlainTextEdit *>()) {
        QTextCursor start(editor->document()); editor->setTextCursor(start);
        if (editor->find(query)) remember(editor->viewport(), editor->cursorRect());
    }
    return match;
}

QSize SessionDirectoryDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    return {220, qMax(144, option.fontMetrics.lineSpacing() * 6 + 60)};
}
void SessionDirectoryDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    using namespace sessionui;
    const auto c = colors(QApplication::palette());
    const bool active = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    const QRectF card = option.rect.adjusted(5, 4, -5, -4);
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    p->setPen(active ? c.userBorder : Qt::transparent);
    p->setBrush(active ? c.accentSoft : hover ? c.raised : c.surface);
    p->drawRoundedRect(card, 10, 10);
    if (active) { p->setPen(Qt::NoPen); p->setBrush(c.accent); p->drawRoundedRect(QRectF(card.left(), card.top() + 13, 3, card.height() - 26), 1.5, 1.5); }
    const qreal x = card.left() + 15, width = card.width() - 30;
    auto small = option.font; auto bold = option.font; bold.setBold(true);
    if (option.state & QStyle::State_HasFocus) {
        p->setBrush(Qt::NoBrush); p->setPen(QPen(c.accent, 1, Qt::DotLine));
        p->drawRoundedRect(card.adjusted(2, 2, -2, -2), 8, 8);
    }
    const int line = QFontMetrics(option.font).lineSpacing();
    p->setFont(small); p->setPen(c.muted);
    p->drawText(QRectF(x, card.top() + 12, width, line), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("%1   ·   %2").arg(index.row() + 1, 2, 10, QLatin1Char('0')).arg(index.data(TimeRole).toString()));
    const qreal questionY = card.top() + 20 + line;
    wrapped(p, index.data(QuestionRole).toString(), bold, c.text, QRectF(x, questionY, width, line * 2), 2);
    const qreal statsY = questionY + line * 2 + 6;
    p->setFont(small); p->setPen(index.data(FailureRole).toInt() ? c.error : c.muted);
    p->drawText(QRectF(x, statsY, width, line), Qt::AlignVCenter,
                QFontMetrics(small).elidedText(index.data(StatsRole).toString(), Qt::ElideRight, static_cast<int>(width)));
    wrapped(p, index.data(ReplyRole).toString(), small, c.muted, QRectF(x, statsY + line + 8, width, line * 2), 2);
    p->restore();
}
} // namespace cubeshell
