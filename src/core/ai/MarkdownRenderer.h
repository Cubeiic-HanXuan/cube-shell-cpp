#pragma once

// MarkdownRenderer.h — lightweight Markdown -> HTML conversion for the AI
// chat panel (QTextBrowser compatible: inline styles, simple tags).
//
// 对应Python: core/ai/ai_panel.py::AIChatPanel._render_markdown /
//             _build_table_html / _escape_html
//
// Supported syntax (same set as the Python renderer, plus links):
//   headers # ## ### ####, unordered/ordered lists, tables |...|,
//   fenced code blocks ``` (CSS class "code lang-<x>" + inline style),
//   inline code `...`, **bold**, *italic*, [text](url), --- hr.
//
// Incremental rendering: render() splits the source into blocks (fenced code
// blocks + blank-line separated paragraphs) and caches each block's HTML in
// an LRU cache (QCache), so during streaming only the growing tail block is
// re-parsed instead of the whole document.

#include <QCache>
#include <QString>
#include <QStringList>

namespace cubeshell {

class MarkdownRenderer {
public:
    explicit MarkdownRenderer(int cacheBlocks = 256);

    // Block-cached conversion; safe to call repeatedly on a growing text.
    QString render(const QString &markdown);

    void clearCache();

    // --- pure helpers (unit-testable, no cache) ---

    // 对应Python: AIChatPanel._render_markdown（单块全量转换）
    static QString renderBlock(const QString &blockText);

    // 对应Python: AIChatPanel._escape_html 的转义部分（不含 \n→<br>）
    static QString escapeHtml(const QString &text);

    // Split into cacheable blocks: fenced code blocks stay intact, the rest
    // is cut after blank lines; separators (newlines) remain attached so the
    // concatenation of per-block HTML equals the whole-text rendering.
    static QStringList splitBlocks(const QString &markdown);

private:
    // 对应Python: AIChatPanel._build_table_html
    static QString buildTableHtml(const QStringList &rows);

    QCache<QString, QString> m_cache;
};

} // namespace cubeshell
