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

void TerminalTabWidget::positionNewTerminalButton()
{
    m_positionPending = false;
    if (!m_newButton)
        return;

    QTabBar *bar = tabBar();
    const int tabCount = bar->count();

    // 在右侧为按钮预留紧凑空间：标签放不下时 QTabBar 会把自己的滚动按钮
    // 限制在这个最大宽度内，悬浮按钮因此不会遮挡滚动按钮。
    const int maxBarWidth = qMax(0, width() - kButtonWidth - kButtonGap - 2);
    if (bar->maximumWidth() != maxBarWidth) {
        bar->setMaximumWidth(maxBarWidth);
        schedulePosition();
    }

    int tabHeight = 0;
    int desiredX = 0;
    int desiredY = 0;
    const QPoint origin = bar->mapTo(this, QPoint(0, 0));
    if (tabCount > 0) {
        const QRect last = bar->tabRect(tabCount - 1);
        tabHeight = qMax(20, last.height());
        desiredX = origin.x() + last.right() + 1 + kButtonGap;
        desiredY = origin.y() + last.y();
    } else {
        tabHeight = qMax(20, bar->sizeHint().height());
        desiredX = origin.x();
        desiredY = origin.y();
    }

    if (m_newButton->width() != kButtonWidth || m_newButton->height() != tabHeight)
        m_newButton->setFixedSize(kButtonWidth, tabHeight);

    // 标签溢出时 tabRect(last) 可能落在可见区域之外：把按钮夹在预留空间内，
    // 保证任何情况下都可点击。
    const int maxX = qMax(0, rect().right() - m_newButton->width());
    m_newButton->move(qBound(0, desiredX, maxX), qMax(0, desiredY));
    m_newButton->show();
    m_newButton->raise();
}

} // namespace cubeshell
