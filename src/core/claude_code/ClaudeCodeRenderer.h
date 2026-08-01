#pragma once

// ClaudeCodeRenderer.h — lightweight Markdown → HTML renderer for the Claude
// Code chat panel.
// 对应Python: Python 侧无独立渲染模块(消息直接进 QTextEdit);此为 C++ 移植
// 契约要求的简化版内联实现,不复用 Phase 4 的 ai/MarkdownRenderer(并行开发)。
//
// Supported subset:
//   - fenced code blocks ``` / ```lang  (rendered as <pre> with dark theme)
//   - inline code `...`
//   - headings # .. ######
//   - bold **x** / italic *x*
//   - unordered lists (- / *) and ordered lists (1.)
//   - links [text](url)
//   - HTML escaping for everything else

#include <QString>

namespace cubeshell {

class ClaudeCodeRenderer {
public:
    // Convert a Markdown fragment to HTML suitable for QTextBrowser.
    static QString toHtml(const QString &markdown);

    // Escape &, <, >, " for safe HTML embedding.
    static QString escapeHtml(const QString &text);

private:
    // Inline-level formatting (code/bold/italic/links) on an escaped line.
    static QString renderInline(const QString &escapedLine);
};

} // namespace cubeshell
