#pragma once

// CodeEditor.h — 带行号、语法高亮、括号匹配与查找/替换的代码编辑器。
// 对应Python: ui/code_editor.py::CodeEditor / Highlighter / LineNumberArea

#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QPaintEvent;
class QResizeEvent;

namespace cubeshell {

class CodeEditor;

// 对应Python: ui/code_editor.py::LineNumberArea
class LineNumberArea : public QWidget {
    Q_OBJECT
public:
    explicit LineNumberArea(CodeEditor *editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodeEditor *m_editor;
};

// 简单关键字 + 正则规则高亮（Dracula 配色）。
// 对应Python: ui/code_editor.py::Highlighter
class Highlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit Highlighter(QTextDocument *parent = nullptr);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    void applyRule(const QString &text, const QRegularExpression &expr,
                   const QTextCharFormat &format);

    QVector<Rule> m_rules;
    QTextCharFormat m_commentFormat;
};

// 对应Python: ui/code_editor.py::CodeEditor
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    // 查找（支持正则/大小写/反向；未找到时从头/尾循环）。
    // 对应Python: CodeEditor.find_text
    bool findText(const QString &text, bool regex = false,
                  bool caseSensitive = false, bool backward = false);
    // 替换当前选中并定位下一处。对应Python: CodeEditor.replace_text
    bool replaceText(const QString &text, const QString &newText,
                     bool regex = false, bool caseSensitive = false);
    // 全部替换，返回替换次数。对应Python: CodeEditor.replace_all
    int replaceAll(const QString &text, const QString &newText,
                   bool regex = false, bool caseSensitive = false);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

private:
    void matchBrackets(QList<QTextEdit::ExtraSelection> &selections);
    void matchLeftBracket(const QTextCursor &cursor, QChar rightBracket,
                          QList<QTextEdit::ExtraSelection> &selections);
    void matchRightBracket(const QTextCursor &cursor, QChar leftBracket,
                           QList<QTextEdit::ExtraSelection> &selections);
    void createBracketSelection(int pos, QList<QTextEdit::ExtraSelection> &selections);

    LineNumberArea *m_lineNumberArea = nullptr;
    Highlighter *m_highlighter = nullptr;
    QTextCharFormat m_bracketFormat;
};

} // namespace cubeshell
