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
// QTabWidget::setCornerWidget 会把控件钉在最右侧，标签少时留下大片空隙；
// 这里改为按最后一个标签的矩形定位，并限制标签栏最大宽度，确保标签溢出
// 时 QTabBar 自带的滚动按钮不会被遮挡。
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

    static constexpr int kButtonWidth = 28;
    static constexpr int kButtonGap = 1;

    QToolButton *m_newButton = nullptr;
    bool m_positionPending = false;
};

} // namespace cubeshell
