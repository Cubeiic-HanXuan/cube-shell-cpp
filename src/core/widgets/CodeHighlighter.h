#pragma once

#include "ConversationWidgets.h"
#include <QSyntaxHighlighter>

namespace cubeshell::sessionui {
// Formatting only: source text is never rewritten. Empty language uses a small
// generic lexer; "text" disables highlighting. Markdown mode reads Qt's code
// metadata and never highlights prose or inline code as a program.
class CodeHighlighter : public QSyntaxHighlighter {
public:
    CodeHighlighter(QTextDocument *document, const Colors &colors,
                    const QString &language = {}, bool markdown = false);
protected:
    void highlightBlock(const QString &text) override;
private:
    QString m_language;
    bool m_markdown;
    QColor m_keyword, m_string, m_number, m_comment, m_symbol, m_error;
};
} // namespace cubeshell::sessionui
