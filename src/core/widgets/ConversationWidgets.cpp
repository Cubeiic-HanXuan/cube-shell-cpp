// SessionWidgets.cpp — 原生消息气泡、执行步骤、只读代码块及目录卡片的绘制。
// 显示文本与复制原文分离；外链只上报请求，不在富文本控件内自动加载资源。

#include "widgets/ConversationWidgets.h"

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
namespace {
QColor mix(const QColor &a, const QColor &b, double amount)
{
    return QColor::fromRgbF(a.redF() * (1 - amount) + b.redF() * amount,
                           a.greenF() * (1 - amount) + b.greenF() * amount,
                           a.blueF() * (1 - amount) + b.blueF() * amount);
}
void drawGlyph(QPainter *p, Glyph glyph)
{
    const auto line = [p](qreal x1, qreal y1, qreal x2, qreal y2) { p->drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    switch (glyph) {
    case Glyph::Back: line(14, 4, 7, 10); line(7, 10, 14, 16); line(7, 10, 18, 10); break;
    case Glyph::Previous: line(12, 5, 7, 10); line(7, 10, 12, 15); break;
    case Glyph::Next: line(8, 5, 13, 10); line(13, 10, 8, 15); break;
    case Glyph::Chevron: line(6, 8, 10, 12); line(10, 12, 14, 8); break;
    case Glyph::Down: line(10, 3, 10, 16); line(5, 11, 10, 16); line(10, 16, 15, 11); break;
    case Glyph::Search: p->drawEllipse(QRectF(3, 3, 10, 10)); line(12, 12, 17, 17); break;
    case Glyph::Copy: p->drawRoundedRect(QRectF(7, 7, 10, 10), 2, 2); line(12, 3, 4, 3); line(4, 3, 3, 4); line(3, 4, 3, 12); break;
    case Glyph::Folder: { QPainterPath path; path.moveTo(2, 5); path.lineTo(8, 5); path.lineTo(10, 7); path.lineTo(18, 7); path.lineTo(18, 16); path.lineTo(2, 16); path.closeSubpath(); p->drawPath(path); break; }
    case Glyph::Download: line(10, 2, 10, 12); line(6, 9, 10, 13); line(10, 13, 14, 9); line(3, 13, 3, 17); line(3, 17, 17, 17); line(17, 17, 17, 13); break;
    case Glyph::Refresh: p->drawArc(QRectF(3, 3, 14, 14), 40 * 16, 280 * 16); line(17, 3, 17, 8); line(17, 8, 12, 8); break;
    case Glyph::Sidebar: p->drawRoundedRect(QRectF(2, 3, 16, 14), 2, 2); line(12, 3, 12, 17); break;
    case Glyph::More: p->setBrush(p->pen().color()); for (int x : {4, 10, 16}) p->drawEllipse(QPointF(x, 10), .7, .7); break;
    case Glyph::Play: { QPainterPath path; path.moveTo(6, 3); path.lineTo(16, 10); path.lineTo(6, 17); path.closeSubpath(); p->drawPath(path); break; }
    case Glyph::Check: line(4, 10, 8, 14); line(8, 14, 16, 5); break;
    case Glyph::Close: line(5, 5, 15, 15); line(5, 15, 15, 5); break;
    case Glyph::Expand: line(6, 7, 10, 3); line(10, 3, 14, 7); line(6, 13, 10, 17); line(10, 17, 14, 13); break;
    case Glyph::Collapse: line(6, 3, 10, 7); line(10, 7, 14, 3); line(6, 17, 10, 13); line(10, 13, 14, 17); break;
    case Glyph::Filter: line(3, 5, 17, 5); line(6, 10, 14, 10); line(8, 15, 12, 15); break;
    case Glyph::Cube: { QPolygonF shape; shape << QPointF(10, 2) << QPointF(17, 6) << QPointF(17, 14) << QPointF(10, 18) << QPointF(3, 14) << QPointF(3, 6) << QPointF(10, 2); p->drawPolyline(shape); line(3, 6, 10, 10); line(17, 6, 10, 10); line(10, 10, 10, 18); break; }
    }
}
class GlyphEngine : public QIconEngine {
public:
    GlyphEngine(Glyph glyph, QColor color) : m_glyph(glyph), m_color(color) {}
    QIconEngine *clone() const override { return new GlyphEngine(m_glyph, m_color); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->translate(rect.topLeft());
        p->scale(rect.width() / 20.0, rect.height() / 20.0);
        auto color = m_color;
        if (mode == QIcon::Disabled) color.setAlpha(75);
        p->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        drawGlyph(p, m_glyph);
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pixmap(size); pixmap.fill(Qt::transparent);
        QPainter p(&pixmap); paint(&p, QRect(QPoint(), size), mode, state); return pixmap;
    }
private:
    Glyph m_glyph;
    QColor m_color;
};
}
Colors colors(const QPalette &palette)
{
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    Colors c;
    c.accent = palette.color(QPalette::Link);
    // 列表、页头 / 页尾和对话画布共用应用底色，不能另设更黑的固定背景。
    c.canvas = palette.color(QPalette::Window);
    c.surface = dark ? QColor("#24272e") : QColor("#ffffff");
    c.raised = dark ? QColor("#2d313a") : QColor("#f0f3f8");
    c.code = dark ? QColor("#21242b") : QColor("#f5f7fa");
    c.text = dark ? QColor("#e5e9f0") : QColor("#283244");
    c.muted = dark ? QColor("#969eae") : QColor("#6b7688");
    c.border = dark ? QColor("#363b45") : QColor("#e1e6ee");
    c.accentSoft = mix(c.canvas, c.accent, dark ? .15 : .09);
    c.user = mix(c.surface, c.accent, dark ? .11 : .07);
    c.userBorder = mix(c.border, c.accent, .26);
    c.success = dark ? QColor("#6fc7a5") : QColor("#247b5c");
    c.error = dark ? QColor("#f08c96") : QColor("#b94052");
    return c;
}
QString css(const QColor &color) { return color.name(QColor::HexRgb); }
QIcon icon(Glyph glyph, const QColor &color) { return QIcon(new GlyphEngine(glyph, color)); }
void styleIconButton(QToolButton *button, const Colors &c)
{
    button->setStyleSheet(QStringLiteral(
        "QToolButton { background:transparent; color:%1; border:1px solid transparent; border-radius:7px; padding:0; }"
        "QToolButton:hover { background:%2; } QToolButton:pressed, QToolButton:checked { background:%3; color:%4; }"
        "QToolButton:focus { border-color:%4; } QToolButton:disabled { background:transparent; }"
        "QToolButton::menu-indicator { image:none; width:0; }")
        .arg(css(c.muted), css(c.raised), css(c.accentSoft), css(c.accent)));
    const auto glyph = button->property("sessionGlyph");
    if (glyph.isValid()) button->setIcon(icon(static_cast<Glyph>(glyph.toInt()), c.muted));
}
QToolButton *iconButton(Glyph glyph, const QString &label, const Colors &c, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setProperty("sessionGlyph", static_cast<int>(glyph));
    button->setFocusPolicy(Qt::StrongFocus);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(32, 32);
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    styleIconButton(button, c);
    return button;
}
} // namespace sessionui

namespace {
using namespace sessionui;
// 富文本仅渲染已提供的字符串，禁用自动外链跳转及资源加载，避免历史内容触发文件 / 网络读取。
class RichText : public QTextBrowser {
public:
    RichText(const QString &text, const Colors &c, QWidget *parent) : QTextBrowser(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setOpenLinks(false); setOpenExternalLinks(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(0);
        document()->setDefaultStyleSheet(QStringLiteral("p { margin-top:3px; margin-bottom:7px; line-height:145%; } a { color:%1; }").arg(css(c.accent)));
        setStyleSheet(QStringLiteral("QTextBrowser { background:transparent; border:0; padding:0; color:%1; selection-background-color:%2; }")
                          .arg(css(c.text), css(c.accent)));
        setHtml(text);
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true); setSizePolicy(policy);
    }
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override { return {300, heightForWidth(300)}; }
    int heightForWidth(int width) const override
    {
        document()->setTextWidth(qMax(40, width - 2));
        return static_cast<int>(std::ceil(document()->documentLayout()->documentSize().height())) + 4;
    }
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

// 轻量只读高亮：后应用的字符串 / 注释规则覆盖关键字颜色，只改变格式，不改写原文。
class CodeHighlighter : public QSyntaxHighlighter {
public:
    CodeHighlighter(QTextDocument *doc, const Colors &c) : QSyntaxHighlighter(doc)
    {
        const bool dark = c.canvas.lightness() < 128;
        const auto rule = [this](const QString &pattern, const QColor &color) {
            QTextCharFormat f; f.setForeground(color); m_rules.append({QRegularExpression(pattern), f});
        };
        rule(QStringLiteral("\\b(class|public|private|protected|static|const|return|if|else|for|while|def|import|from|function|let|var|void|int|bool|auto|namespace|using|package|new|async|await|try|catch|throw|true|false|null|None|self|SELECT|FROM|WHERE|JOIN|LIMIT)\\b"),
             dark ? QColor("#baacf4") : QColor("#7351b5"));
        rule(QStringLiteral("\\b[0-9]+(?:\\.[0-9]+)?\\b"), dark ? QColor("#e5b582") : QColor("#976015"));
        rule(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'"), dark ? QColor("#98c5a3") : QColor("#347750"));
        rule(QStringLiteral("(?:^|\\s)(?://|#).*$"), c.muted);
    }
protected:
    void highlightBlock(const QString &text) override
    {
        for (const auto &rule : m_rules) {
            auto matches = rule.first.globalMatch(text);
            while (matches.hasNext()) {
                const auto m = matches.next();
                setFormat(m.capturedStart(), m.capturedLength(), rule.second);
            }
        }
    }
private:
    QList<QPair<QRegularExpression, QTextCharFormat>> m_rules;
};
class CodeView;
class Gutter : public QWidget {
public:
    explicit Gutter(CodeView *editor);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    CodeView *m_editor;
};
// 只读代码区保留独立滚动与行号；达到滚动边界后把滚轮交还外层时间线。
class CodeView : public QPlainTextEdit {
public:
    CodeView(const QString &text, const Colors &c, QWidget *parent) : QPlainTextEdit(parent), m_colors(c)
    {
        setObjectName(QStringLiteral("sessionCode"));
        setReadOnly(true); setFrameShape(QFrame::NoFrame);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (font().pixelSize() > 0) mono.setPixelSize(font().pixelSize());
        else mono.setPointSizeF(font().pointSizeF());
        setFont(mono);
        setStyleSheet(QStringLiteral("QPlainTextEdit { background:%1; color:%2; border:none; padding:4px 0; selection-background-color:%3; }")
                          .arg(css(c.code), css(c.text), css(c.accent)));
        setPlainText(text);
        m_gutter = new Gutter(this);
        setViewportMargins(gutterWidth(), 0, 8, 0);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] {
            setViewportMargins(gutterWidth(), 0, 8, 0);
            m_gutter->setGeometry(0, 4, gutterWidth(), height() - 8);
        });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &, int dy) {
            if (dy) m_gutter->scroll(0, dy); else m_gutter->update();
        });
        new CodeHighlighter(document(), c);
        const int lines = qMin(18, qMax(1, blockCount()));
        setFixedHeight(lines * fontMetrics().lineSpacing() + 20 + (text.size() > 200 ? 8 : 0));
    }
    int gutterWidth() const { return 20 + fontMetrics().horizontalAdvance(QString::number(qMax(1, blockCount()))); }
    void paintGutter(QPainter &p)
    {
        p.fillRect(m_gutter->rect(), m_colors.code);
        p.setFont(font()); p.setPen(m_colors.muted);
        QTextBlock block = firstVisibleBlock();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        while (block.isValid() && top <= height()) {
            const int blockHeight = qRound(blockBoundingRect(block).height());
            if (block.isVisible())
                p.drawText(0, top, m_gutter->width() - 9, fontMetrics().height(), Qt::AlignRight, QString::number(block.blockNumber() + 1));
            top += blockHeight; block = block.next();
        }
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        m_gutter->setGeometry(0, 4, gutterWidth(), height() - 8);
    }
    void wheelEvent(QWheelEvent *event) override
    {
        const auto *bar = verticalScrollBar();
        if ((event->angleDelta().y() > 0 && bar->value() == bar->minimum())
            || (event->angleDelta().y() < 0 && bar->value() == bar->maximum()))
            event->ignore();
        else QPlainTextEdit::wheelEvent(event);
    }
private:
    Colors m_colors;
    Gutter *m_gutter = nullptr;
};
Gutter::Gutter(CodeView *editor) : QWidget(editor), m_editor(editor) { setAttribute(Qt::WA_OpaquePaintEvent); }
void Gutter::paintEvent(QPaintEvent *) { QPainter painter(this); m_editor->paintGutter(painter); }

}
namespace sessionui {
ElidedLabel::ElidedLabel(const QString &text, QWidget *parent) : QLabel(text, parent)
{ setTextFormat(Qt::PlainText); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); setToolTip(text.left(2000)); }
void ElidedLabel::paintEvent(QPaintEvent *)
{
    QPainter p(this); p.setFont(font()); p.setPen(palette().color(QPalette::WindowText));
    p.drawText(contentsRect(), alignment() | Qt::AlignVCenter,
               fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
}

void styleRichText(QTextBrowser *view, const Colors &c)
{
    view->document()->setDefaultFont(view->font());
    view->document()->setDefaultStyleSheet(QStringLiteral("p { margin-top:3px; margin-bottom:7px; } a { color:%1; }").arg(css(c.accent)));
    view->setStyleSheet(QStringLiteral("QTextBrowser { background:transparent; border:0; padding:0; color:%1; selection-background-color:%2; }").arg(css(c.text), css(c.accent)));
}
QTextBrowser *richText(const QString &html, const Colors &c, QWidget *parent)
{ return new RichText(html, c, parent); }
QPlainTextEdit *codeView(const QString &text, const Colors &c, QWidget *parent)
{ return new CodeView(text, c, parent); }
QToolButton *disclosureButton(const QString &text, const Colors &c, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text); button->setCheckable(true);
    button->setFocusPolicy(Qt::StrongFocus); button->setCursor(Qt::PointingHandCursor);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(16, 16));
    button->setAccessibleName(text);
    styleIconButton(button, c);
    return button;
}
} // namespace sessionui
} // namespace cubeshell
