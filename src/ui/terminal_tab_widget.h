#pragma once

// terminal_tab_widget.h — 终端标签组件及其标签装饰件。
// 对应Python: ui/terminal_tab_widget.py::TerminalTabWidget
//            cube-shell.py::TabStatusDot / TabCloseButton

#include <QLabel>
#include <QTabWidget>
#include <QWidget>

class QToolButton;

namespace cubeshell {

// Tab 标题左侧的连接状态圆点（绿=已连接 / 红=未连接）。
// 对应Python: cube-shell.py::TabStatusDot
class TabStatusDot : public QWidget {
    Q_OBJECT
public:
    explicit TabStatusDot(QWidget *parent = nullptr);
    void setConnected(bool connected);

private:
    QLabel *m_dot = nullptr;
};

// Tab 标题右侧的关闭按钮：整体 25x16，✕ 占左侧 16x16，右侧 9px 透明留白。
// 对应Python: cube-shell.py::TabCloseButton
class TabCloseButton : public QWidget {
    Q_OBJECT
public:
    explicit TabCloseButton(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    QLabel *m_closeLabel = nullptr;
};

// 让 "+" 按钮始终紧跟最后一个标签的标签组件。
//
// QTabWidget::setCornerWidget 会把控件钉在最右侧，标签少时 "+" 离最后标签很远；
// 这里改为按最后一个标签的矩形定位悬浮 "+"。但标签溢出（出现 QTabBar 自带的
// 滚动按钮）时，悬浮 "+" 会压住滚动按钮 —— 因此用 tabBar->setMaximumWidth 把
// 标签栏收窄，在右侧预留一块不含滚动按钮的区域：溢出时 "+" 固定到该预留位，
// 未溢出时仍跟随最后一个标签。
// （注：本平台实测 QTabWidget 的角落控件不会挤窄标签栏 —— 角落控件被排到窗口
//   右缘之外；只有 setMaximumWidth 能真正收窄标签栏、把滚动按钮限制在预留区左侧。）
// 对应Python: ui/terminal_tab_widget.py::TerminalTabWidget
class TerminalTabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit TerminalTabWidget(QWidget *parent = nullptr);

signals:
    void newLocalTerminalRequested();

protected:
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void schedulePosition();
    void positionNewTerminalButton();
    // 标签是否已溢出（QTabBar 自带的滚动按钮已显示）。
    bool tabsOverflow() const;

    static constexpr int kButtonWidth = 28;
    // 跟随最后标签时 "+" 与标签的间隙。
    static constexpr int kButtonGap = 1;
    // 溢出时 QTabBar 滚动按钮（在标签栏最右缘）与角落 "+" 之间的间隙，
    // 要足够大才不显得两者重合。
    static constexpr int kScrollGap = 12;

    QToolButton *m_newButton = nullptr;
    bool m_positionPending = false;
};

} // namespace cubeshell
