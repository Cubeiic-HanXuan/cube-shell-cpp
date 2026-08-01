#pragma once

// HermesHighlighters.h — regex-based syntax highlighters for Hermes config
// editors (config.yaml / .env).
// 对应Python: core/hermes/config_highlighter.py（YamlHighlighter /
// DotenvHighlighter — Python 版由 Pygments 驱动，C++ 版用等价的手写正则规则）
//
// Header-only on purpose: shared by HermesConfigWidget and HermesAgentWidget
// without adding another translation unit. No Q_OBJECT — the classes add no
// signals/slots, so AUTOMOC is not required for this header.

#include <QColor>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace cubeshell {

// YAML 高亮：注释灰斜体、键名蓝粗体、字符串绿、数字橙、布尔紫。
class YamlHighlighter : public QSyntaxHighlighter {
public:
    explicit YamlHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        // 规则按声明顺序应用，后面的规则可覆盖前面的着色（注释最后压轴）。
        QTextCharFormat keyFormat;
        keyFormat.setForeground(QColor(QStringLiteral("#569cd6")));
        keyFormat.setFontWeight(QFont::Bold);
        m_rules.append({QRegularExpression(
                            QStringLiteral(R"(^\s*-?\s*[\w.\-]+(?=\s*:))")),
                        keyFormat});

        QTextCharFormat numberFormat;
        numberFormat.setForeground(QColor(QStringLiteral("#d19a66")));
        m_rules.append({QRegularExpression(
                            QStringLiteral(R"(\b-?\d+(\.\d+)?\b)")),
                        numberFormat});

        QTextCharFormat boolFormat;
        boolFormat.setForeground(QColor(QStringLiteral("#c678dd")));
        m_rules.append({QRegularExpression(
                            QStringLiteral(R"(\b(true|false|yes|no)\b)"),
                            QRegularExpression::CaseInsensitiveOption),
                        boolFormat});

        QTextCharFormat stringFormat;
        stringFormat.setForeground(QColor(QStringLiteral("#98c379")));
        m_rules.append({QRegularExpression(
                            QStringLiteral(R"("[^"]*"|'[^']*')")),
                        stringFormat});

        QTextCharFormat commentFormat;
        commentFormat.setForeground(QColor(QStringLiteral("#7f848e")));
        commentFormat.setFontItalic(true);
        m_rules.append({QRegularExpression(QStringLiteral("#[^\n]*")),
                        commentFormat});
    }

protected:
    void highlightBlock(const QString &text) override
    {
        for (const Rule &rule : m_rules) {
            QRegularExpressionMatchIterator it =
                rule.pattern.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                setFormat(int(match.capturedStart()),
                          int(match.capturedLength()), rule.format);
            }
        }
    }

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> m_rules;
};

// .env 高亮：注释灰斜体、KEY 粗体、值保持默认色。
class DotenvHighlighter : public QSyntaxHighlighter {
public:
    explicit DotenvHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        QTextCharFormat keyFormat;
        keyFormat.setFontWeight(QFont::Bold);
        m_rules.append({QRegularExpression(
                            QStringLiteral(R"(^\s*(export\s+)?[A-Za-z_][A-Za-z0-9_]*(?=\s*=))")),
                        keyFormat});

        QTextCharFormat commentFormat;
        commentFormat.setForeground(QColor(QStringLiteral("#7f848e")));
        commentFormat.setFontItalic(true);
        m_rules.append({QRegularExpression(QStringLiteral("^\\s*#[^\n]*")),
                        commentFormat});
    }

protected:
    void highlightBlock(const QString &text) override
    {
        for (const Rule &rule : m_rules) {
            QRegularExpressionMatchIterator it =
                rule.pattern.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                setFormat(int(match.capturedStart()),
                          int(match.capturedLength()), rule.format);
            }
        }
    }

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QVector<Rule> m_rules;
};

} // namespace cubeshell
