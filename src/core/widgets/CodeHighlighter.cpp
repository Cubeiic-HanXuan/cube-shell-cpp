#include "CodeHighlighter.h"

#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>

namespace cubeshell::sessionui {
CodeHighlighter::CodeHighlighter(QTextDocument *document, const Colors &c,
                                 const QString &language, bool markdown)
    : QSyntaxHighlighter(document), m_language(language.toLower()), m_markdown(markdown)
{
    const bool dark = c.canvas.lightness() < 128;
    m_keyword = dark ? QColor("#c4afff") : QColor("#7040a0");
    m_string = dark ? QColor("#9ad6ae") : QColor("#267346");
    m_number = dark ? QColor("#edbd8b") : QColor("#925513");
    m_comment = c.muted;
    m_symbol = dark ? QColor("#80cddd") : QColor("#166d85");
    m_error = c.error;
}

void CodeHighlighter::highlightBlock(const QString &text)
{
    const auto block = currentBlock();
    const auto format = block.blockFormat();
    setCurrentBlockState(0);
    if (m_markdown && !format.hasProperty(QTextFormat::BlockCodeLanguage)) return;
    const QString lang = (m_markdown ? format.stringProperty(QTextFormat::BlockCodeLanguage) : m_language)
                             .toLower().section(QLatin1Char(' '), 0, 0);
    if (lang == QLatin1String("text") || lang == QLatin1String("plaintext") || lang == QLatin1String("output")) return;
    if (lang == QLatin1String("diff") || lang == QLatin1String("patch")) {
        if (text.startsWith(QLatin1Char('+'))) setFormat(0, text.size(), m_string);
        else if (text.startsWith(QLatin1Char('-'))) setFormat(0, text.size(), m_error);
        else if (text.startsWith(QLatin1String("@@"))) setFormat(0, text.size(), m_symbol);
        return;
    }
    const bool shell = lang == QLatin1String("sh") || lang == QLatin1String("bash")
        || lang == QLatin1String("shell") || lang == QLatin1String("zsh") || lang == QLatin1String("powershell");
    const bool python = lang == QLatin1String("py") || lang == QLatin1String("python");
    const bool sql = lang == QLatin1String("sql");
    const bool config = lang == QLatin1String("yaml") || lang == QLatin1String("yml")
        || lang == QLatin1String("toml") || lang == QLatin1String("ini") || lang == QLatin1String("nginx");
    const bool hashComments = shell || python || config || lang.isEmpty() || lang == QLatin1String("ruby");
    const bool cComments = !shell && !python && !config && !sql && lang != QLatin1String("json");
    static const QSet<QString> keywords = {
        "class", "struct", "enum", "public", "private", "protected", "static", "const", "constexpr",
        "return", "if", "else", "elif", "for", "while", "do", "done", "then", "fi", "case", "esac",
        "def", "import", "from", "function", "let", "var", "void", "int", "bool", "auto", "namespace",
        "using", "package", "new", "delete", "async", "await", "try", "catch", "except", "finally",
        "throw", "raise", "true", "false", "null", "none", "self", "this", "export", "in", "is",
        "not", "and", "or", "as", "with", "yield", "break", "continue", "switch", "include",
        "select", "from", "where", "join", "limit", "order", "by", "group", "having", "insert", "into",
        "values", "update", "set", "create", "table", "drop", "alter", "on", "left", "desc", "asc"
    };
    // Single token pass prevents comment markers inside strings (URLs, shell
    // snippets, JSON) from overriding the string's format.
    static const QRegularExpression tokens(QStringLiteral(
        R"(\"(?:[^\"\\]|\\.)*\"?|'(?:[^'\\]|\\.)*'?|`(?:[^`\\]|\\.)*`?|/\*|//|\#|--|\$\{?\w+\}?|\b(?:0x[\da-fA-F]+|\d+(?:\.\d+)?)\b|[A-Za-z_][\w-]*|-[A-Za-z][\w-]*)"));
    int position = 0;
    int state = previousBlockState();
    if (m_markdown && !block.previous().blockFormat().hasProperty(QTextFormat::BlockCodeLanguage)) state = 0;
    const auto finishMultiline = [&](int start, int contentStart, int multilineState) {
        const QString endToken = multilineState == 1 ? QStringLiteral("*/")
            : multilineState == 2 ? QStringLiteral("'''") : QStringLiteral("\"\"\"");
        const int end = text.indexOf(endToken, contentStart);
        setFormat(start, end < 0 ? text.size() - start : end + endToken.size() - start,
                  multilineState == 1 ? m_comment : m_string);
        if (end < 0) { setCurrentBlockState(multilineState); position = text.size(); }
        else position = end + endToken.size();
    };
    if (state > 0) finishMultiline(0, 0, state);
    while (position < text.size()) {
        const auto match = tokens.match(text, position);
        if (!match.hasMatch()) break;
        const int start = match.capturedStart();
        const QString token = match.captured();
        position = match.capturedEnd();
        if (python && (text.mid(start, 3) == QLatin1String("'''") || text.mid(start, 3) == QLatin1String("\"\"\""))) {
            finishMultiline(start, start + 3, text.at(start) == QLatin1Char('\'') ? 2 : 3);
        } else if (token == QLatin1String("/*") && cComments) {
            finishMultiline(start, position, 1);
        } else if ((token == QLatin1String("//") && cComments)
                   || (token == QLatin1String("#") && hashComments)
                   || (token == QLatin1String("--") && sql)) {
            setFormat(start, text.size() - start, m_comment); break;
        } else if (token.startsWith(QLatin1Char('"')) || token.startsWith(QLatin1Char('\'')) || token.startsWith(QLatin1Char('`'))) {
            int next = position;
            while (next < text.size() && text.at(next).isSpace()) ++next;
            setFormat(start, token.size(), next < text.size() && text.at(next) == QLatin1Char(':') ? m_symbol : m_string);
        } else if (token.at(0).isDigit()) setFormat(start, token.size(), m_number);
        else if (keywords.contains(token.toLower())) setFormat(start, token.size(), m_keyword);
        else if (token.startsWith(QLatin1Char('$')) || token.startsWith(QLatin1Char('-'))
                 || (position < text.size() && (text.at(position) == QLatin1Char('(') || text.at(position) == QLatin1Char(':')))
                 || (shell && text.left(start).trimmed().isEmpty())) setFormat(start, token.size(), m_symbol);
    }
}
} // namespace cubeshell::sessionui
