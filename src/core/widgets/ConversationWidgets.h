#pragma once

// Small native conversation primitives shared by live AI and saved sessions.
#include <QColor>
#include <QIcon>
#include <QLabel>

class QPlainTextEdit;
class QTextBrowser;
class QToolButton;

namespace cubeshell::sessionui {
struct Colors {
    QColor canvas, surface, raised, code, text, muted, border, accent, accentSoft;
    QColor user, userBorder, success, error;
};
Colors colors(const QPalette &palette);
QString css(const QColor &color);
enum class Glyph { Back, Previous, Next, Search, Copy, Folder, Download, Refresh,
                   Sidebar, More, Chevron, Play, Check, Close, Expand, Collapse, Down, Filter, Cube };
QIcon icon(Glyph glyph, const QColor &color);
QToolButton *iconButton(Glyph glyph, const QString &label, const Colors &colors, QWidget *parent);
void styleIconButton(QToolButton *button, const Colors &colors);

class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString &text, QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *) override;
};

// HTML must already be escaped/rendered by the caller. Resources never load.
QTextBrowser *richText(const QString &html, const Colors &colors, QWidget *parent);
void styleRichText(QTextBrowser *view, const Colors &colors);
QPlainTextEdit *codeView(const QString &text, const Colors &colors, QWidget *parent);
QToolButton *disclosureButton(const QString &text, const Colors &colors, QWidget *parent);
} // namespace cubeshell::sessionui
