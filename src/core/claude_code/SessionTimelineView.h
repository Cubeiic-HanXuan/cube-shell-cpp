#pragma once

// SessionTimelineView.h — 变高行的原生会话时间线，负责虚拟化布局与阅读位置保持。
//
// 逻辑行描述完整历史，实际 QWidget 仅覆盖视口及少量预加载区域。
// 行控件回收时保留展开/搜索状态；高度变化通过稳定行键和行内偏移恢复阅读锚点。

#include "claude_code/SessionWidgets.h"
#include <QAbstractScrollArea>
#include <QHash>

namespace cubeshell {

// 控件只负责呈现；needMore 请求由详情页转交后端，滚动布局不执行文件 IO。
class SessionTimelineView : public QAbstractScrollArea {
    Q_OBJECT
public:
    enum Filter { All, Conversation, Changes, Failures };
    explicit SessionTimelineView(QWidget *parent = nullptr);
    // 替换只读快照；complete 表示已读到文件快照末尾，否则可触发 needMore。
    void setSnapshot(const ClaudeSessionSnapshot &snapshot, bool complete);
    void clear();
    // 各筛选模式分别保存工具展开态；改动 / 失败模式首次进入时默认展开。
    void setFilter(Filter filter);
    Filter filter() const { return m_filter; }
    void setStepsExpanded(bool expanded);
    void setProcessesCollapsed(bool collapsed);
    // 搜索定位会同时打开所属执行过程、长消息、参数及命中的代码块。
    void scrollToEntry(int entry, const QString &query = QString());
    void scrollToTurn(int turn);
    void scrollToLatest();
    int visibleWidgetCount() const { return m_widgets.size(); }
    int logicalRowCount() const { return m_rows.size(); }
    int currentTurn() const { return m_currentTurn; }
    const QList<ClaudeSessionTurn> &turns() const { return m_turns; }
    const sessionui::ViewState &viewState() const { return m_state; }
    void applyTheme();

signals:
    void currentTurnChanged(int turn);
    void needMore();
    void copied();
    void externalLinkRequested(const QUrl &url);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    // 锚点使用逻辑行键 + 行内像素偏移，不能使用会随展开操作变化的行号。
    struct Anchor { quint64 key = 0; int offset = 0; };
    Anchor anchor() const;
    void restoreAnchor(const Anchor &anchor);
    void rebuildRows(bool preserveAnchor = true);
    void updatePositions();
    // 用实测高度替换视口附近的估算高度，并销毁远离视口的行控件。
    void refreshVisible();
    void scheduleLayout();
    void clearWidgets();
    void onAction(int action, int entry, const QString &key);
    int rowAt(int position) const;
    int contentWidth() const;
    int estimate(const sessionui::Row &row) const;

    ClaudeSessionSnapshot m_data = std::make_shared<ClaudeSessionData>();
    QList<ClaudeSessionTurn> m_turns;
    QList<sessionui::Row> m_rows;
    QList<int> m_positions; // 逻辑行顶部坐标的前缀和，供二分查找及滚动定位
    QHash<quint64, int> m_heights; // 已实例化行的实测高度；宽度 / 字体 / 内容变化时清除
    QHash<quint64, SessionRowWidget *> m_widgets;
    sessionui::ViewState m_state;
    QHash<int, QPair<bool, QSet<int>>> m_disclosures; // 各筛选模式的默认展开态与例外集合
    sessionui::Colors m_colors;
    QFont m_layoutFont;
    bool m_themePending = false;
    Filter m_filter = All;
    bool m_complete = true;
    bool m_layouting = false;
    bool m_layoutScheduled = false;
    bool m_moreRequested = false; // 每份快照最多发一次续读请求，防止滚动反复触发
    int m_currentTurn = -1;
    int m_contentWidth = 0;
    quint64 m_generation = 0; // 控件重建代次，丢弃已销毁行控件排队的交互操作
};
} // namespace cubeshell
