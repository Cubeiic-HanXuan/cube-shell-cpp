#pragma once

// MarkdownView.h — AI 回复的原生 Markdown 文档视图。
// 由 Qt 统一解析整个文档，保留列表、表格与引用链接语义；代码复制按钮是可聚焦的原生控件。

#include <QHash>
#include <QTextBrowser>
#include <QTextCursor>

class QToolButton;

namespace cubeshell {
class MarkdownView : public QTextBrowser {
public:
    explicit MarkdownView(QWidget *parent = nullptr);
    // 流式纯文本尾部可增量追加；结构变化或流式结束时重新完整解析。
    // 用户正在选择文本时仅保存最新待处理帧，避免刷新破坏选区和复制操作。
    void setMarkdownText(const QString &text, bool streaming = false);
    bool isStreaming() const { return m_streaming; }
    QSize minimumSizeHint() const override { return {0, 0}; }
    QSize sizeHint() const override;
    int heightForWidth(int width) const override; // 探测后恢复原文档宽度，不能改变实际布局
protected:
    // 禁止 Markdown 图片等隐式读取本地文件/网络；链接点击另按 http/https 白名单处理。
    QVariant loadResource(int, const QUrl &) override { return {}; }
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override; // 将滚轮交给外层对话时间线
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void render(const QString &text);
    bool appendPlainTail(const QString &text); // 仅允许无 Markdown 语法的根段落末尾追加
    void positionActions();
    // 用文档游标锚定代码头，换行/尺寸变化后按实际帧坐标重定位按钮。
    struct CodeAction { QTextCursor header; QToolButton *button; };
    QList<CodeAction> m_actions;
    QList<QTextFrame *> m_quotes; // 当前文档拥有的引用块，重建文档前清空旧指针
    QString m_rendered, m_pending;
    mutable QHash<int, int> m_heightCache; // 当前内容/字体的有限宽度探测缓存，变化后失效
    bool m_hasPending = false;
    bool m_pendingStreaming = false; // 与待处理正文同时保存，不能丢失“流式已结束”信息
    bool m_streaming = false;
    bool m_rendering = false; // 批量编辑文档时抑制中间布局通知，结束后统一定位
};
} // namespace cubeshell
