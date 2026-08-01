// ClaudeCodeRenderer.cpp — see ClaudeCodeRenderer.h for the supported subset.
// 对应Python: 无直接对应(移植契约要求的轻量 Markdown→HTML 内联实现)

#include "claude_code/ClaudeCodeRenderer.h"

#include <QRegularExpression>
#include <QStringList>

namespace cubeshell {

QString ClaudeCodeRenderer::escapeHtml(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}

// 行内格式:先转义再替换,顺序为 行内码 -> 粗体 -> 斜体 -> 链接
QString ClaudeCodeRenderer::renderInline(const QString &escapedLine)
{
    QString out = escapedLine;

    // 行内码 `...`(内部不再做其它替换,用占位符保护)
    static const QRegularExpression reCode(QStringLiteral("`([^`]+)`"));
    QStringList codeSpans;
    QRegularExpressionMatchIterator it = reCode.globalMatch(out);
    // 先收集再替换,避免嵌套替换互相干扰
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        codeSpans.append(m.captured(1));
    }
    for (int i = 0; i < codeSpans.size(); ++i) {
        out.replace(QStringLiteral("`%1`").arg(codeSpans.at(i)),
                    QStringLiteral("\x01%1\x02").arg(i));
    }

    // 粗体 **x**
    static const QRegularExpression reBold(
        QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    out.replace(reBold, QStringLiteral("<b>\\1</b>"));
    // 斜体 *x*
    static const QRegularExpression reItalic(
        QStringLiteral("\\*([^*]+)\\*"));
    out.replace(reItalic, QStringLiteral("<i>\\1</i>"));
    // 链接 [text](url)
    static const QRegularExpression reLink(
        QStringLiteral("\\[([^\\]]+)\\]\\((https?://[^)\\s]+)\\)"));
    out.replace(reLink, QStringLiteral("<a href=\"\\2\">\\1</a>"));

    // 恢复行内码占位符
    for (int i = 0; i < codeSpans.size(); ++i) {
        out.replace(QStringLiteral("\x01%1\x02").arg(i),
                    QStringLiteral("<code style=\"background:#2d2d2d;"
                                   "color:#e6db74;padding:1px 4px;"
                                   "border-radius:3px;\">%1</code>")
                        .arg(codeSpans.at(i)));
    }
    return out;
}

QString ClaudeCodeRenderer::toHtml(const QString &markdown)
{
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QStringList html;
    bool inCodeBlock = false;
    QStringList codeLines;
    QString codeLang;
    bool inList = false;
    bool listOrdered = false;

    const auto closeList = [&]() {
        if (inList) {
            html.append(listOrdered ? QStringLiteral("</ol>")
                                    : QStringLiteral("</ul>"));
            inList = false;
        }
    };

    static const QRegularExpression reHeading(
        QStringLiteral("^(#{1,6})\\s+(.*)$"));
    static const QRegularExpression reUnordered(
        QStringLiteral("^\\s*[-*]\\s+(.*)$"));
    static const QRegularExpression reOrdered(
        QStringLiteral("^\\s*\\d+[.)]\\s+(.*)$"));

    for (const QString &line : lines) {
        // 围栏代码块开/关
        if (line.trimmed().startsWith(QLatin1String("```"))) {
            if (!inCodeBlock) {
                closeList();
                inCodeBlock = true;
                codeLang = line.trimmed().mid(3).trimmed();
                codeLines.clear();
            } else {
                inCodeBlock = false;
                QString label;
                if (!codeLang.isEmpty())
                    label = QStringLiteral(
                        "<div style=\"color:#75715e;font-size:11px;\">%1</div>")
                        .arg(escapeHtml(codeLang));
                html.append(QStringLiteral(
                    "%1<pre style=\"background:#1e1e1e;color:#d4d4d4;"
                    "padding:8px;border-radius:4px;white-space:pre-wrap;\">"
                    "%2</pre>")
                    .arg(label, escapeHtml(codeLines.join(QLatin1Char('\n')))));
            }
            continue;
        }
        if (inCodeBlock) {
            codeLines.append(line);
            continue;
        }

        // 标题
        const QRegularExpressionMatch mh = reHeading.match(line);
        if (mh.hasMatch()) {
            closeList();
            const int level = static_cast<int>(mh.captured(1).size());
            html.append(QStringLiteral("<h%1>%2</h%1>")
                            .arg(level)
                            .arg(renderInline(escapeHtml(mh.captured(2)))));
            continue;
        }

        // 列表
        const QRegularExpressionMatch mu = reUnordered.match(line);
        const QRegularExpressionMatch mo = reOrdered.match(line);
        if (mu.hasMatch() || mo.hasMatch()) {
            const bool ordered = mo.hasMatch() && !mu.hasMatch();
            if (!inList || listOrdered != ordered) {
                closeList();
                inList = true;
                listOrdered = ordered;
                html.append(ordered ? QStringLiteral("<ol>")
                                    : QStringLiteral("<ul>"));
            }
            const QString item = ordered ? mo.captured(1) : mu.captured(1);
            html.append(QStringLiteral("<li>%1</li>")
                            .arg(renderInline(escapeHtml(item))));
            continue;
        }
        closeList();

        // 空行 -> 段落间隔
        if (line.trimmed().isEmpty()) {
            html.append(QStringLiteral("<br/>"));
            continue;
        }

        // 普通段落
        html.append(QStringLiteral("<p style=\"margin:2px 0;\">%1</p>")
                        .arg(renderInline(escapeHtml(line))));
    }

    // 未闭合的代码块按已闭合处理
    if (inCodeBlock && !codeLines.isEmpty()) {
        html.append(QStringLiteral(
            "<pre style=\"background:#1e1e1e;color:#d4d4d4;padding:8px;"
            "border-radius:4px;white-space:pre-wrap;\">%1</pre>")
            .arg(escapeHtml(codeLines.join(QLatin1Char('\n')))));
    }
    closeList();
    return html.join(QLatin1Char('\n'));
}

} // namespace cubeshell
