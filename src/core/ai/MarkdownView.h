#pragma once

#include <QTextBrowser>
#include <QTextCursor>

class QToolButton;

namespace cubeshell {
// A single selectable Markdown document with native, keyboard-accessible code
// actions. Parsing the whole document preserves lists and reference links.
class MarkdownView : public QTextBrowser {
public:
    explicit MarkdownView(QWidget *parent = nullptr);
    void setMarkdownText(const QString &text);
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override;
    int heightForWidth(int width) const override;
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    void render(const QString &text);
    void positionActions();
    struct CodeAction { QTextCursor header; QToolButton *button; };
    QList<CodeAction> m_actions;
    QList<QTextFrame *> m_quotes;
    QString m_rendered, m_pending;
    bool m_hasPending = false;
    bool m_rendering = false;
};
} // namespace cubeshell
