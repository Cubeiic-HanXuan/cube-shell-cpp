#include "terminal_tab_widget.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

namespace cubeshell {

// ---------------------------------------------------------------------------
// TabStatusDot。对应Python: cube-shell.py::TabStatusDot
// ---------------------------------------------------------------------------

TabStatusDot::TabStatusDot(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(3, 0, 0, 0);
    layout->setSpacing(0);
    m_dot = new QLabel(this);
    m_dot->setFixedSize(10, 10);
    layout->addWidget(m_dot);
    setConnected(true);
}

void TabStatusDot::setConnected(bool connected)
{
    const QString color = connected ? QStringLiteral("#4CAF50") : QStringLiteral("#f44336");
    m_dot->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; border-radius: 5px; }").arg(color));
}

// ---------------------------------------------------------------------------
// TabCloseButton。对应Python: cube-shell.py::TabCloseButton
// ---------------------------------------------------------------------------

TabCloseButton::TabCloseButton(QWidget *parent)
    : QWidget(parent)
{
    // widget 整体 25x16：比 ✕ 字符多 9px 右 margin，让 ✕ 距 tab 右边有缓冲。
    setFixedSize(25, 16);

    m_closeLabel = new QLabel(this);
    m_closeLabel->setFixedSize(16, 16);
    m_closeLabel->move(0, 0);
    m_closeLabel->setAlignment(Qt::AlignCenter);
    m_closeLabel->setText(QStringLiteral("✕"));
    m_closeLabel->setCursor(Qt::PointingHandCursor);
    // 红色 hover 只作用在 16x16 的 QLabel 上，不会画到右侧透明留白。
    m_closeLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "    background-color: transparent;"
        "    color: #888;"
        "    font-size: 14px;"
        "    font-weight: bold;"
        "    border-radius: 3px;"
        "}"
        "QLabel:hover {"
        "    background-color: #e81123;"
        "    color: white;"
        "}"));
}

void TabCloseButton::mousePressEvent(QMouseEvent *event)
{
    // 只有落在 ✕ 上的点击才算关闭，右侧透明留白透传给标签栏。
    if (m_closeLabel->geometry().contains(event->position().toPoint())) {
        emit clicked();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// ---------------------------------------------------------------------------
// TerminalTabWidget。对应Python: ui/terminal_tab_widget.py
// ---------------------------------------------------------------------------

TerminalTabWidget::TerminalTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    m_newButton = new QToolButton(this);
    m_newButton->setObjectName(QStringLiteral("newLocalTerminalButton"));
    m_newButton->setText(QStringLiteral("+"));
    m_newButton->setToolTip(tr("新建本机终端"));
    m_newButton->setAccessibleName(tr("新建本机终端"));
    m_newButton->setCursor(Qt::PointingHandCursor);
    m_newButton->setFocusPolicy(Qt::StrongFocus);
    m_newButton->setAutoRaise(true);
    m_newButton->setStyleSheet(QStringLiteral(
        "QToolButton#newLocalTerminalButton {"
        "    padding: 0;"
        "    margin-top: 2px;"
//         "    margin: 2;"
        "    margin-bottom: 0;"
        "    border: none;"
        "    border-radius: 2px;"
        "    font-size: 20px;"
        "    font-weight: 400;"
        "}"
        "QToolButton#newLocalTerminalButton:pressed { background-color: palette(mid); }"
        "QToolButton#newLocalTerminalButton:focus { border-color: palette(highlight); }"));
    connect(m_newButton, &QToolButton::clicked,
            this, &TerminalTabWidget::newLocalTerminalRequested);
#ifndef CUBESHELL_WITH_LOCALPTY
    // 鸿蒙：无本地 shell，"+"（新建本机终端）按钮没有可用动作，整体隐藏。
    m_newButton->setVisible(false);
#endif

    QTabBar *bar = tabBar();
    connect(bar, &QTabBar::tabMoved, this, [this](int, int) { schedulePosition(); });
    bar->installEventFilter(this);
    schedulePosition();
}

void TerminalTabWidget::tabInserted(int index)
{
    QTabWidget::tabInserted(index);
    schedulePosition();
}

void TerminalTabWidget::tabRemoved(int index)
{
    QTabWidget::tabRemoved(index);
    schedulePosition();
}

void TerminalTabWidget::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    schedulePosition();
}

void TerminalTabWidget::showEvent(QShowEvent *event)
{
    QTabWidget::showEvent(event);
    schedulePosition();
}

void TerminalTabWidget::changeEvent(QEvent *event)
{
    QTabWidget::changeEvent(event);
    switch (event->type()) {
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::PaletteChange:
        schedulePosition();
        break;
    default:
        break;
    }
}

bool TerminalTabWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == tabBar()) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::LayoutRequest:
            schedulePosition();
            break;
        default:
            break;
        }
    }
    return QTabWidget::eventFilter(watched, event);
}

void TerminalTabWidget::schedulePosition()
{
    if (m_positionPending || !m_newButton)
        return;
    m_positionPending = true;
    QTimer::singleShot(0, this, &TerminalTabWidget::positionNewTerminalButton);
}

bool TerminalTabWidget::tabsOverflow() const
{
    // QTabBar 只有在标签溢出且 usesScrollButtons=true 时才会显示它自带的两个
    // 滚动 QToolButton。我们挂在标签上的装饰件（TabStatusDot/TabCloseButton）
    // 都是 QWidget 子类，不会被这里的 QToolButton 匹配到，可安全用来判断溢出。
    const auto buttons = tabBar()->findChildren<QToolButton *>();
    for (const QToolButton *b : buttons) {
        if (b->isVisible())
            return true;
    }
    return false;
}

void TerminalTabWidget::positionNewTerminalButton()
{
    m_positionPending = false;
    if (!m_newButton)
        return;

    QTabBar *bar = tabBar();
    const int tabCount = bar->count();

    // 真正缩小标签栏可用宽度：把 tab bar 的最大宽度限制为窗口宽减去右侧预留
    // （"+" 按钮宽 + 与滚动按钮的间隙）。注意 QTabWidget 的角落控件（corner
    // widget）在本平台并不会把标签栏挤窄（实测角落控件被排到窗口右缘之外），
    // 而 tabBar->setMaximumWidth 经实测能把标签栏收窄、让溢出时的滚动按钮停在
    // 该宽度内，从而在右侧留出一块不被遮挡的区域放 "+"。
    const int reserve = kButtonWidth + kButtonGap + kScrollGap;
    const int maxBarWidth = qMax(0, width() - reserve);
    if (bar->maximumWidth() != maxBarWidth) {
        bar->setMaximumWidth(maxBarWidth);
        // 宽度变化后 tab 矩形会变，重排一轮再用新几何定位，避免用旧值。
        schedulePosition();
        return;
    }

    int tabHeight = 0;
    const QPoint origin = bar->mapTo(this, QPoint(0, 0));
    if (tabCount > 0)
        tabHeight = qMax(20, bar->tabRect(tabCount - 1).height());
    else
        tabHeight = qMax(20, bar->sizeHint().height());

    if (m_newButton->width() != kButtonWidth || m_newButton->height() != tabHeight)
        m_newButton->setFixedSize(kButtonWidth, tabHeight);

    // 右上角预留位的左边界（此区域不含滚动按钮）。
    const int cornerX = qMax(0, rect().right() - m_newButton->width());

    int desiredX = 0;
    int desiredY = 0;
    if (tabCount == 0) {
        desiredX = origin.x();
        desiredY = origin.y();
    } else if (!tabsOverflow()) {
        // 标签放得下：紧跟最后一个标签（浏览器式）。钳到预留位之内。
        const QRect last = bar->tabRect(tabCount - 1);
        desiredX = qMin(origin.x() + last.right() + 1 + kButtonGap, cornerX);
        desiredY = origin.y() + last.y();
    } else {
        // 标签溢出（滚动按钮已显示）：固定到右上角预留位，绝不压滚动按钮。
        desiredX = cornerX;
        desiredY = origin.y() + bar->tabRect(tabCount - 1).y();
    }

    m_newButton->move(qMax(0, desiredX), qMax(0, desiredY));
    m_newButton->show();
    m_newButton->raise();
}

} // namespace cubeshell
