#include "MarkdownRenderer.h"

#include <QGuiApplication>
#include <QTextDocument>

namespace cubeshell {
namespace {
class MarkdownDocument : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
}

MarkdownRenderer::MarkdownRenderer(int cacheBlocks) : m_cache(cacheBlocks) {}
void MarkdownRenderer::clearCache() { m_cache.clear(); }

QString MarkdownRenderer::render(const QString &markdown)
{
    QString html;
    for (const auto &block : splitBlocks(markdown)) {
        if (const auto *cached = m_cache.object(block)) html += *cached;
        else {
            const QString rendered = renderBlock(block);
            m_cache.insert(block, new QString(rendered));
            html += rendered;
        }
    }
    return html;
}

QString MarkdownRenderer::escapeHtml(const QString &text)
{
    QString escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\''), QLatin1String("&#39;"));
    return escaped;
}

QStringList MarkdownRenderer::splitBlocks(const QString &markdown)
{
    QStringList blocks;
    QString current;
    bool inFence = false;
    int pos = 0;
    while (pos < markdown.size()) {
        const int nl = markdown.indexOf(QLatin1Char('\n'), pos);
        const QString line = nl < 0 ? markdown.mid(pos) : markdown.mid(pos, nl - pos + 1);
        pos = nl < 0 ? markdown.size() : nl + 1;
        const bool fence = line.trimmed().startsWith(QLatin1String("```"));
        if (fence && !inFence && !current.isEmpty()) {
            blocks.append(current);
            current.clear();
        }
        current += line;
        if (fence) {
            if (inFence) { blocks.append(current); current.clear(); }
            inFence = !inFence;
        } else if (!inFence && line.trimmed().isEmpty()) {
            blocks.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty()) blocks.append(current);
    return blocks;
}

QString MarkdownRenderer::renderBlock(const QString &text)
{
    if (text.isEmpty()) return {};
    // Qt's parser protects fenced/inline code from subsequent Markdown passes,
    // handles an unfinished streaming fence and retains GitHub-style tables.
    MarkdownDocument document;
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        document.setDefaultFont(QGuiApplication::font());
    document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                   | QTextDocument::MarkdownNoHTML);
    const QString html = document.toHtml();
    const int body = html.indexOf(QLatin1String("<body"));
    const int start = html.indexOf(QLatin1Char('>'), body) + 1;
    const int end = html.lastIndexOf(QLatin1String("</body>"));
    return start > 0 && end >= start ? html.mid(start, end - start) : QString();
}

} // namespace cubeshell
