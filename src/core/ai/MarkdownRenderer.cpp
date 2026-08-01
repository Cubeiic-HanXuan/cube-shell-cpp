// MarkdownRenderer.cpp — see MarkdownRenderer.h.
//
// 对应Python: core/ai/ai_panel.py::AIChatPanel._render_markdown

#include "MarkdownRenderer.h"

#include <QRegularExpression>

namespace cubeshell {

namespace {

// 代码块 ```lang\n...``` — 对应Python: r"```(?:\w*)\n?(.*?)```"
const QRegularExpression &codeBlockRe()
{
    static const QRegularExpression re(
        QStringLiteral("```(\\w*)\\n?(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    return re;
}

const QRegularExpression &inlineCodeRe()
{
    static const QRegularExpression re(QStringLiteral("`([^`]+)`"));
    return re;
}

const QRegularExpression &boldRe()
{
    static const QRegularExpression re(QStringLiteral("\\*\\*(.+?)\\*\\*"));
    return re;
}

const QRegularExpression &italicRe()
{
    static const QRegularExpression re(QStringLiteral("\\*(.+?)\\*"));
    return re;
}

// [text](url) — 任务要求补充的链接支持（Python 渲染器未覆盖）
const QRegularExpression &linkRe()
{
    static const QRegularExpression re(
        QStringLiteral("\\[([^\\]]+)\\]\\((https?://[^\\s)]+)\\)"));
    return re;
}

const QRegularExpression &tableSepRe()
{
    static const QRegularExpression re(QStringLiteral("^\\|[\\s\\-:|]+\\|$"));
    return re;
}

const QRegularExpression &hrRe()
{
    static const QRegularExpression re(
        QStringLiteral("^(\\-{3,}|\\*{3,}|_{3,})$"));
    return re;
}

const QRegularExpression &headerRe()
{
    static const QRegularExpression re(QStringLiteral("^(#{1,4})\\s+(.+)$"));
    return re;
}

const QRegularExpression &orderedItemRe()
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)\\.\\s+(.+)$"));
    return re;
}

} // namespace

MarkdownRenderer::MarkdownRenderer(int cacheBlocks)
    : m_cache(cacheBlocks)
{
}

void MarkdownRenderer::clearCache()
{
    m_cache.clear();
}

QString MarkdownRenderer::render(const QString &markdown)
{
    const QStringList blocks = splitBlocks(markdown);
    QString html;
    for (const QString &block : blocks) {
        if (const QString *cached = m_cache.object(block)) {
            html += *cached;
            continue;
        }
        const QString rendered = renderBlock(block);
        // QCache takes ownership of the heap copy.
        m_cache.insert(block, new QString(rendered));
        html += rendered;
    }
    return html;
}

// 对应Python: AIChatPanel._escape_html（不含 \n→<br> 部分）
QString MarkdownRenderer::escapeHtml(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return out;
}

QStringList MarkdownRenderer::splitBlocks(const QString &markdown)
{
    // Blocks: fenced code (kept whole so streaming inside a fence只重渲染
    // 该块) and blank-line separated paragraphs. Separators stay attached to
    // the preceding block so concatenation reproduces the original text.
    QStringList blocks;
    if (markdown.isEmpty())
        return blocks;

    QString current;
    bool inFence = false;
    int pos = 0;
    while (pos < markdown.size()) {
        int nl = markdown.indexOf(QLatin1Char('\n'), pos);
        const bool lastLine = (nl < 0);
        const QString line = lastLine
            ? markdown.mid(pos)
            : markdown.mid(pos, nl - pos + 1);   // includes '\n'
        pos = lastLine ? markdown.size() : nl + 1;

        const QString trimmed = QString(line).remove(QLatin1Char('\n')).trimmed();
        const bool isFenceLine = trimmed.startsWith(QLatin1String("```"));

        current += line;
        if (isFenceLine) {
            if (inFence) {
                blocks << current;               // fence closed => flush
                current.clear();
            }
            inFence = !inFence;
        } else if (!inFence && trimmed.isEmpty()) {
            blocks << current;                   // blank line => flush
            current.clear();
        }
    }
    if (!current.isEmpty())
        blocks << current;
    return blocks;
}

// 对应Python: AIChatPanel._build_table_html
QString MarkdownRenderer::buildTableHtml(const QStringList &rows)
{
    if (rows.isEmpty())
        return QString();

    QString html = QStringLiteral(
        "<table style=\"border-collapse:collapse;margin:6px 0;width:100%;"
        "font-size:12px;\">");
    for (int i = 0; i < rows.size(); ++i) {
        QString row = rows.at(i);
        while (row.startsWith(QLatin1Char('|')))
            row.remove(0, 1);
        while (row.endsWith(QLatin1Char('|')))
            row.chop(1);
        const QStringList cells = row.split(QLatin1Char('|'));
        const QString tag = (i == 0) ? QStringLiteral("th") : QStringLiteral("td");
        const QString cellStyle = (i == 0)
            ? QStringLiteral("style=\"border:1px solid rgba(128,128,128,0.3);"
                             "padding:4px 8px;font-weight:bold;"
                             "background:rgba(128,128,128,0.1);\"")
            : QStringLiteral("style=\"border:1px solid rgba(128,128,128,0.3);"
                             "padding:4px 8px;\"");
        html += QLatin1String("<tr>");
        for (const QString &cell : cells) {
            html += QStringLiteral("<%1 %2>%3</%1>")
                        .arg(tag, cellStyle, cell.trimmed());
        }
        html += QLatin1String("</tr>");
    }
    html += QLatin1String("</table>");
    return html;
}

// 对应Python: AIChatPanel._render_markdown
QString MarkdownRenderer::renderBlock(const QString &blockText)
{
    if (blockText.isEmpty())
        return QString();

    // 先转义基础 HTML 字符
    QString text = escapeHtml(blockText);

    // 代码块 ```lang ... ``` — class 标注 + 内联样式（QTextBrowser 需要）
    {
        QString out;
        int last = 0;
        auto it = codeBlockRe().globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += text.mid(last, m.capturedStart() - last);
            const QString lang = m.captured(1);
            const QString cssClass = lang.isEmpty()
                ? QStringLiteral("code")
                : QStringLiteral("code lang-%1").arg(lang);
            out += QStringLiteral(
                "<pre class=\"%1\" style=\"background:#263238;color:#e0e0e0;"
                "padding:8px;border-radius:4px;font-family:Courier New;"
                "font-size:12px;overflow-x:auto;white-space:pre-wrap;"
                "margin:6px 0;\">%2</pre>")
                .arg(cssClass, m.captured(2).trimmed());
            last = m.capturedEnd();
        }
        out += text.mid(last);
        text = out;
    }

    // 行内代码 `...`
    text.replace(inlineCodeRe(), QStringLiteral(
        "<code style=\"background:rgba(128,128,128,0.2);padding:1px 4px;"
        "border-radius:3px;font-family:Courier New;font-size:12px;\">\\1</code>"));

    // 链接 [text](url)
    text.replace(linkRe(), QStringLiteral("<a href=\"\\2\">\\1</a>"));

    // 粗体 / 斜体
    text.replace(boldRe(), QStringLiteral("<b>\\1</b>"));
    text.replace(italicRe(), QStringLiteral("<i>\\1</i>"));

    // 按行处理：标题、表格、列表
    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList resultLines;
    bool inUl = false;
    bool inOl = false;
    bool inTable = false;
    QStringList tableRows;

    const auto closeList = [&]() {
        if (inUl) {
            resultLines << QStringLiteral("</ul>");
            inUl = false;
        }
        if (inOl) {
            resultLines << QStringLiteral("</ol>");
            inOl = false;
        }
    };

    for (const QString &line : lines) {
        const QString stripped = line.trimmed();

        // <pre> 块内容已处理，直接透传
        if (line.contains(QLatin1String("<pre "))
            || line.contains(QLatin1String("</pre>"))) {
            closeList();
            if (inTable) {
                resultLines << buildTableHtml(tableRows);
                tableRows.clear();
                inTable = false;
            }
            resultLines << line;
            continue;
        }

        // 表格行（以 | 开头和结尾）
        if (stripped.startsWith(QLatin1Char('|'))
            && stripped.endsWith(QLatin1Char('|'))) {
            closeList();
            if (tableSepRe().match(stripped).hasMatch()) {
                inTable = true;
                continue;                        // 跳过分隔行 |---|---|
            }
            tableRows << stripped;
            inTable = true;
            continue;
        }
        if (inTable) {
            resultLines << buildTableHtml(tableRows);
            tableRows.clear();
            inTable = false;
        }

        // 水平分隔线 --- *** ___
        if (hrRe().match(stripped).hasMatch()) {
            closeList();
            resultLines << QStringLiteral(
                "<hr style=\"margin:2px 0;border:none;"
                "border-top:1px solid rgba(128,128,128,0.3);\">");
            continue;
        }

        // 标题 # ## ### ####
        const QRegularExpressionMatch header = headerRe().match(stripped);
        if (header.hasMatch()) {
            closeList();
            const int level = header.captured(1).size();
            static const char *sizes[] = {"18px", "16px", "14px", "13px"};
            const QString fontSize = QLatin1String(
                sizes[qBound(1, level, 4) - 1]);
            resultLines << QStringLiteral(
                "<p style=\"font-size:%1;font-weight:bold;"
                "margin:1px 0 1px 0;\">%2</p>")
                .arg(fontSize, header.captured(2));
            continue;
        }

        // 无序列表 - ... / • ...
        if (stripped.startsWith(QLatin1String("- "))
            || stripped.startsWith(QStringLiteral("• "))) {
            if (inOl) {
                resultLines << QStringLiteral("</ol>");
                inOl = false;
            }
            if (!inUl) {
                resultLines << QStringLiteral(
                    "<ul style='margin:1px 0;padding-left:20px;'>");
                inUl = true;
            }
            resultLines << QStringLiteral("<li>%1</li>").arg(stripped.mid(2));
            continue;
        }

        // 有序列表 1. 2. 3.
        const QRegularExpressionMatch ol = orderedItemRe().match(stripped);
        if (ol.hasMatch()) {
            if (inUl) {
                resultLines << QStringLiteral("</ul>");
                inUl = false;
            }
            if (!inOl) {
                resultLines << QStringLiteral(
                    "<ol style='margin:1px 0;padding-left:20px;'>");
                inOl = true;
            }
            resultLines << QStringLiteral("<li>%1</li>").arg(ol.captured(2));
            continue;
        }

        // 普通行
        closeList();
        resultLines << line;
    }

    // 关闭未结束的列表/表格
    closeList();
    if (inTable)
        resultLines << buildTableHtml(tableRows);

    text = resultLines.join(QLatin1Char('\n'));

    // 换行：不在 <pre>/<table>/<ul>/<ol> 块内的换行转为 <br>
    {
        static const QRegularExpression blockRe(
            QStringLiteral("(<pre.*?</pre>|<table.*?</table>|"
                           "<ul.*?</ul>|<ol.*?</ol>)"),
            QRegularExpression::DotMatchesEverythingOption);
        QString out;
        int last = 0;
        auto it = blockRe.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            QString plain = text.mid(last, m.capturedStart() - last);
            plain.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            out += plain + m.captured(1);
            last = m.capturedEnd();
        }
        QString plain = text.mid(last);
        plain.replace(QLatin1Char('\n'), QLatin1String("<br>"));
        out += plain;
        text = out;
    }

    return text;
}

} // namespace cubeshell
