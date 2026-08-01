#include "CodeEditor.h"

#include <QPainter>
#include <QTextBlock>

namespace cubeshell {

// ---------------------------------------------------------------------------
// LineNumberArea
// ---------------------------------------------------------------------------

// 对应Python: ui/code_editor.py::LineNumberArea.__init__
LineNumberArea::LineNumberArea(CodeEditor *editor)
    : QWidget(editor)
    , m_editor(editor)
{
}

QSize LineNumberArea::sizeHint() const
{
    return QSize(m_editor->lineNumberAreaWidth(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent *event)
{
    m_editor->lineNumberAreaPaintEvent(event);
}

// ---------------------------------------------------------------------------
// Highlighter
// ---------------------------------------------------------------------------

// 对应Python: ui/code_editor.py::Highlighter.__init__
Highlighter::Highlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    // Keyword format
    QTextCharFormat keywordFormat;
    keywordFormat.setForeground(QColor(QStringLiteral("#ff79c6"))); // Pink
    keywordFormat.setFontWeight(QFont::Bold);
    const QStringList keywords = {
        QStringLiteral("class"), QStringLiteral("const"), QStringLiteral("def"),
        QStringLiteral("delete"), QStringLiteral("elif"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("except"), QStringLiteral("explicit"),
        QStringLiteral("export"), QStringLiteral("extends"), QStringLiteral("false"),
        QStringLiteral("finally"), QStringLiteral("for"), QStringLiteral("from"),
        QStringLiteral("function"), QStringLiteral("if"), QStringLiteral("implements"),
        QStringLiteral("import"), QStringLiteral("in"), QStringLiteral("instanceof"),
        QStringLiteral("interface"), QStringLiteral("let"), QStringLiteral("new"),
        QStringLiteral("null"), QStringLiteral("package"), QStringLiteral("private"),
        QStringLiteral("protected"), QStringLiteral("public"), QStringLiteral("return"),
        QStringLiteral("static"), QStringLiteral("super"), QStringLiteral("switch"),
        QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("true"),
        QStringLiteral("try"), QStringLiteral("typeof"), QStringLiteral("var"),
        QStringLiteral("void"), QStringLiteral("while"), QStringLiteral("with"),
        QStringLiteral("yield"), QStringLiteral("async"), QStringLiteral("await"),
    };
    for (const QString &kw : keywords)
        m_rules.append({QRegularExpression(QStringLiteral("\\b%1\\b").arg(kw)), keywordFormat});

    // String format
    QTextCharFormat stringFormat;
    stringFormat.setForeground(QColor(QStringLiteral("#f1fa8c"))); // Yellow
    m_rules.append({QRegularExpression(QStringLiteral("\".*\"")), stringFormat});
    m_rules.append({QRegularExpression(QStringLiteral("'.*'")), stringFormat});

    // Function format
    QTextCharFormat functionFormat;
    functionFormat.setForeground(QColor(QStringLiteral("#50fa7b"))); // Green
    m_rules.append({QRegularExpression(QStringLiteral("\\b[A-Za-z0-9_]+(?=\\()")), functionFormat});

    // Self format
    QTextCharFormat selfFormat;
    selfFormat.setForeground(QColor(QStringLiteral("#ff5555"))); // Red
    selfFormat.setFontWeight(QFont::Bold);
    m_rules.append({QRegularExpression(QStringLiteral("\\bself\\b")), selfFormat});

    // Number format
    QTextCharFormat numberFormat;
    numberFormat.setForeground(QColor(QStringLiteral("#bd93f9"))); // Purple
    m_rules.append({QRegularExpression(QStringLiteral("\\b\\d+\\b")), numberFormat});

    // 注释格式最后应用（覆盖注释内被高亮的关键字）。
    m_commentFormat.setForeground(QColor(QStringLiteral("#6272a4"))); // Gray/Blue
}

// 对应Python: Highlighter.highlightBlock
void Highlighter::highlightBlock(const QString &text)
{
    // 1. 先应用所有语法高亮规则（关键字、字符串、数字等）
    for (const Rule &rule : m_rules)
        applyRule(text, rule.pattern, rule.format);

    // 2. 最后应用注释规则，覆盖掉之前可能被高亮的关键字等
    applyRule(text, QRegularExpression(QStringLiteral("#[^\n]*")), m_commentFormat);
    applyRule(text, QRegularExpression(QStringLiteral("//[^\n]*")), m_commentFormat);
}

// 对应Python: Highlighter.applyRule
void Highlighter::applyRule(const QString &text, const QRegularExpression &expr,
                            const QTextCharFormat &format)
{
    QRegularExpressionMatch match = expr.match(text);
    while (match.hasMatch()) {
        const int index = int(match.capturedStart());
        const int length = int(match.capturedLength());
        if (length <= 0)
            break;
        setFormat(index, length, format);
        match = expr.match(text, index + length);
    }
}

// ---------------------------------------------------------------------------
// CodeEditor
// ---------------------------------------------------------------------------

// 对应Python: ui/code_editor.py::CodeEditor.__init__
CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_lineNumberArea = new LineNumberArea(this);
    m_highlighter = new Highlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged,
            this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest,
            this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged,
            this, &CodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();

    QFont font;
    font.setFamily(QStringLiteral("Consolas"));
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSize(14);
    setFont(font);

    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);

    m_bracketFormat.setForeground(QColor(QStringLiteral("#ff79c6")));
}

// 对应Python: CodeEditor.lineNumberAreaWidth
int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int maxNum = qMax(1, blockCount());
    while (maxNum >= 10) {
        maxNum /= 10;
        ++digits;
    }
    return 3 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

// 对应Python: CodeEditor.highlightCurrentLine
void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> extraSelections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        QColor lineColor(QStringLiteral("#44475a")); // Dracula selection
        lineColor.setAlpha(50);
        selection.format.setBackground(lineColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }
    matchBrackets(extraSelections);
    setExtraSelections(extraSelections);
}

// 对应Python: CodeEditor.matchBrackets
void CodeEditor::matchBrackets(QList<QTextEdit::ExtraSelection> &selections)
{
    const QTextCursor cursor = textCursor();
    const QString text = cursor.block().text();
    const int pos = cursor.positionInBlock();

    if (pos > 0 && pos <= text.length()) {
        const QChar ch = text.at(pos - 1);
        if (QStringLiteral(")]}").contains(ch))
            matchLeftBracket(cursor, ch, selections);
        else if (QStringLiteral("([{").contains(ch))
            matchRightBracket(cursor, ch, selections);
    }
}

// 对应Python: CodeEditor.matchLeftBracket
void CodeEditor::matchLeftBracket(const QTextCursor &cursor, QChar rightBracket,
                                  QList<QTextEdit::ExtraSelection> &selections)
{
    QChar leftBracket;
    if (rightBracket == QLatin1Char(')'))
        leftBracket = QLatin1Char('(');
    else if (rightBracket == QLatin1Char(']'))
        leftBracket = QLatin1Char('[');
    else
        leftBracket = QLatin1Char('{');

    const QString text = document()->toPlainText();
    int pos = cursor.position() - 1;
    int count = 1;

    while (pos > 0) {
        --pos;
        const QChar ch = text.at(pos);
        if (ch == rightBracket) {
            ++count;
        } else if (ch == leftBracket) {
            --count;
            if (count == 0) {
                createBracketSelection(pos, selections);
                createBracketSelection(cursor.position() - 1, selections);
                break;
            }
        }
    }
}

// 对应Python: CodeEditor.matchRightBracket
void CodeEditor::matchRightBracket(const QTextCursor &cursor, QChar leftBracket,
                                   QList<QTextEdit::ExtraSelection> &selections)
{
    QChar rightBracket;
    if (leftBracket == QLatin1Char('('))
        rightBracket = QLatin1Char(')');
    else if (leftBracket == QLatin1Char('['))
        rightBracket = QLatin1Char(']');
    else
        rightBracket = QLatin1Char('}');

    const QString text = document()->toPlainText();
    int pos = cursor.position();
    int count = 1;
    const int limit = text.length();

    while (pos < limit) {
        const QChar ch = text.at(pos);
        if (ch == leftBracket) {
            ++count;
        } else if (ch == rightBracket) {
            --count;
            if (count == 0) {
                createBracketSelection(pos, selections);
                createBracketSelection(cursor.position() - 1, selections);
                break;
            }
        }
        ++pos;
    }
}

// 对应Python: CodeEditor.createBracketSelection
void CodeEditor::createBracketSelection(int pos, QList<QTextEdit::ExtraSelection> &selections)
{
    QTextEdit::ExtraSelection selection;
    selection.format = m_bracketFormat;
    selection.cursor = textCursor();
    selection.cursor.setPosition(pos);
    selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    selections.append(selection);
}

// 对应Python: CodeEditor.lineNumberAreaPaintEvent
void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), QColor(QStringLiteral("#282a36"))); // Background

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = int(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + int(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(QColor(QStringLiteral("#6272a4"))); // Line number color
            painter.setFont(font());
            painter.drawText(0, top, m_lineNumberArea->width(), fontMetrics().height(),
                             Qt::AlignRight, QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + int(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

// 对应Python: CodeEditor.find_text
bool CodeEditor::findText(const QString &text, bool regex, bool caseSensitive, bool backward)
{
    QTextDocument::FindFlags flags;
    if (caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (backward)
        flags |= QTextDocument::FindBackward;

    QRegularExpression reg;
    if (regex) {
        reg = QRegularExpression(text);
        if (!caseSensitive)
            reg.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    }

    QTextCursor found = regex ? document()->find(reg, textCursor(), flags)
                              : document()->find(text, textCursor(), flags);

    // 未找到：从头/尾循环再查一次（wrap around）。
    if (found.isNull()) {
        QTextCursor tempCursor = textCursor();
        tempCursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        found = regex ? document()->find(reg, tempCursor, flags)
                      : document()->find(text, tempCursor, flags);
    }

    if (!found.isNull()) {
        setTextCursor(found);
        return true;
    }
    return false;
}

// 对应Python: CodeEditor.replace_text
bool CodeEditor::replaceText(const QString &text, const QString &newText,
                             bool regex, bool caseSensitive)
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection() && (cursor.selectedText() == text || regex)) {
        cursor.insertText(newText);
        return true;
    }
    return findText(text, regex, caseSensitive);
}

// 对应Python: CodeEditor.replace_all
int CodeEditor::replaceAll(const QString &text, const QString &newText,
                           bool regex, bool caseSensitive)
{
    int count = 0;
    QTextCursor editCursor = textCursor();
    editCursor.beginEditBlock();

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);
    setTextCursor(cursor);

    QTextDocument::FindFlags flags;
    if (caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;

    QRegularExpression reg;
    if (regex) {
        reg = QRegularExpression(text);
        if (!caseSensitive)
            reg.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    }

    // 直接用 document()->find() 从当前光标向后找（不循环，避免死循环）。
    while (true) {
        QTextCursor found = regex ? document()->find(reg, textCursor(), flags)
                                  : document()->find(text, textCursor(), flags);
        if (found.isNull())
            break;
        setTextCursor(found);
        textCursor().insertText(newText);
        ++count;
        if (count > 100000)  // 安全上限（如替换空串）
            break;
    }

    editCursor.endEditBlock();
    return count;
}

} // namespace cubeshell
