#include "MarkdownView.h"
#include "widgets/CodeHighlighter.h"
#include "widgets/ConversationWidgets.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <cmath>

namespace cubeshell {
using namespace sessionui;
namespace {
struct SourceFence { QString language; QStringList lines; };
// Qt retains code language, but not the boundary between adjacent fences of
// the same language. Recover those boundaries from source, then validate each
// candidate against Qt's parsed blocks. Qt still owns all Markdown semantics.
QList<SourceFence> sourceFences(const QString &text)
{
    static const QRegularExpression opening(QStringLiteral("^( {0,3})(`{3,}|~{3,})(.*)$"));
    QList<SourceFence> result;
    QString delimiter;
    int indent = 0;
    const auto lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (delimiter.isEmpty()) {
            const auto match = opening.match(line);
            if (!match.hasMatch()) continue;
            const QString info = match.captured(3).trimmed();
            if (match.captured(2).startsWith(QLatin1Char('`')) && info.contains(QLatin1Char('`'))) continue;
            delimiter = match.captured(2); indent = match.captured(1).size();
            result.append({info, {}});
        } else {
            const auto match = opening.match(line);
            if (match.hasMatch() && match.captured(2).front() == delimiter.front()
                && match.captured(2).size() >= delimiter.size() && match.captured(3).trimmed().isEmpty()) {
                delimiter.clear(); continue;
            }
            int remove = 0;
            while (remove < indent && remove < line.size() && line.at(remove) == QLatin1Char(' ')) ++remove;
            if (i < lines.size() - 1 || !line.isEmpty()) result.last().lines.append(line.mid(remove));
        }
    }
    return result;
}
}

MarkdownView::MarkdownView(QWidget *parent) : QTextBrowser(parent)
{
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false); setOpenExternalLinks(false);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto policy = sizePolicy(); policy.setHeightForWidth(true); setSizePolicy(policy);
    setAccessibleName(tr("Markdown 回复，可选择文字；代码块支持单独复制"));
    connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
            QDesktopServices::openUrl(url);
    });
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (!m_rendering) m_heightCache.clear();
    });
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] {
        if (m_rendering) return;
        updateGeometry();
        positionActions();
    });
    connect(this, &QTextBrowser::selectionChanged, this, [this] {
        // Do not destroy the user's selection on every streaming delta. The
        // latest frame is applied once they finish selecting/copying.
        if (!m_rendering && m_hasPending && !textCursor().hasSelection())
            QTimer::singleShot(0, this, [this] {
                if (m_hasPending && !textCursor().hasSelection()) {
                    const auto text = m_pending;
                    const bool streaming = m_pendingStreaming;
                    m_hasPending = false;
                    setMarkdownText(text, streaming);
                }
            });
    });
}

void MarkdownView::setMarkdownText(const QString &text, bool streaming)
{
    if (textCursor().hasSelection()) {
        m_pending = text; m_pendingStreaming = streaming; m_hasPending = true;
        return;
    }
    m_hasPending = false;
    const bool finalize = m_streaming && !streaming;
    m_streaming = streaming;
    if (!finalize && text == m_rendered && !document()->isEmpty()) return;
    if (streaming && appendPlainTail(text)) return;
    render(text); // structural edits and final replies retain full Markdown semantics
}

bool MarkdownView::appendPlainTail(const QString &text)
{
    if (m_rendered.isEmpty() || !text.startsWith(m_rendered) || text.size() == m_rendered.size()) return false;
    const QString suffix = text.mid(m_rendered.size());
    const QString paragraph = m_rendered.mid(m_rendered.lastIndexOf(QLatin1Char('\n')) + 1);
    // Only an existing literal root paragraph is appendable. Markup, tables,
    // lists, code fences, escapes and newlines go through the real parser.
    static const QRegularExpression syntax(QStringLiteral("[\\n\\r`*_\\[\\]<>!#|\\\\~&:=.(){}+-]"));
    if (paragraph.isEmpty() || syntax.match(paragraph).hasMatch() || syntax.match(suffix).hasMatch()) return false;
    const auto block = document()->lastBlock();
    QTextCursor cursor(block);
    if (block.text() != paragraph || block.textList() || block.blockFormat().headingLevel()
        || cursor.currentFrame() != document()->rootFrame()) return false;
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(suffix);
    m_rendered = text;
    m_heightCache.clear();
    updateGeometry();
    return true;
}

void MarkdownView::render(const QString &text)
{
    m_rendering = true;
    m_heightCache.clear();
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    const bool layoutEnabled = document()->isLayoutEnabled();
    document()->setLayoutEnabled(false);
#endif
    m_rendered = text;
    for (const auto &action : m_actions) delete action.button;
    m_actions.clear(); m_quotes.clear();
    // Highlighter owns block state and must not see intermediate frame edits.
    const auto highlighters = document()->findChildren<QSyntaxHighlighter *>();
    qDeleteAll(highlighters);
    const auto c = colors(palette());
    setStyleSheet(QStringLiteral("QTextBrowser { background:transparent; border:0; padding:0; color:%1; selection-background-color:%2; }")
                      .arg(css(c.text), css(c.accent)));
    document()->setDefaultFont(font());
    document()->setDocumentMargin(2);
    document()->setIndentWidth(fontMetrics().horizontalAdvance(QStringLiteral("MM")));
    document()->setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                     | QTextDocument::MarkdownNoHTML);
    QTextCursor formatting(document());
    formatting.beginEditBlock();
    QList<QPair<int, int>> quotes;
    bool inQuote = false;
    for (auto block = document()->begin(); block.isValid(); block = block.next()) {
        const bool quoted = block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0;
        if (quoted) {
            if (!inQuote) quotes.append({block.position(), block.position() + block.length() - 1});
            else quotes.last().second = block.position() + block.length() - 1;
        }
        inQuote = quoted;
    }
    for (auto it = quotes.crbegin(); it != quotes.crend(); ++it) {
        QTextCursor cursor(document()); cursor.setPosition(it->first); cursor.setPosition(it->second, QTextCursor::KeepAnchor);
        QTextFrameFormat ff; ff.setBackground(c.accentSoft); ff.setPadding(12);
        ff.setTopMargin(8); ff.setBottomMargin(8);
        m_quotes.append(cursor.insertFrame(ff));
    }
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (font().pixelSize() > 0) mono.setPixelSize(font().pixelSize());
    else mono.setPointSizeF(font().pointSizeF());
    const qreal line = fontMetrics().lineSpacing();
    struct CodeRange { int start, end; QString language, text; };
    QList<CodeRange> ranges;
    const auto fences = sourceFences(text);
    int nextFence = 0, remainingLines = -1;
    bool inCode = false;
    for (auto block = document()->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto bf = block.blockFormat();
        const bool code = bf.hasProperty(QTextFormat::BlockCodeLanguage);
        if (code) {
            const QString language = bf.stringProperty(QTextFormat::BlockCodeLanguage);
            if (!inCode || remainingLines == 0 || ranges.last().language != language) {
                remainingLines = -1;
                for (int i = nextFence; i < fences.size(); ++i) {
                    const auto &fence = fences.at(i);
                    if (fence.language != language || fence.lines.isEmpty()) continue;
                    auto candidate = block;
                    bool matches = true;
                    for (const auto &line : fence.lines) {
                        if (!candidate.isValid() || !candidate.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage)
                            || candidate.text() != line) { matches = false; break; }
                        candidate = candidate.next();
                    }
                    if (matches) { remainingLines = fence.lines.size(); nextFence = i + 1; break; }
                }
                ranges.append({block.position(), block.position() + block.length() - 1, language, block.text()});
            } else {
                ranges.last().end = block.position() + block.length() - 1;
                ranges.last().text += QLatin1Char('\n') + block.text();
            }
            if (remainingLines > 0) --remainingLines;
            bf.setNonBreakableLines(false); // long commands wrap, never widen the conversation
            bf.setTopMargin(0); bf.setBottomMargin(0);
            bf.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
            cursor.setBlockFormat(bf);
            cursor.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat cf; cf.setFont(mono); cf.setForeground(c.text);
            cursor.mergeCharFormat(cf);
        } else {
            const int heading = bf.headingLevel();
            bf.setTopMargin(heading ? line * .9 : line * .22);
            bf.setBottomMargin(heading ? line * .4 : line * .5);
            bf.setLineHeight(145, QTextBlockFormat::ProportionalHeight);
            if (block.textList()) { bf.setTopMargin(2); bf.setBottomMargin(line * .25); }
            if (bf.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
                bf.clearBackground(); bf.setLeftMargin(4); bf.setRightMargin(0);
                bf.setTopMargin(0); bf.setBottomMargin(0);
            }
            cursor.setBlockFormat(bf);
            if (heading) {
                QTextCharFormat cf; QFont headingFont = font();
                const double scale = heading == 1 ? 1.5 : heading == 2 ? 1.3 : 1.12;
                if (headingFont.pixelSize() > 0) headingFont.setPixelSize(qRound(headingFont.pixelSize() * scale));
                else headingFont.setPointSizeF(headingFont.pointSizeF() * scale);
                headingFont.setBold(true); cf.setFont(headingFont); cf.setForeground(c.text);
                cursor.select(QTextCursor::BlockUnderCursor); cursor.mergeCharFormat(cf);
            }
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                auto cf = fragment.charFormat();
                bool changed = false;
                if (cf.fontFixedPitch()) { cf.setFont(mono); cf.setBackground(c.code); cf.setForeground(c.accent); changed = true; }
                if (cf.isAnchor()) {
                    const QUrl url(cf.anchorHref());
                    if (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")) {
                        cf.setAnchor(false); cf.setAnchorHref({});
                    } else cf.setForeground(c.accent);
                    cf.setFontUnderline(cf.isAnchor()); changed = true;
                }
                if (changed) {
                    cursor.setPosition(fragment.position()); cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                    cursor.setCharFormat(cf);
                }
            }
        }
        inCode = code;
    }
    // Give GitHub tables a real grid, padded cells and a restrained header tint.
    const auto styleTables = [&](auto &&self, QTextFrame *frame) -> void {
        for (auto *child : frame->childFrames()) {
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                auto tf = table->format(); tf.setBorder(1); tf.setBorderBrush(c.border);
                tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid); tf.setBorderCollapse(true);
                tf.setCellPadding(8); tf.setCellSpacing(0); tf.setTopMargin(8); tf.setBottomMargin(12);
                tf.setWidth(QTextLength(QTextLength::PercentageLength, 100)); table->setFormat(tf);
                for (int row = 0; row < table->rows(); ++row) for (int col = 0; col < table->columns(); ++col) {
                    auto cell = table->cellAt(row, col); auto cf = cell.format().toTableCellFormat();
                    cf.setBorder(1); cf.setBorderBrush(c.border); cf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                    cf.setBackground(row == 0 ? c.raised : c.surface); cell.setFormat(cf);
                }
            }
            self(self, child);
        }
    };
    styleTables(styleTables, document()->rootFrame());
    // Work backwards so inserting a frame/header cannot invalidate later ranges.
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
        QTextCursor cursor(document()); cursor.setPosition(it->start); cursor.setPosition(it->end, QTextCursor::KeepAnchor);
        QTextFrameFormat ff; ff.setBackground(c.code); ff.setBorder(1); ff.setBorderBrush(c.border);
        ff.setBorderStyle(QTextFrameFormat::BorderStyle_Solid); ff.setPadding(12);
        ff.setTopMargin(8); ff.setBottomMargin(12);
        auto *frame = cursor.insertFrame(ff);
        auto header = frame->firstCursorPosition();
        const auto codeFormat = header.blockFormat();
        QTextBlockFormat headerFormat;
        headerFormat.setBottomMargin(10); headerFormat.setRightMargin(fontMetrics().horizontalAdvance(tr("已复制")) + 42);
        headerFormat.setLineHeight(qMax(28, fontMetrics().height() + 10), QTextBlockFormat::MinimumHeight);
        header.setBlockFormat(headerFormat);
        QTextCharFormat label; label.setFont(font()); label.setForeground(c.muted);
        const QString language = it->language.isEmpty() ? QStringLiteral("code") : it->language;
        header.insertText(language.left(32), label);
        QTextCursor anchor = header;
        anchor.setKeepPositionOnInsert(true);
        QTextCharFormat codeFont; codeFont.setFont(mono); codeFont.setForeground(c.text);
        header.insertBlock(codeFormat, codeFont);
        auto *copy = iconButton(Glyph::Copy, tr("复制代码"), c, viewport());
        copy->setObjectName(QStringLiteral("aiCopyCode"));
        copy->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); copy->setText(tr("复制"));
        copy->setFixedSize(copy->fontMetrics().horizontalAdvance(tr("已复制")) + 36, qMax(28, copy->fontMetrics().height() + 8));
        copy->setToolTip(tr("复制 %1 代码（不含语言标签）").arg(language));
        connect(copy, &QToolButton::clicked, copy, [copy, source = it->text, c] {
            QApplication::clipboard()->setText(source);
            copy->setText(tr("已复制")); copy->setIcon(icon(Glyph::Check, c.success));
            QTimer::singleShot(1600, copy, [copy, c] { copy->setText(tr("复制")); copy->setIcon(icon(Glyph::Copy, c.muted)); });
        });
        m_actions.append({anchor, copy}); copy->show();
    }
    // insertFrame creates empty root boundary paragraphs. Keep them as small
    // separators, not full blank text lines on both sides of every code/note.
    for (auto block = document()->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        if (!block.text().isEmpty() || block.textList() || cursor.currentFrame() != document()->rootFrame()) continue;
        QTextBlockFormat bf; bf.setLineHeight(3, QTextBlockFormat::FixedHeight);
        cursor.setBlockFormat(bf);
    }
    formatting.endEditBlock();
    new CodeHighlighter(document(), c, {}, true);
    // Apply synchronously as well: the first paint must already be highlighted.
    for (auto *highlighter : document()->findChildren<QSyntaxHighlighter *>()) highlighter->rehighlight();
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    document()->setLayoutEnabled(layoutEnabled);
#endif
    m_rendering = false;
    updateGeometry(); positionActions();
}

QSize MarkdownView::sizeHint() const { return {300, heightForWidth(300)}; }
int MarkdownView::heightForWidth(int width) const
{
    // Layout probes sizeHint() at 300px even when the visible widget is wider.
    // Never leave that probe width on the live document: it would wrap into
    // extra, clipped lines after the parent has already assigned its height.
    const int measuredWidth = qMax(40, width - 2);
    const auto cached = m_heightCache.constFind(measuredWidth);
    if (cached != m_heightCache.cend()) return cached.value();
    QSignalBlocker blocker(document()->documentLayout());
    const qreal previous = document()->textWidth();
    document()->setTextWidth(measuredWidth);
    const int height = int(std::ceil(document()->documentLayout()->documentSize().height())) + 4;
    document()->setTextWidth(previous);
    if (m_heightCache.size() >= 8) m_heightCache.clear();
    m_heightCache.insert(measuredWidth, height);
    return height;
}
void MarkdownView::positionActions()
{
    for (const auto &action : m_actions) {
        const QRect rect = cursorRect(action.header);
        const auto frameRect = document()->documentLayout()->frameBoundingRect(action.header.currentFrame());
        action.button->move(qMax(0, int(frameRect.right()) - action.button->width() - 10), rect.top());
    }
}
void MarkdownView::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);
    document()->setTextWidth(qMax(40, viewport()->width()));
    positionActions();
}
void MarkdownView::changeEvent(QEvent *event)
{
    QTextBrowser::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        m_heightCache.clear();
        if (!m_rendering) updateGeometry();
    }
}
void MarkdownView::wheelEvent(QWheelEvent *event) { event->ignore(); }
void MarkdownView::paintEvent(QPaintEvent *event)
{
    QTextBrowser::paintEvent(event);
    QPainter painter(viewport()); painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    painter.setPen(QPen(colors(palette()).accent, 3, Qt::SolidLine, Qt::RoundCap));
    for (auto *quote : m_quotes) {
        const auto rect = document()->documentLayout()->frameBoundingRect(quote);
        painter.drawLine(QPointF(rect.left() + 1.5, rect.top() + 4), QPointF(rect.left() + 1.5, rect.bottom() - 4));
    }
}
} // namespace cubeshell
