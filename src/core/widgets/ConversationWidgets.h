#pragma once

// ConversationWidgets.h — AI 实时对话与历史会话共用的原生展示控件。
// 只负责绘制、布局和可访问性；不持有会话数据、不执行命令、不自动加载外部资源。
#include <QColor>
#include <QIcon>
#include <QLabel>

class QPlainTextEdit;
class QTextBrowser;
class QToolButton;

namespace cubeshell::sessionui {
// 按用途集中定义主题颜色，避免各面板分别硬编码不一致的背景与状态色。
struct Colors {
    QColor canvas, surface, raised, code, text, muted, border, accent, accentSoft;
    QColor user, userBorder, success, error;
};
Colors colors(const QPalette &palette);
QString css(const QColor &color);
// 矢量图标使用统一坐标系缩放，不依赖平台字体是否具有对应符号。
enum class Glyph { Back, Previous, Next, Search, Copy, Folder, Download, Refresh,
                   Sidebar, More, Chevron, Play, Check, Close, Expand, Collapse, Down, Filter, Cube,
                   Send, Microphone, Stop };
QIcon icon(Glyph glyph, const QColor &color);
// label 同时用于提示和可访问名称；按钮支持键盘聚焦，所有权归 parent。
QToolButton *iconButton(Glyph glyph, const QString &label, const Colors &colors, QWidget *parent);
void styleIconButton(QToolButton *button, const Colors &colors);

// 只在绘制时省略右侧文本，QLabel::text 保留完整原文，不用省略后的字符串覆盖数据。
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString &text, QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *) override;
};

// 调用方先转义/渲染 HTML；控件禁止资源加载和自动链接跳转，滚轮交给外层容器。
QTextBrowser *richText(const QString &html, const Colors &colors, QWidget *parent);
void styleRichText(QTextBrowser *view, const Colors &colors);
// 只读代码区带行号与语法高亮，保留原始文本；内部滚动到边界后交还外层时间线。
QPlainTextEdit *codeView(const QString &text, const Colors &colors, QWidget *parent, const QString &language = {});
// 可勾选的展开按钮；是否展示详情、如何持久化展开状态由调用方负责。
QToolButton *disclosureButton(const QString &text, const Colors &colors, QWidget *parent);
} // namespace cubeshell::sessionui
