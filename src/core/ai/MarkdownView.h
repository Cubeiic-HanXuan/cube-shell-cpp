#pragma once

#include <QHash>
#include <QTextBrowser>
#include <QTextCursor>

class QToolButton;

namespace cubeshell {
// A single selectable Markdown document with native, keyboard-accessible code
// actions. Parsing the whole document preserves lists and reference links.
class MarkdownView : public QTextBrowser {
public:
    explicit MarkdownView(QWidget *parent = nullptr);
    void setMarkdownText(const QString &text, bool streaming = false);
    bool isStreaming() const { return m_streaming; }
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override;
    int heightForWidth(int width) const override;
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void render(const QString &text);
    bool appendPlainTail(const QString &text);
    void positionActions();
    struct CodeAction { QTextCursor header; QToolButton *button; };
    QList<CodeAction> m_actions;
    QList<QTextFrame *> m_quotes;
    QString m_rendered, m_pending;
    mutable QHash<int, int> m_heightCache; // current content/font only; bounded width probes
    bool m_hasPending = false;
    bool m_pendingStreaming = false;
    bool m_streaming = false;
    bool m_rendering = false;
};
} // namespace cubeshell
