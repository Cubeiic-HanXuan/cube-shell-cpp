// TerminalDisplay.cpp — C++ port of qtermwidget/terminal_display.py
//
// See TerminalDisplay.h for the module description and copyright.

#include "TerminalDisplay.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPen>
#include <QResizeEvent>
#include <QSpacerItem>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include "Screen.h"           // MODE_Cursor, Screen::isSelectionValid, blockSelectionMode
#include "ScreenWindow.h"     // ScreenWindow (already ported)
#include "konsole_wcwidth.h"  // konsole_wcwidth

namespace Konsole {

// ---------------------------------------------------------------------------
// Static / module-level data (对应 Python 顶部常量)
// ---------------------------------------------------------------------------

bool TerminalDisplay::s_antialiasText = true;
bool TerminalDisplay::s_haveTransparency = true;

// 对应Python: TEXT_BLINK_DELAY = 500
static constexpr int TEXT_BLINK_DELAY = 500;

// 对应Python: REPCHAR = "..."
static const char REPCHAR[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefgjijklmnopqrstuvwxyz"
    "0123456789./+@";

// Line drawing characters mapping (对应 Python VT100_GRAPHICS).
// 上游用 Character::isLineChar / LINE_CHARS 表;Python 版只用 LINE_CHARS。
// LTR override character (0x202D) is unused in the simplified drawing path.

// 对应Python: class LineEncode (线字符绘制位编码)
namespace LineEncode {
static constexpr int TopL = (1 << 1);
static constexpr int TopC = (1 << 2);
static constexpr int TopR = (1 << 3);
static constexpr int LeftT = (1 << 5);
static constexpr int Int11 = (1 << 6);
static constexpr int Int12 = (1 << 7);
static constexpr int Int13 = (1 << 8);
static constexpr int RightT = (1 << 9);
static constexpr int LeftC = (1 << 10);
static constexpr int Int21 = (1 << 11);
static constexpr int Int22 = (1 << 12);
static constexpr int Int23 = (1 << 13);
static constexpr int RightC = (1 << 14);
static constexpr int LeftB = (1 << 15);
static constexpr int Int31 = (1 << 16);
static constexpr int Int32 = (1 << 17);
static constexpr int Int33 = (1 << 18);
static constexpr int RightB = (1 << 19);
static constexpr int BotL = (1 << 21);
static constexpr int BotC = (1 << 22);
static constexpr int BotR = (1 << 23);
} // namespace LineEncode

// 对应Python: LINE_CHARS (256 项,来自 LineFont.h)
static const quint32 LINE_CHARS[256] = {
    0x00007c00, 0x000fffe0, 0x00421084, 0x00e739ce, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00427000, 0x004e7380, 0x00e77800, 0x00ef7bc0,
    0x00421c00, 0x00439ce0, 0x00e73c00, 0x00e7bde0, 0x00007084, 0x000e7384, 0x000079ce, 0x000f7bce,
    0x00001c84, 0x00039ce4, 0x00003dce, 0x0007bdee, 0x00427084, 0x004e7384, 0x004279ce, 0x00e77884,
    0x00e779ce, 0x004f7bce, 0x00ef7bc4, 0x00ef7bce, 0x00421c84, 0x00439ce4, 0x00423dce, 0x00e73c84,
    0x00e73dce, 0x0047bdee, 0x00e7bde4, 0x00e7bdee, 0x00427c00, 0x0043fce0, 0x004e7f80, 0x004fffe0,
    0x004fffe0, 0x00e7fde0, 0x006f7fc0, 0x00efffe0, 0x00007c84, 0x0003fce4, 0x000e7f84, 0x000fffe4,
    0x00007dce, 0x0007fdee, 0x000f7fce, 0x000fffee, 0x00427c84, 0x0043fce4, 0x004e7f84, 0x004fffe4,
    0x00427dce, 0x00e77c84, 0x00e77dce, 0x0047fdee, 0x004e7fce, 0x00e7fde4, 0x00ef7f84, 0x004fffee,
    0x00efffe4, 0x00e7fdee, 0x00ef7fce, 0x00efffee, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x000f83e0, 0x00a5294a, 0x004e1380, 0x00a57800, 0x00ad0bc0, 0x004390e0, 0x00a53c00, 0x00a5a1e0,
    0x000e1384, 0x0000794a, 0x000f0b4a, 0x000390e4, 0x00003d4a, 0x0007a16a, 0x004e1384, 0x00a5694a,
    0x00ad2b4a, 0x004390e4, 0x00a52d4a, 0x00a5a16a, 0x004f83e0, 0x00a57c00, 0x00ad83e0, 0x000f83e4,
    0x00007d4a, 0x000f836a, 0x004f93e4, 0x00a57d4a, 0x00ad836a, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00001c00, 0x00001084, 0x00007000, 0x00421000,
    0x00039ce0, 0x000039ce, 0x000e7380, 0x00e73800, 0x000e7f80, 0x00e73884, 0x0003fce0, 0x004239ce,
    // 其余 128 项为 0
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0
};

// 权限字符串 (drwxr-xr-x) 的逐字符着色 (对应 Python _PERM_HL_COLORS)。
static QColor permHlColor(QChar c, bool &ok)
{
    ok = true;
    switch (c.toLatin1()) {
    case 'd': return QColor(QStringLiteral("#bd93f9")); // 紫
    case 'r': return QColor(QStringLiteral("#8be9fd")); // 蓝
    case 'w': return QColor(QStringLiteral("#f1fa8c")); // 黄
    case 'x': return QColor(QStringLiteral("#ff5555")); // 红
    case '-': return QColor(QStringLiteral("#6272a4")); // 灰
    default: ok = false; return QColor();
    }
}

// 对应Python: draw_line_char(painter, x, y, w, h, code)
static void drawLineChar(QPainter &painter, int x, int y, int w, int h, quint8 code)
{
    const quint32 toDraw = LINE_CHARS[code];
    if (!toDraw)
        return;

    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int ex = x + w - 1;
    const int ey = y + h - 1;

    if (toDraw & LineEncode::TopL) painter.drawLine(cx - 1, y, cx - 1, cy - 2);
    if (toDraw & LineEncode::TopC) painter.drawLine(cx, y, cx, cy - 2);
    if (toDraw & LineEncode::TopR) painter.drawLine(cx + 1, y, cx + 1, cy - 2);

    if (toDraw & LineEncode::BotL) painter.drawLine(cx - 1, cy + 2, cx - 1, ey);
    if (toDraw & LineEncode::BotC) painter.drawLine(cx, cy + 2, cx, ey);
    if (toDraw & LineEncode::BotR) painter.drawLine(cx + 1, cy + 2, cx + 1, ey);

    if (toDraw & LineEncode::LeftT) painter.drawLine(x, cy - 1, cx - 2, cy - 1);
    if (toDraw & LineEncode::LeftC) painter.drawLine(x, cy, cx - 2, cy);
    if (toDraw & LineEncode::LeftB) painter.drawLine(x, cy + 1, cx - 2, cy + 1);

    if (toDraw & LineEncode::RightT) painter.drawLine(cx + 2, cy - 1, ex, cy - 1);
    if (toDraw & LineEncode::RightC) painter.drawLine(cx + 2, cy, ex, cy);
    if (toDraw & LineEncode::RightB) painter.drawLine(cx + 2, cy + 1, ex, cy + 1);

    if (toDraw & LineEncode::Int11) painter.drawPoint(cx - 1, cy - 1);
    if (toDraw & LineEncode::Int12) painter.drawPoint(cx, cy - 1);
    if (toDraw & LineEncode::Int13) painter.drawPoint(cx + 1, cy - 1);

    if (toDraw & LineEncode::Int21) painter.drawPoint(cx - 1, cy);
    if (toDraw & LineEncode::Int22) painter.drawPoint(cx, cy);
    if (toDraw & LineEncode::Int23) painter.drawPoint(cx + 1, cy);

    if (toDraw & LineEncode::Int31) painter.drawPoint(cx - 1, cy + 1);
    if (toDraw & LineEncode::Int32) painter.drawPoint(cx, cy + 1);
    if (toDraw & LineEncode::Int33) painter.drawPoint(cx + 1, cy + 1);
}

// 对应Python: draw_other_char(painter, x, y, w, h, code)
static void drawOtherChar(QPainter &painter, int x, int y, int w, int h, quint8 code)
{
    const int cx = x + w / 2;
    const int cy = y + h / 2;
    const int ex = x + w - 1;
    const int ey = y + h - 1;

    if (code >= 0x4C && code <= 0x4F) {
        const int xHalfGap = qMax(w / 15, 1);
        const int yHalfGap = qMax(h / 15, 1);

        if (code == 0x4D) {
            painter.drawLine(x, cy - 1, cx - xHalfGap - 1, cy - 1);
            painter.drawLine(x, cy + 1, cx - xHalfGap - 1, cy + 1);
            painter.drawLine(cx + xHalfGap, cy - 1, ex, cy - 1);
            painter.drawLine(cx + xHalfGap, cy + 1, ex, cy + 1);
        }
        if (code == 0x4C || code == 0x4D) {
            painter.drawLine(x, cy, cx - xHalfGap - 1, cy);
            painter.drawLine(cx + xHalfGap, cy, ex, cy);
        } else if (code == 0x4F) {
            painter.drawLine(cx - 1, y, cx - 1, cy - yHalfGap - 1);
            painter.drawLine(cx + 1, y, cx + 1, cy - yHalfGap - 1);
            painter.drawLine(cx - 1, cy + yHalfGap, cx - 1, ey);
            painter.drawLine(cx + 1, cy + yHalfGap, cx + 1, ey);
        }
        if (code == 0x4E || code == 0x4F) {
            painter.drawLine(cx, y, cx, cy - yHalfGap - 1);
            painter.drawLine(cx, cy + yHalfGap, cx, ey);
        }
    } else if (code >= 0x6D && code <= 0x70) {
        const int r = w * 3 / 8;
        const int d = 2 * r;

        if (code == 0x6D) {
            painter.drawLine(cx, cy + r, cx, ey);
            painter.drawLine(cx + r, cy, ex, cy);
            painter.drawArc(cx, cy, d, d, 90 * 16, 90 * 16);
        } else if (code == 0x6E) {
            painter.drawLine(cx, cy + r, cx, ey);
            painter.drawLine(x, cy, cx - r, cy);
            painter.drawArc(cx - d, cy, d, d, 0 * 16, 90 * 16);
        } else if (code == 0x6F) {
            painter.drawLine(cx, y, cx, cy - r);
            painter.drawLine(x, cy, cx - r, cy);
            painter.drawArc(cx - d, cy - d, d, d, 270 * 16, 90 * 16);
        } else if (code == 0x70) {
            painter.drawLine(cx, y, cx, cy - r);
            painter.drawLine(cx + r, cy, ex, cy);
            painter.drawArc(cx, cy - d, d, d, 180 * 16, 90 * 16);
        }
    } else if (code >= 0x71 && code <= 0x73) {
        if (code == 0x71) {
            painter.drawLine(ex, y, x, ey);
        } else if (code == 0x72) {
            painter.drawLine(x, y, ex, ey);
        } else if (code == 0x73) {
            painter.drawLine(ex, y, x, ey);
            painter.drawLine(x, y, ex, ey);
        }
    }
}

// ---------------------------------------------------------------------------
// ScrollBar
// ---------------------------------------------------------------------------

ScrollBar::ScrollBar(QWidget *parent)
    : QScrollBar(parent)
{
}

void ScrollBar::enterEvent(QEnterEvent *event)
{
    // 对应Python: enterEvent — 目前只是透传 (恢复自动隐藏的鼠标光标在上游处理)。
    QScrollBar::enterEvent(event);
}

// ---------------------------------------------------------------------------
// AutoScrollHandler
// ---------------------------------------------------------------------------

AutoScrollHandler::AutoScrollHandler(QObject *parent)
    : QObject(parent)
{
    parent->installEventFilter(this);
}

void AutoScrollHandler::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != _timerId)
        return;

    auto *widget = qobject_cast<QWidget *>(parent());
    if (!widget)
        return;

    QMouseEvent mouseEvent(QEvent::MouseMove,
                           widget->mapFromGlobal(QCursor::pos()),
                           QCursor::pos(),
                           Qt::NoButton,
                           Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(widget, &mouseEvent);
}

bool AutoScrollHandler::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    auto *widget = qobject_cast<QWidget *>(parent());
    if (!widget)
        return false;

    if (event->type() == QEvent::MouseMove) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const bool mouseInWidget = widget->rect().contains(mouseEvent->pos());
        if (mouseInWidget) {
            if (_timerId) {
                killTimer(_timerId);
                _timerId = 0;
            }
        } else {
            if (!_timerId && (mouseEvent->buttons() & Qt::LeftButton))
                _timerId = startTimer(100);
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (_timerId && !(mouseEvent->buttons() & Qt::LeftButton)) {
            killTimer(_timerId);
            _timerId = 0;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// TerminalDisplay — construction
// ---------------------------------------------------------------------------

TerminalDisplay::TerminalDisplay(QWidget *parent)
    : QWidget(parent)
{
    // 对应Python __init__:初始化颜色表为 Konsole 基础调色板。
    const ColorEntry *base = base_color_table();
    for (int i = 0; i < TABLE_COLORS; ++i)
        _colorTable[i] = base[i];

    // 过滤器链
    _filterChain = new TerminalImageFilterChain();

    // 输出/滤镜合并刷新定时器
    _outputUpdateTimer = new QTimer(this);
    _outputUpdateTimer->setSingleShot(true);
    connect(_outputUpdateTimer, &QTimer::timeout, this, &TerminalDisplay::_flushPendingOutputUpdates);

    _filterUpdateTimer = new QTimer(this);
    _filterUpdateTimer->setSingleShot(true);
    connect(_filterUpdateTimer, &QTimer::timeout, this, &TerminalDisplay::_flushPendingFilterUpdates);

    _scrollUpdateTimer = new QTimer(this);
    _scrollUpdateTimer->setSingleShot(true);
    connect(_scrollUpdateTimer, &QTimer::timeout, this, &TerminalDisplay::_delayedScrollUpdate);

    _initWidget();
}

TerminalDisplay::~TerminalDisplay()
{
    delete _filterChain;
    _filterChain = nullptr;
}

void TerminalDisplay::_initWidget()
{
    // 对应Python _init_widget:正确的初始化顺序。
    setLayoutDirection(Qt::LeftToRight);

    _scrollBar = new ScrollBar(this);
    _scrollBar->hide();
    connect(_scrollBar, &QScrollBar::valueChanged, this, &TerminalDisplay::scrollBarPositionChanged);

    _gridLayout = new QGridLayout(this);
    _gridLayout->setContentsMargins(0, 0, 0, 0);
    setLayout(_gridLayout);

    _setupTimers();

    setFocusPolicy(Qt::WheelFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    // 禁用 QSS 背景绘制:全局 QWidget 样式表会覆盖 QPalette,
    // 终端背景必须始终来自 color-scheme (_colorTable)。
    setAttribute(Qt::WA_StyledBackground, false);

    _makeImage();

    // 自动滚动处理 (对应 Python AutoScrollHandler)。
    _autoScrollHandler = new AutoScrollHandler(this);

    _mouseMarks = true;
    setCursor(_mouseMarks ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void TerminalDisplay::_setupTimers()
{
    _blinkTimer = new QTimer(this);
    connect(_blinkTimer, &QTimer::timeout, this, &TerminalDisplay::_blinkEvent);

    _blinkCursorTimer = new QTimer(this);
    connect(_blinkCursorTimer, &QTimer::timeout, this, &TerminalDisplay::_blinkCursorEvent);
}

// ---------------------------------------------------------------------------
// ScreenWindow wiring
// ---------------------------------------------------------------------------

void TerminalDisplay::setScreenWindow(ScreenWindow *window)
{
    if (_screenWindow)
        _screenWindow->disconnect(this);

    _screenWindow = window;

    if (window) {
        connect(window, &ScreenWindow::outputChanged, this, &TerminalDisplay::_onOutputChanged);
        connect(window, &ScreenWindow::selectionChanged, this, &TerminalDisplay::_onSelectionChanged);
        connect(window, &ScreenWindow::scrolled, this, &TerminalDisplay::_onScrolled);
        connect(window, &ScreenWindow::scrollToEnd, this, &TerminalDisplay::scrollToEnd);

        window->setWindowLines(_lines);
    }
}

// ---------------------------------------------------------------------------
// Update coalescing (对应 Python _on_output_changed / _schedule_* / _flush_*)
// ---------------------------------------------------------------------------

void TerminalDisplay::_onOutputChanged()
{
    _pendingUpdateLineProperties = true;
    _pendingUpdateImage = true;
    _scheduleOutputUpdate();
    _scheduleFilterUpdate();
}

void TerminalDisplay::_onSelectionChanged()
{
    _pendingUpdateImage = true;
    _scheduleOutputUpdate();
}

void TerminalDisplay::_onScrolled(int line)
{
    Q_UNUSED(line);
    _scheduleFilterUpdate();
}

void TerminalDisplay::_scheduleOutputUpdate()
{
    if (!_outputUpdateTimer->isActive())
        _outputUpdateTimer->start(_outputUpdateIntervalMs);
}

void TerminalDisplay::_scheduleFilterUpdate()
{
    _pendingUpdateFilters = true;
    if (!_filterUpdateTimer->isActive())
        _filterUpdateTimer->start(_filterUpdateIntervalMs);
}

void TerminalDisplay::_flushPendingOutputUpdates()
{
    if (!_screenWindow) {
        _pendingUpdateImage = false;
        _pendingUpdateLineProperties = false;
        return;
    }

    if (_pendingUpdateLineProperties) {
        _pendingUpdateLineProperties = false;
        updateLineProperties();
    }
    if (_pendingUpdateImage) {
        _pendingUpdateImage = false;
        updateImage();
    }
}

void TerminalDisplay::_flushPendingFilterUpdates()
{
    if (!_screenWindow) {
        _pendingUpdateFilters = false;
        return;
    }

    if (_outputUpdateTimer->isActive()) {
        _filterUpdateTimer->start(_filterUpdateIntervalMs);
        return;
    }

    if (_pendingUpdateFilters) {
        _pendingUpdateFilters = false;
        processFilters();
    }
}

// ---------------------------------------------------------------------------
// Color table
// ---------------------------------------------------------------------------

void TerminalDisplay::setColorTable(const ColorEntry table[])
{
    for (int i = 0; i < TABLE_COLORS; ++i)
        _colorTable[i] = table[i];

    setBackgroundColor(_colorTable[DEFAULT_BACK_COLOR].color);
}

void TerminalDisplay::setBackgroundColor(const QColor &color)
{
    _colorTable[DEFAULT_BACK_COLOR].color = color;
    QPalette p = palette();
    p.setColor(backgroundRole(), color);
    setPalette(p);

    // 避免把调色板改动传播给滚动条。
    _scrollBar->setPalette(QApplication::palette());

    update();
}

void TerminalDisplay::setForegroundColor(const QColor &color)
{
    _colorTable[DEFAULT_FORE_COLOR].color = color;
    update();
}

void TerminalDisplay::setSuppressProgramBackgroundColors(bool suppress)
{
    _suppressProgramBackgroundColors = suppress;
    update();
}

void TerminalDisplay::setOpacity(qreal opacity)
{
    _opacity = qBound<qreal>(0.0, opacity, 1.0);
}

void TerminalDisplay::setBackgroundImage(const QString &backgroundImage)
{
    if (!backgroundImage.isEmpty()) {
        _backgroundImage.load(backgroundImage);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    } else {
        _backgroundImage = QPixmap();
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }
}

// ---------------------------------------------------------------------------
// Scroll bar
// ---------------------------------------------------------------------------

void TerminalDisplay::setScrollBarPosition(ScrollBarPosition position)
{
    _scrollbarLocation = position;
    if (position == NoScrollBar)
        _scrollBar->hide();
    else
        _scrollBar->show();

    _propagateSize();
    update();
}

void TerminalDisplay::_setScroll(int cursor, int lines)
{
    if (_scrollBar->minimum() == 0 &&
        _scrollBar->maximum() == (lines - _lines) &&
        _scrollBar->value() == cursor)
        return;

    _scrollBar->setRange(0, lines - _lines);
    _scrollBar->setSingleStep(1);
    _scrollBar->setPageStep(_lines);
    _scrollBar->setValue(cursor);
}

void TerminalDisplay::setScroll(int cursor, int lines)
{
    _setScroll(cursor, lines);
}

void TerminalDisplay::scrollToEnd()
{
    _scrollBar->setValue(_scrollBar->maximum());

    if (_screenWindow) {
        _screenWindow->scrollTo(_scrollBar->value() + 1);
        _screenWindow->setTrackOutput(_screenWindow->atEndOfOutput());
    }
}

void TerminalDisplay::scrollBarPositionChanged(int value)
{
    if (!_screenWindow)
        return;

    // 节流滚动更新 (~60fps)。
    _pendingScrollValue = value;
    if (!_scrollUpdateTimer->isActive())
        _scrollUpdateTimer->start(16);
}

void TerminalDisplay::_delayedScrollUpdate()
{
    if (!_screenWindow)
        return;

    _screenWindow->scrollTo(_pendingScrollValue);
    const bool atEnd = _pendingScrollValue == _scrollBar->maximum();
    _screenWindow->setTrackOutput(atEnd);
    updateImage();
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

void TerminalDisplay::processFilters()
{
    if (!_screenWindow)
        return;

    const QRegion preUpdateHotspots = _hotSpotRegion();

    Character *image = _screenWindow->getImage();
    const int lines = _screenWindow->windowLines();
    const int columns = _screenWindow->windowColumns();
    const QVector<LineProperty> lineProperties = _screenWindow->getLineProperties();

    _filterChain->setImage(image, lines, columns, lineProperties);
    _filterChain->process();

    const QRegion postUpdateHotspots = _hotSpotRegion();
    update(preUpdateHotspots | postUpdateHotspots);
}

QRegion TerminalDisplay::_hotSpotRegion() const
{
    QRegion region;
    const QList<Filter::HotSpot *> hotspots = _filterChain->hotSpots();

    for (Filter::HotSpot *hotspot : hotspots) {
        QRect r;
        if (hotspot->startLine() == hotspot->endLine()) {
            r.setLeft(hotspot->startColumn());
            r.setTop(hotspot->startLine());
            r.setRight(hotspot->endColumn());
            r.setBottom(hotspot->endLine());
            region |= _imageToWidget(r);
        } else {
            r.setLeft(hotspot->startColumn());
            r.setTop(hotspot->startLine());
            r.setRight(_columns);
            r.setBottom(hotspot->startLine());
            region |= _imageToWidget(r);

            for (int line = hotspot->startLine() + 1; line < hotspot->endLine(); ++line) {
                r.setLeft(0);
                r.setTop(line);
                r.setRight(_columns);
                r.setBottom(line);
                region |= _imageToWidget(r);
            }

            r.setLeft(0);
            r.setTop(hotspot->endLine());
            r.setRight(hotspot->endColumn());
            r.setBottom(hotspot->endLine());
            region |= _imageToWidget(r);
        }
    }
    return region;
}

QList<QAction *> TerminalDisplay::filterActions(const QPoint &position)
{
    int charLine, charColumn;
    getCharacterPosition(position, charLine, charColumn);
    Filter::HotSpot *hotspot = _filterChain->hotSpotAt(charLine, charColumn);
    return hotspot ? hotspot->actions() : QList<QAction *>();
}

// ---------------------------------------------------------------------------
// Cursor
// ---------------------------------------------------------------------------

void TerminalDisplay::setKeyboardCursorColor(bool useForegroundColor, const QColor &color)
{
    _cursorColor = useForegroundColor ? QColor() : color;
}

void TerminalDisplay::setBlinkingCursor(bool blink)
{
    _hasBlinkingCursor = blink;

    if (blink && !_blinkCursorTimer->isActive() && hasFocus()) {
        const int flashTime = qMax(QApplication::cursorFlashTime(), 1000);
        _blinkCursorTimer->start(flashTime / 2);
    }

    if (!blink && _blinkCursorTimer->isActive()) {
        _blinkCursorTimer->stop();
        if (_cursorBlinking)
            _blinkCursorEvent();
        else
            _cursorBlinking = false;
    }
}

void TerminalDisplay::setBlinkingTextEnabled(bool blink)
{
    _allowBlinkingText = blink;

    if (blink && !_blinkTimer->isActive() && hasFocus())
        _blinkTimer->start(TEXT_BLINK_DELAY);

    if (!blink && _blinkTimer->isActive()) {
        _blinkTimer->stop();
        _blinking = false;
    }
}

void TerminalDisplay::_blinkEvent()
{
    if (!_allowBlinkingText)
        return;
    _blinking = !_blinking;
    update();
}

void TerminalDisplay::_blinkCursorEvent()
{
    _cursorBlinking = !_cursorBlinking;
    _updateCursor();
}

void TerminalDisplay::_updateCursor()
{
    QPoint cursorPos = _cursorPosition();
    int charWidth = 1;
    if (!_image.isEmpty()) {
        int idx = cursorPos.y() * _columns + cursorPos.x();
        if (idx >= 0 && idx < _image.size()) {
            quint16 c = _image[idx].character;
            // 若光标在续位列上,回退到宽字符起始列。
            if (c == 0 && cursorPos.x() > 0) {
                cursorPos = QPoint(cursorPos.x() - 1, cursorPos.y());
                idx -= 1;
                c = _image[idx].character;
            }
            if (c > 0) {
                const int w = konsole_wcwidth(c);
                if (w > 1)
                    charWidth = w;
            }
        }
    }
    const QRect cursorRect = _imageToWidget(QRect(cursorPos, QSize(charWidth, 1)));
    update(cursorRect);
}

QPoint TerminalDisplay::_cursorPosition() const
{
    return _screenWindow ? _screenWindow->cursorPosition() : QPoint(0, 0);
}

// ---------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------

void TerminalDisplay::setVTFont(const QFont &font)
{
    if (!QFontInfo(font).fixedPitch())
        qWarning("Using a variable-width font in the terminal may cause display issues");

    QFont f = font;
    if (s_antialiasText)
        f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferQuality));
    else
        f.setStyleStrategy(QFont::NoAntialias);

    f.setKerning(false);
    f.setHintingPreference(QFont::PreferDefaultHinting);
    f.setStyleName(QString());

    QWidget::setFont(f);
    _fontChange(f);
}

void TerminalDisplay::_fontChange(const QFont &font)
{
    QFontMetrics fm(font);
    _fontHeight = fm.height() + _lineSpacing;

    // 复刻 C++ 逻辑:计算 REPCHAR 的平均宽度。
    int widthSum = 0;
    const int repLen = static_cast<int>(sizeof(REPCHAR) - 1);
    for (int i = 0; i < repLen; ++i)
        widthSum += fm.horizontalAdvance(QLatin1Char(REPCHAR[i]));
    _fontWidth = repLen > 0 ? qRound(static_cast<double>(widthSum) / repLen) : 1;

    _fixedFont = true; // 强制等宽处理以启用优化绘制路径
    _fixedFontOriginal = _fixedFont;

    if (_fontWidth < 1)
        _fontWidth = 1;

    _fontAscent = fm.ascent();

    emit changedFontMetricSignal(_fontHeight, _fontWidth);
    _propagateSize();

    update();
}

// ---------------------------------------------------------------------------
// Size and layout
// ---------------------------------------------------------------------------

void TerminalDisplay::setSize(int cols, int lins)
{
    _columns = qMax(1, cols);
    _lines = qMax(1, lins);
    _usedColumns = qMin(_usedColumns, _columns);
    _usedLines = qMin(_usedLines, _lines);

    const int scrollBarWidth =
        (_scrollBar->isHidden() ||
         _scrollBar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, _scrollBar))
            ? 0
            : _scrollBar->sizeHint().width();

    const int horizontalMargin = 2 * _leftBaseMargin;
    const int verticalMargin = 2 * _topBaseMargin;

    const QSize newSize(horizontalMargin + scrollBarWidth + (cols * _fontWidth),
                        verticalMargin + (lins * _fontHeight));

    if (newSize != size()) {
        _size = newSize;
        updateGeometry();
    }

    if (!_image.isEmpty())
        _makeImage();
}

void TerminalDisplay::setFixedSize(int cols, int lins)
{
    _isFixedSize = true;
    _columns = qMax(1, cols);
    _lines = qMax(1, lins);
    _usedColumns = qMin(_usedColumns, _columns);
    _usedLines = qMin(_usedLines, _lines);

    if (!_image.isEmpty())
        _makeImage();

    setSize(cols, lins);
    QWidget::setFixedSize(_size);
}

QSize TerminalDisplay::sizeHint() const
{
    return _size.isValid() ? _size : QSize(800, 600);
}

// ---------------------------------------------------------------------------
// Bell
// ---------------------------------------------------------------------------

void TerminalDisplay::bell(const QString &message)
{
    if (_bellMode == NoBell)
        return;

    if (_allowBell) {
        _allowBell = false;
        QTimer::singleShot(500, this, &TerminalDisplay::_enableBell);

        if (_bellMode == SystemBeepBell) {
            QApplication::beep();
        } else if (_bellMode == NotifyBell) {
            emit notifyBell(message);
        } else if (_bellMode == VisualBell) {
            _swapColorTable();
            QTimer::singleShot(200, this, &TerminalDisplay::_swapColorTable);
        }
    }
}

void TerminalDisplay::_enableBell()
{
    _allowBell = true;
}

void TerminalDisplay::_swapColorTable()
{
    std::swap(_colorTable[0], _colorTable[1]);
    _colorsInverted = !_colorsInverted;
    update();
}

// ---------------------------------------------------------------------------
// Line spacing
// ---------------------------------------------------------------------------

void TerminalDisplay::setLineSpacing(int spacing)
{
    _lineSpacing = spacing;
    setVTFont(font()); // 触发更新
}

// ---------------------------------------------------------------------------
// Mouse usage / primary screen / report focus
// ---------------------------------------------------------------------------

void TerminalDisplay::setUsesMouse(bool usesMouse)
{
    if (_mouseMarks != usesMouse) {
        _mouseMarks = usesMouse;
        setCursor(_mouseMarks ? Qt::IBeamCursor : Qt::ArrowCursor);
        emit usesMouseChanged();
    }
}

void TerminalDisplay::setPrimaryScreenInUse(bool primary)
{
    if (_primaryScreenInUse != primary) {
        _primaryScreenInUse = primary;
        update();
    }
}

void TerminalDisplay::setReportFocusMode(bool enabled)
{
    if (_programReportFocus != enabled) {
        _programReportFocus = enabled;
        update();
    }
}

// ---------------------------------------------------------------------------
// calculateTextArea / bracketText / charClass
// ---------------------------------------------------------------------------

QRect TerminalDisplay::calculateTextArea(int topLeftX, int topLeftY, int startColumn,
                                         int line, int length)
{
    const int left = _fixedFont ? _fontWidth * startColumn : _textWidth(0, startColumn, line);
    const int top = _fontHeight * line;
    const int width = _fixedFont ? _fontWidth * length : _textWidth(startColumn, length, line);

    return QRect(_leftMargin + topLeftX + left,
                 _topMargin + topLeftY + top,
                 width,
                 _fontHeight);
}

QString TerminalDisplay::bracketText(const QString &text) const
{
    return _bracketText(text);
}

QChar TerminalDisplay::charClass(const Character &ch) const
{
    return _charClass(ch);
}

// ---------------------------------------------------------------------------
// paintEvent / drawing
// ---------------------------------------------------------------------------

void TerminalDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter;
    if (!painter.begin(this))
        return;

    const QRect cr = contentsRect();

    // 背景图
    if (!_backgroundImage.isNull()) {
        QColor background = _colorTable[DEFAULT_BACK_COLOR].color;
        if (_opacity < 1.0) {
            background.setAlphaF(_opacity);
            painter.save();
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(cr, background);
            painter.restore();
        } else {
            painter.fillRect(cr, background);
        }

        painter.save();
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        if (_backgroundMode == BackgroundStretch) {
            painter.drawPixmap(cr, _backgroundImage, _backgroundImage.rect());
        } else if (_backgroundMode == BackgroundZoom) {
            _drawBackgroundZoom(painter, cr);
        } else if (_backgroundMode == BackgroundFit) {
            _drawBackgroundFit(painter, cr);
        } else if (_backgroundMode == BackgroundCenter) {
            _drawBackgroundCenter(painter, cr);
        } else {
            painter.drawPixmap(0, 0, _backgroundImage);
        }
        painter.restore();
    }

    // 内容
    const QRegion regionToDraw = event->region() & cr;

    // 本帧只计算一次语法高亮映射(备用屏/交互式 TUI 返回空 → 本帧不高亮)。
    _highlightMap = _computeHighlightMap();
    _hasHighlightMap = !_highlightMap.isEmpty();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const auto rects = regionToDraw.rects();
#else
    // Qt < 6.8 没有 QRegion::rects()，用迭代器区间收集。
    const QList<QRect> rects(regionToDraw.begin(), regionToDraw.end());
#endif
    if (rects.size() > 256) {
        const QRect rect = regionToDraw.boundingRect();
        // 直接取 color-scheme 背景色:palette 可能被全局 QSS 覆盖
        _drawBackground(painter, rect, _colorTable[DEFAULT_BACK_COLOR].color, true);
        _drawContents(painter, rect);
    } else {
        for (const QRect &rect : rects) {
            _drawBackground(painter, rect, _colorTable[DEFAULT_BACK_COLOR].color, true);
            _drawContents(painter, rect);
        }
    }

    _highlightMap.clear();
    _hasHighlightMap = false;

    // 滤镜 (链接下划线等;语法高亮已并入正常绘制)。
    _paintFilters(painter);

    // IME preedit
    _drawInputMethodPreeditString(painter, _preeditRect());

    painter.end();
}

void TerminalDisplay::_drawBackgroundZoom(QPainter &painter, const QRect &cr)
{
    QRect r = _backgroundImage.rect();
    const double wRatio = static_cast<double>(cr.width()) / r.width();
    const double hRatio = static_cast<double>(cr.height()) / r.height();

    if (wRatio > hRatio) {
        r.setWidth(qRound(r.width() * hRatio));
        r.setHeight(cr.height());
    } else {
        r.setHeight(qRound(r.height() * wRatio));
        r.setWidth(cr.width());
    }

    r.moveCenter(cr.center());
    painter.drawPixmap(r, _backgroundImage, _backgroundImage.rect());
}

void TerminalDisplay::_drawBackgroundFit(QPainter &painter, const QRect &cr)
{
    QRect r = _backgroundImage.rect();
    const double wRatio = static_cast<double>(cr.width()) / r.width();
    const double hRatio = static_cast<double>(cr.height()) / r.height();

    if (r.width() > cr.width()) {
        if (wRatio <= hRatio) {
            r.setHeight(qRound(r.height() * wRatio));
            r.setWidth(cr.width());
        } else {
            r.setWidth(qRound(r.width() * hRatio));
            r.setHeight(cr.height());
        }
    } else if (r.height() > cr.height()) {
        r.setWidth(qRound(r.width() * hRatio));
        r.setHeight(cr.height());
    }

    r.moveCenter(cr.center());
    painter.drawPixmap(r.topLeft(), _backgroundImage);
}

void TerminalDisplay::_drawBackgroundCenter(QPainter &painter, const QRect &cr)
{
    QRect r = _backgroundImage.rect();
    r.moveCenter(cr.center());
    painter.drawPixmap(r.topLeft(), _backgroundImage);
}

void TerminalDisplay::_calcDrawTextAdditionHeight(QPainter &painter)
{
    const QRect testRect(0, 0, 100, 100);
    const QFontMetrics fontMetrics = painter.fontMetrics();
    const QString text = QString(QChar(0x202D)) + QStringLiteral("Mq");

    painter.drawText(testRect, Qt::AlignBottom, text);

    const int textHeight = fontMetrics.height();
    _drawTextAdditionHeight = qMax(0, (textHeight - _fontHeight) / 2);
}

void TerminalDisplay::_drawBackground(QPainter &painter, const QRect &rect,
                                      const QColor &backgroundColor, bool useOpacity)
{
    if (useOpacity) {
        if (_backgroundImage.isNull()) {
            QColor color(backgroundColor);
            color.setAlphaF(_opacity);
            painter.save();
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(rect, color);
            painter.restore();
        }
    } else {
        painter.fillRect(rect, backgroundColor);
    }
}

void TerminalDisplay::_drawContents(QPainter &painter, const QRect &rect)
{
    if (_image.isEmpty())
        return;
    if (_usedColumns <= 0 || _usedLines <= 0)
        return;
    if (_fontWidth <= 0 || _fontHeight <= 0)
        return;

    const QPoint tl = contentsRect().topLeft();
    const int tlx = tl.x();
    const int tly = tl.y();

    const int lux = qMin(_usedColumns - 1, qMax(0, (rect.left() - tlx - _leftMargin) / _fontWidth));
    const int luy = qMin(_usedLines - 1, qMax(0, (rect.top() - tly - _topMargin) / _fontHeight));
    const int rlx = qMin(_usedColumns - 1, qMax(0, (rect.right() - tlx - _leftMargin) / _fontWidth));
    const int rly = qMin(_usedLines - 1, qMax(0, (rect.bottom() - tly - _topMargin) / _fontHeight));

    const QFontMetrics fm(font());

    _hasSelectionCache = _computeSelectionCache(_selectionCache);
    for (int y = luy; y <= rly; ++y)
        _drawLine(painter, y, lux, rlx, tlx, tly, fm);
    _hasSelectionCache = false;
}

QMap<int, QMap<int, QColor>> TerminalDisplay::_computeHighlightMap()
{
    QMap<int, QMap<int, QColor>> result;

    if (!_primaryScreenInUse || !_mouseMarks || _programReportFocus)
        return result;
    if (!_filterChain)
        return result;

    const QList<Filter::HotSpot *> spots = _filterChain->hotSpots();
    if (spots.isEmpty())
        return result;

    const int cols = _columns;
    const int total = _image.size();
    if (cols <= 0 || total <= 0)
        return result;

    for (Filter::HotSpot *spot : spots) {
        if (spot->type() != Filter::HotSpot::Highlight)
            continue;

        const bool isPerm = spot->isPermissionHotSpot();
        QColor fg;
        if (!isPerm) {
            fg = spot->foregroundColor();
            if (!fg.isValid())
                continue; // 仅以背景表达的高亮忽略
        }

        const int sLine = spot->startLine();
        const int eLine = spot->endLine();
        for (int line = sLine; line <= eLine; ++line) {
            const int startCol = (line == sLine) ? spot->startColumn() : 0;
            const int endCol = (line == eLine) ? spot->endColumn() : cols;
            QMap<int, QColor> &row = result[line];
            const int base = line * cols;
            for (int col = startCol; col < endCol; ++col) {
                const int idx = base + col;
                if (idx >= total)
                    break;
                if (isPerm) {
                    const quint16 ch = _image[idx].character;
                    const QChar c = (ch > 0) ? QChar(ch) : QChar(QLatin1Char(' '));
                    bool ok = false;
                    const QColor pc = permHlColor(c, ok);
                    if (!ok)
                        continue;
                    row[col] = pc;
                } else {
                    row[col] = fg;
                }
            }
        }
    }

    return result;
}

bool TerminalDisplay::_computeSelectionCache(SelectionCache &out) const
{
    ScreenWindow *sw = _screenWindow;
    if (!sw)
        return false;
    Screen *screen = sw->screen();
    if (!screen || !screen->isSelectionValid())
        return false;

    int startCol, startLine, endCol, endLine;
    sw->getSelectionStart(startCol, startLine);
    sw->getSelectionEnd(endCol, endLine);

    out.block = screen->isBlockSelectionMode();
    if (qMakePair(startLine, startCol) <= qMakePair(endLine, endCol)) {
        out.topCol = startCol; out.topLine = startLine;
        out.botCol = endCol;   out.botLine = endLine;
    } else {
        out.topCol = endCol;   out.topLine = endLine;
        out.botCol = startCol; out.botLine = startLine;
    }
    return true;
}

bool TerminalDisplay::_selectionRangeForLine(int y, int &left, int &right) const
{
    if (!_hasSelectionCache)
        return false;
    const SelectionCache &c = _selectionCache;
    if (y < c.topLine || y > c.botLine)
        return false;

    if (c.block) {
        left = qMin(c.topCol, c.botCol);
        right = qMax(c.topCol, c.botCol);
        return true;
    }
    if (c.topLine == c.botLine) {
        left = c.topCol;
        right = c.botCol;
        return true;
    }
    if (y == c.topLine) {
        left = c.topCol;
        right = _usedColumns - 1;
        return true;
    }
    if (y == c.botLine) {
        left = 0;
        right = c.botCol;
        return true;
    }
    left = 0;
    right = _usedColumns - 1;
    return true;
}

void TerminalDisplay::_drawLine(QPainter &painter, int y, int lux, int rlx,
                                int tlx, int tly, const QFontMetrics &fm)
{
    Q_UNUSED(fm);
    if (y < 0 || lux < 0 || rlx < 0)
        return;
    if (_columns <= 0 || y >= _image.size() / _columns)
        return;

    const int lineStart = y * _columns;
    int x = lux;
    int guard = 0;
    const int guardMax = qMax(16, (rlx - lux + 1) * 8);

    // 本行高亮映射(可能为空)。
    QMap<int, QColor> hlRow;
    if (_hasHighlightMap)
        hlRow = _highlightMap.value(y);

    int selLeft = 0, selRight = 0;
    const bool hasSelRange = _selectionRangeForLine(y, selLeft, selRight);

    while (x <= rlx) {
        if (++guard > guardMax)
            break;
        if (lineStart + x >= _image.size())
            break;

        Character chr = _image[lineStart + x];
        if (chr.character == 0 && x > 0) {
            // 续位列回退到宽字符起始列。
            int backGuard = 0;
            while (x > 0 && chr.character == 0 && backGuard < 4) {
                --x;
                ++backGuard;
                chr = _image[lineStart + x];
            }
        }

        // 聚合相同属性的连续字符。
        QString text;
        int textWidth = 0;
        const Character currentAttrs = chr;
        const int startX = x;
        const bool currentSelected = hasSelRange && (selLeft <= startX && startX <= selRight);

        QColor startHl;
        bool hasStartHl = false;
        if (!hlRow.isEmpty() && !currentSelected) {
            auto it = hlRow.constFind(startX);
            if (it != hlRow.constEnd()) {
                startHl = it.value();
                hasStartHl = true;
            }
        }

        while (x <= rlx && lineStart + x < _image.size()) {
            if (_image[lineStart + x].character == 0 && x > 0) {
                // 续位列继承起始列选中状态。
            } else {
                const bool selected = hasSelRange && (selLeft <= x && x <= selRight);
                if (selected != currentSelected)
                    break;

                QColor cellHl;
                bool hasCellHl = false;
                if (!hlRow.isEmpty() && !currentSelected) {
                    auto it = hlRow.constFind(x);
                    if (it != hlRow.constEnd()) {
                        cellHl = it.value();
                        hasCellHl = true;
                    }
                }
                if (hasCellHl != hasStartHl || (hasCellHl && cellHl != startHl))
                    break;
            }

            chr = _image[lineStart + x];

            if (chr.character != 0) {
                if (chr.foregroundColor != currentAttrs.foregroundColor ||
                    chr.backgroundColor != currentAttrs.backgroundColor ||
                    chr.rendition != currentAttrs.rendition)
                    break;

                if (chr.character != 0) {
                    const QChar cs(chr.character);
                    if (cs.unicode() >= 32 || cs == QLatin1Char('\t')) {
                        text += cs;
                        if (_fixedFont) {
                            int w = konsole_wcwidth(cs.unicode());
                            if (w <= 0) w = 1;
                            textWidth += w * _fontWidth;
                        } else {
                            textWidth += fm.horizontalAdvance(cs);
                        }
                    }
                }
            }

            ++x;
        }

        if (!text.isEmpty()) {
            const QRect textArea(_leftMargin + tlx + startX * _fontWidth,
                                 _topMargin + tly + y * _fontHeight,
                                 textWidth,
                                 _fontHeight);
            _drawTextFragment(painter, textArea, text, currentAttrs, currentSelected,
                              hasStartHl ? startHl : QColor());
        }
    }
}

void TerminalDisplay::_drawTextFragment(QPainter &painter, const QRect &rect,
                                        const QString &text, const Character &style,
                                        bool invertColors, const QColor &overrideFg)
{
    painter.save();

    const QColor fgColor = style.foregroundColor.color(_colorTable);
    const QColor bgColor = style.backgroundColor.color(_colorTable);
    const QColor defaultBg = _colorTable[DEFAULT_BACK_COLOR].color;

    // 反显标记:RE_REVERSE 已在 screen.updateEffectiveRendition() 阶段交换过,
    // 这里 fgColor/bgColor 已是交换后的结果,绝不能再次交换 (双重 reverse)。
    const bool isReverse = (style.rendition & RE_REVERSE);

    QColor effectiveFg, effectiveBg;
    QColor textColor;

    if (invertColors) {
        const QColor selectionBg = palette().highlight().color();
        const QColor selectionFg = palette().highlightedText().color();
        _drawBackground(painter, rect, selectionBg, false);
        effectiveFg = selectionFg;
        effectiveBg = selectionBg;
    } else {
        effectiveFg = fgColor;
        effectiveBg = bgColor;

        QColor fillColor = effectiveBg;
        if (_suppressProgramBackgroundColors && !isReverse)
            fillColor = defaultBg;

        // 反显单元格对比度保险。
        if (isReverse && effectiveBg.isValid()) {
            if (qAbs(_brightness(fillColor) - _brightness(defaultBg)) < 40) {
                fillColor = _bestBwForBg(defaultBg);
                effectiveBg = fillColor;
            }
        }

        if (fillColor != defaultBg)
            _drawBackground(painter, rect, fillColor, false);
    }

    textColor = effectiveFg;
    const bool applyOverride = (!invertColors) && overrideFg.isValid();
    if (applyOverride) {
        textColor = overrideFg;
    } else if (!invertColors && effectiveBg.isValid()) {
        if (qAbs(_brightness(textColor) - _brightness(effectiveBg)) < 20)
            textColor = _bestBwForBg(effectiveBg);
    }
    if (!applyOverride && _suppressProgramBackgroundColors && !invertColors && !isReverse) {
        if (effectiveBg.isValid() && effectiveBg != defaultBg) {
            if (qAbs(_brightness(textColor) - _brightness(defaultBg)) < 50)
                textColor = effectiveBg;
        }
        if (qAbs(_brightness(textColor) - _brightness(defaultBg)) < 20)
            textColor = _bestBwForBg(defaultBg);
    }

    if ((style.rendition & RE_CURSOR) && hasFocus()) {
        // 尊重 DECTCEM (\x1b[?25l) 隐藏光标。
        bool modeCursorOn = true;
        if (_screenWindow && _screenWindow->screen())
            modeCursorOn = _screenWindow->screen()->getMode(MODE_Cursor);

        if (modeCursorOn && !_cursorBlinking) {
            QColor cursorFill, cursorText;
            _cursorPaintColors(effectiveFg, effectiveBg, _cursorColor, cursorFill, cursorText);
            painter.fillRect(rect, cursorFill);
            textColor = cursorText;
        }
    }

    painter.setPen(textColor);

    QFont f = painter.font();
    f.setBold(bool(style.rendition & RE_BOLD));
    f.setItalic(bool(style.rendition & RE_ITALIC));
    f.setUnderline(bool(style.rendition & RE_UNDERLINE));
    painter.setFont(f);

    if (_fixedFont) {
        int currentX = rect.x();
        const int baselineY = rect.y() + _fontAscent + _lineSpacing;
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        for (const QChar ch : text) {
            int w = konsole_wcwidth(ch.unicode());
            if (w <= 0) w = 1;
            painter.drawText(currentX, baselineY, QString(ch));
            currentX += w * _fontWidth;
        }
    } else {
        painter.drawText(rect.x(), rect.y() + _fontAscent + _lineSpacing, text);
    }

    painter.restore();
}

qreal TerminalDisplay::_brightness(const QColor &color)
{
    return 0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue();
}

QColor TerminalDisplay::_bestBwForBg(const QColor &bg)
{
    return _brightness(bg) > 128 ? QColor(0, 0, 0) : QColor(255, 255, 255);
}

void TerminalDisplay::_cursorPaintColors(const QColor &effectiveFg, const QColor &effectiveBg,
                                         const QColor &configuredCursor,
                                         QColor &fill, QColor &text)
{
    if (configuredCursor.isValid()) {
        fill = configuredCursor;
        text = _bestBwForBg(configuredCursor);
        return;
    }

    fill = effectiveFg;
    text = effectiveBg;
    if (qAbs(_brightness(fill) - _brightness(text)) < 50) {
        fill = _getSmartCursorColor(effectiveFg, effectiveBg);
        text = _bestBwForBg(fill);
    }
}

QColor TerminalDisplay::_getSmartCursorColor(const QColor &fgColor, const QColor &bgColor)
{
    const qreal bgBrightness = _brightness(bgColor);
    const qreal fgBrightness = _brightness(fgColor);

    QColor contrastColor = (bgBrightness > 128) ? QColor(0, 0, 0) : QColor(255, 255, 255);
    const qreal contrastBrightness = _brightness(contrastColor);

    if (qAbs(contrastBrightness - fgBrightness) < 50)
        return (bgBrightness > 128) ? QColor(255, 0, 0) : QColor(255, 255, 0);

    return contrastColor;
}

void TerminalDisplay::_drawCharacters(QPainter &painter, const QRect &rect,
                                      const QString &text, const Character &style,
                                      bool invertColors)
{
    if (_blinking && (style.rendition & RE_BLINK))
        return;
    if (style.rendition & RE_CONCEAL)
        return;
    if (text.isEmpty())
        return;

    QFont f = painter.font();
    const bool useBold = (style.rendition & RE_BOLD) && _boldIntense;
    const bool useUnderline = (style.rendition & RE_UNDERLINE);
    const bool useItalic = (style.rendition & RE_ITALIC);
    const bool useStrikeout = (style.rendition & RE_STRIKEOUT);
    const bool useOverline = (style.rendition & RE_OVERLINE);

    if (f.bold() != useBold || f.underline() != useUnderline || f.italic() != useItalic ||
        f.strikeOut() != useStrikeout || f.overline() != useOverline) {
        f.setBold(useBold);
        f.setUnderline(useUnderline);
        f.setItalic(useItalic);
        f.setStrikeOut(useStrikeout);
        f.setOverline(useOverline);
        painter.setFont(f);
    }

    QColor color = invertColors ? style.backgroundColor.color(_colorTable)
                                : style.foregroundColor.color(_colorTable);
    painter.setPen(color);

    if (_isLineCharString(text)) {
        _drawLineCharString(painter, rect.x(), rect.y(), text, style);
    } else {
        const QFontMetrics fm(f);
        const int baselineY = rect.y() + fm.ascent();
        painter.setLayoutDirection(Qt::LeftToRight);
        painter.drawText(rect.x(), baselineY, text);
    }
}

bool TerminalDisplay::_drawCursor(QPainter &painter, const QRect &rect,
                                  const QColor &foregroundColor, const QColor &backgroundColor)
{
    Q_UNUSED(backgroundColor);
    if (_cursorBlinking)
        return false;

    QRectF cursorRect(rect);
    cursorRect.setHeight(_fontHeight - _lineSpacing - 1);

    QColor cursorColor;
    if (_cursorColor.isValid()) {
        painter.setPen(_cursorColor);
        cursorColor = _cursorColor;
    } else {
        painter.setPen(foregroundColor);
        cursorColor = foregroundColor;
    }

    if (_cursorShape == KeyboardCursorShape::BlockCursor) {
        const int penWidth = qMax(1, painter.pen().width());
        painter.drawRect(cursorRect.adjusted(penWidth / 2.0, penWidth / 2.0,
                                             -penWidth / 2.0, -penWidth / 2.0));
        if (hasFocus()) {
            painter.fillRect(cursorRect, cursorColor);
            if (!_cursorColor.isValid())
                return true;
        }
    } else if (_cursorShape == KeyboardCursorShape::UnderlineCursor) {
        painter.drawLine(cursorRect.left(), cursorRect.bottom(),
                         cursorRect.right(), cursorRect.bottom());
    } else if (_cursorShape == KeyboardCursorShape::IBeamCursor) {
        painter.drawLine(cursorRect.left(), cursorRect.top(),
                         cursorRect.left(), cursorRect.bottom());
    }

    return false;
}

void TerminalDisplay::_drawLineCharString(QPainter &painter, int x, int y,
                                          const QString &text, const Character &attributes)
{
    const QPen currentPen = painter.pen();

    if ((attributes.rendition & RE_BOLD) && _boldIntense) {
        QPen boldPen(currentPen);
        boldPen.setWidth(3);
        painter.setPen(boldPen);
    }

    for (int i = 0; i < text.size(); ++i) {
        const quint8 code = text.at(i).unicode() & 0xFF;
        if (LINE_CHARS[code])
            drawLineChar(painter, x + (_fontWidth * i), y, _fontWidth, _fontHeight, code);
        else
            drawOtherChar(painter, x + (_fontWidth * i), y, _fontWidth, _fontHeight, code);
    }

    painter.setPen(currentPen);
}

void TerminalDisplay::_paintFilters(QPainter &painter)
{
    if (_usedLines <= 0 || _usedColumns <= 0)
        return;

    const QPoint cursorPos = mapFromGlobal(QCursor::pos());
    int charLine, charColumn;
    getCharacterPosition(cursorPos, charLine, charColumn);

    if (charLine >= 0 && charLine < _image.size() / _columns &&
        charColumn >= 0 && charColumn < _columns) {
        const Character &cursorChar = _image[charLine * _columns + charColumn];
        painter.setPen(QPen(cursorChar.foregroundColor.color(_colorTable)));
    }

    const QList<Filter::HotSpot *> spots = _filterChain->hotSpots();
    for (Filter::HotSpot *spot : spots) {
        // 语法高亮 (Highlight) 已并入 _drawLine;此处只画 Link 悬停下划线。
        if (spot->type() == Filter::HotSpot::Link)
            _drawHotspotHighlight(painter, spot);
    }
}

void TerminalDisplay::_drawHotspotHighlight(QPainter &painter, Filter::HotSpot *spot)
{
    const int leftMargin =
        _leftBaseMargin +
        ((_scrollbarLocation == ScrollBarLeft &&
          !_scrollBar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, _scrollBar))
             ? _scrollBar->width()
             : 0);

    for (int line = spot->startLine(); line <= spot->endLine(); ++line) {
        const int startColumn = (line == spot->startLine()) ? spot->startColumn() : 0;
        int endColumn = (line == spot->endLine()) ? spot->endColumn() : _columns - 1;

        // 跳过末尾空白。
        while (endColumn > 0 && line < _image.size() / _columns) {
            const int charIdx = line * _columns + endColumn;
            if (charIdx < _image.size() && QChar(_image[charIdx].character).isSpace())
                --endColumn;
            else
                break;
        }
        ++endColumn;

        const QRect r(startColumn * _fontWidth + 1 + leftMargin,
                      line * _fontHeight + 1 + _topBaseMargin,
                      (endColumn - startColumn) * _fontWidth - 1,
                      _fontHeight - 1);

        if (spot->type() == Filter::HotSpot::Link) {
            const QFontMetrics fm(font());
            const int baseline = r.bottom() - fm.descent();
            const int underlinePos = baseline + fm.underlinePos();

            const QPoint cursorPos = mapFromGlobal(QCursor::pos());
            if (r.contains(cursorPos))
                painter.drawLine(r.left(), underlinePos, r.right(), underlinePos);
        }
    }
}

QRect TerminalDisplay::_preeditRect() const
{
    const int preeditLength = _preeditString.size();
    if (preeditLength == 0)
        return QRect();

    const QPoint cursorPos = _cursorPosition();
    return QRect(_leftMargin + _fontWidth * cursorPos.x(),
                 _topMargin + _fontHeight * cursorPos.y(),
                 _fontWidth * preeditLength,
                 _fontHeight);
}

void TerminalDisplay::_drawInputMethodPreeditString(QPainter &painter, const QRect &rect)
{
    if (_preeditString.isEmpty())
        return;

    const QPoint cursorPos = _cursorPosition();
    const bool invertColors = false;
    const QColor background = _colorTable[DEFAULT_BACK_COLOR].color;
    const QColor foreground = _colorTable[DEFAULT_FORE_COLOR].color;

    const int charIdx = cursorPos.y() * _columns + cursorPos.x();
    Character style;
    if (charIdx >= 0 && charIdx < _image.size())
        style = _image[charIdx];

    _drawBackground(painter, rect, background, true);
    _drawCursor(painter, rect, foreground, background);
    _drawCharacters(painter, rect, _preeditString, style, invertColors);

    _previousPreeditRect = rect;
}

void TerminalDisplay::getCharacterPosition(const QPointF &widgetPoint, int &line, int &column) const
{
    if (_usedLines <= 0 || _usedColumns <= 0 || _fontHeight <= 0) {
        line = 0;
        column = 0;
        return;
    }
    line = static_cast<int>((widgetPoint.y() - contentsRect().top() - _topMargin) / _fontHeight);
    line = qBound(0, line, _usedLines - 1);

    const qreal x = widgetPoint.x() - contentsRect().left() - _leftMargin;

    // 使用累计宽度计算以支持变宽字符 (CJK)。
    column = 0;
    qreal currentWidth = 0;
    while (column < _usedColumns) {
        const int charIdx = line * _columns + column;
        int w = 1;
        if (charIdx < _image.size()) {
            const quint16 c = _image[charIdx].character;
            if (c == 0) {
                ++column;
                continue;
            }
            w = konsole_wcwidth(c);
            if (w <= 0) w = 1;
        }

        const qreal charPixelWidth = w * _fontWidth;
        if (currentWidth + charPixelWidth > x)
            break;

        currentWidth += charPixelWidth;
        column += w; // 跳过宽字符占用的所有列(含续位列)
    }
    column = qBound(0, column, _usedColumns);
}

int TerminalDisplay::_textWidth(int startColumn, int length, int line) const
{
    if (_fixedFont)
        return length * _fontWidth;

    const QFontMetrics fm(font());
    int result = 0;
    for (int column = 0; column < length; ++column) {
        const int charIdx = line * _columns + startColumn + column;
        if (charIdx < _image.size()) {
            const Character &chr = _image[charIdx];
            if (_fixedFontOriginal && !_isLineChar(chr.character))
                result += fm.horizontalAdvance(QLatin1Char(REPCHAR[0]));
            else
                result += fm.horizontalAdvance(QChar(chr.character));
        }
    }
    return result;
}

bool TerminalDisplay::_isLineChar(uint code) const
{
    return _drawLineChars && (code & 0xFF80) == 0x2500;
}

bool TerminalDisplay::_isLineCharString(const QString &text) const
{
    if (text.isEmpty())
        return false;
    return _drawLineChars && (text.at(0).unicode() & 0xFF80) == 0x2500;
}

QRect TerminalDisplay::_imageToWidget(const QRect &imageArea) const
{
    QRect result;
    result.setLeft(_leftMargin + _fontWidth * imageArea.left());
    result.setTop(_topMargin + _fontHeight * imageArea.top());
    result.setWidth(_fontWidth * imageArea.width());
    result.setHeight(_fontHeight * imageArea.height());
    return result;
}

// ---------------------------------------------------------------------------
// updateImage / updateFilters / updateLineProperties
// ---------------------------------------------------------------------------

void TerminalDisplay::updateImage()
{
    if (!_screenWindow)
        return;

    // 尽量滚动现有图像。
    const int scrollCount = _screenWindow->scrollCount();
    const QRect scrollRegion = _screenWindow->scrollRegion();
    _scrollImage(scrollCount, scrollRegion);
    _screenWindow->resetScrollCount();

    if (_image.isEmpty())
        _updateImageSize();

    Character *newImage = _screenWindow->getImage();
    const int lines = _screenWindow->windowLines();
    const int columns = _screenWindow->windowColumns();
    const int currentLine = _screenWindow->currentLine();
    const int lineCount = _screenWindow->lineCount();

    _setScroll(currentLine, lineCount);

    const int linesToUpdate = qMin(_lines, qMax(0, lines));
    const int columnsToUpdate = qMin(_columns, qMax(0, columns));

    QRegion dirtyRegion;
    _hasBlinker = false;

    for (int y = 0; y < linesToUpdate; ++y) {
        const int currentLineStart = y * _columns;
        const int newLineStart = y * columns;

        if (currentLineStart >= _image.size())
            break;

        const int columnsThisLine = qMin(columnsToUpdate, qMin(_image.size() - currentLineStart, columns));
        Q_UNUSED(newLineStart);

        // 查找该行首个/末个不同位置。
        int startX = 0;
        int endX = columnsThisLine - 1;
        const Character *cur = _image.constData() + currentLineStart;
        const Character *nw = newImage + newLineStart;

        while (startX <= endX && cur[startX] == nw[startX])
            ++startX;
        while (endX >= startX && cur[endX] == nw[endX])
            --endX;

        if (startX <= endX) {
            // 宽字符边界扩展。
            int adjStart = startX;
            while (adjStart > 0 && nw[adjStart].character == 0)
                --adjStart;
            int adjEnd = endX;
            if (adjEnd < columnsThisLine) {
                const quint16 c = nw[adjEnd].character;
                if (c != 0) {
                    const int w = konsole_wcwidth(c);
                    if (w > 1)
                        adjEnd = qMin(columnsThisLine - 1, adjEnd + w - 1);
                }
            }

            const QRect dirtyRect(_leftMargin + contentsRect().left() + adjStart * _fontWidth,
                                  _topMargin + contentsRect().top() + _fontHeight * y,
                                  (adjEnd - adjStart + 1) * _fontWidth,
                                  _fontHeight);
            dirtyRegion |= dirtyRect;

            for (int x = startX; x <= endX; ++x) {
                const int ix = currentLineStart + x;
                if (ix < _image.size())
                    _image[ix] = nw[x];
            }
        }

        // 检查闪烁文本。
        for (int x = 0; x < columnsThisLine; ++x) {
            if (nw[x].rendition & RE_BLINK) {
                _hasBlinker = true;
                break;
            }
        }
    }

    // 清除新图像之外的区域（缩小时）或标记新暴露区域（放大时）。
    if (linesToUpdate < _usedLines) {
        dirtyRegion |= QRect(_leftMargin + contentsRect().left(),
                             _topMargin + contentsRect().top() + _fontHeight * linesToUpdate,
                             _fontWidth * _columns,
                             _fontHeight * (_usedLines - linesToUpdate));
    } else if (linesToUpdate > _usedLines) {
        // 窗口放大：新暴露的行区域需要重绘
        dirtyRegion |= QRect(_leftMargin + contentsRect().left(),
                             _topMargin + contentsRect().top() + _fontHeight * _usedLines,
                             _fontWidth * columnsToUpdate,
                             _fontHeight * (linesToUpdate - _usedLines));
    }
    if (columnsToUpdate < _usedColumns) {
        dirtyRegion |= QRect(_leftMargin + contentsRect().left() + columnsToUpdate * _fontWidth,
                             _topMargin + contentsRect().top(),
                             _fontWidth * (_usedColumns - columnsToUpdate),
                             _fontHeight * _lines);
    } else if (columnsToUpdate > _usedColumns) {
        // 窗口放大：新暴露的列区域需要重绘
        dirtyRegion |= QRect(_leftMargin + contentsRect().left() + _usedColumns * _fontWidth,
                             _topMargin + contentsRect().top(),
                             _fontWidth * (columnsToUpdate - _usedColumns),
                             _fontHeight * linesToUpdate);
    }

    _usedLines = linesToUpdate;
    _usedColumns = columnsToUpdate;

    dirtyRegion |= _previousPreeditRect;

    update(dirtyRegion);

    if (_hasBlinker && !_blinkTimer->isActive())
        _blinkTimer->start(TEXT_BLINK_DELAY);
    if (!_hasBlinker && _blinkTimer->isActive()) {
        _blinkTimer->stop();
        _blinking = false;
    }
}

void TerminalDisplay::updateFilters()
{
    if (_screenWindow)
        _scheduleFilterUpdate();
}

void TerminalDisplay::updateLineProperties()
{
    if (!_screenWindow)
        return;
    _lineProperties = _screenWindow->getLineProperties();
}

void TerminalDisplay::_scrollImage(int lines, const QRect &region)
{
    if (lines == 0 || _image.isEmpty() || !region.isValid() ||
        (region.top() + qAbs(lines)) >= region.bottom() ||
        _lines <= region.height())
        return;

    if (_resizeWidget && _resizeWidget->isVisible())
        _resizeWidget->hide();

    const QRect scrollRegion(region.left(), region.top(), region.width(),
                             qMin(region.bottom(), _lines - 2));

    const int scrollBarWidth = _scrollBar->isHidden() ? 0 : _scrollBar->width();
    const int SCROLLBAR_CONTENT_GAP = scrollBarWidth > 0 ? 1 : 0;

    QRect scrollRect;
    if (_scrollbarLocation == ScrollBarLeft) {
        scrollRect.setLeft(scrollBarWidth + SCROLLBAR_CONTENT_GAP);
        scrollRect.setRight(width());
    } else {
        scrollRect.setLeft(0);
        scrollRect.setRight(width() - scrollBarWidth - SCROLLBAR_CONTENT_GAP);
    }

    const int linesToMove = scrollRegion.height() - qAbs(lines);
    const int cellsToMove = linesToMove * _columns;

    if (lines > 0) {
        const int firstCharPos = scrollRegion.top() * _columns;
        const int lastCharPos = (scrollRegion.top() + qAbs(lines)) * _columns;
        if (firstCharPos + cellsToMove <= _image.size()) {
            for (int i = 0; i < cellsToMove; ++i) {
                if (firstCharPos + i < _image.size() && lastCharPos + i < _image.size())
                    _image[firstCharPos + i] = _image[lastCharPos + i];
            }
        }
        scrollRect.setTop(_topMargin + scrollRegion.top() * _fontHeight);
    } else {
        const int firstCharPos = scrollRegion.top() * _columns;
        const int lastCharPos = (scrollRegion.top() + qAbs(lines)) * _columns;
        if (lastCharPos + cellsToMove <= _image.size()) {
            for (int i = cellsToMove - 1; i >= 0; --i) {
                if (firstCharPos + i < _image.size() && lastCharPos + i < _image.size())
                    _image[lastCharPos + i] = _image[firstCharPos + i];
            }
        }
        scrollRect.setTop(_topMargin + (scrollRegion.top() + qAbs(lines)) * _fontHeight);
    }

    scrollRect.setHeight(linesToMove * _fontHeight);

    if (scrollRect.isValid() && !scrollRect.isEmpty())
        scroll(0, -_fontHeight * lines, scrollRect);
}

// ---------------------------------------------------------------------------
// Resize / geometry
// ---------------------------------------------------------------------------

void TerminalDisplay::resizeEvent(QResizeEvent *event)
{
    _updateImageSize();
    processFilters();
    QWidget::resizeEvent(event);
}

void TerminalDisplay::_updateImageSize()
{
    const QVector<Character> oldImage = _image;
    const int oldLines = _lines;
    const int oldColumns = _columns;

    _makeImage();

    if (!oldImage.isEmpty()) {
        const int lines = qMin(oldLines, _lines);
        const int columns = qMin(oldColumns, _columns);
        for (int line = 0; line < lines; ++line) {
            const int oldStart = line * oldColumns;
            const int newStart = line * _columns;
            for (int col = 0; col < columns; ++col) {
                if (oldStart + col < oldImage.size() && newStart + col < _image.size())
                    _image[newStart + col] = oldImage[oldStart + col];
            }
        }
    }

    if (_screenWindow) {
        _screenWindow->setWindowLines(_lines);
        // 缩放时 image 已完全重建，scroll 优化不再适用——
        // 清除残留 scrollCount 防止 updateImage() 误移 _image 数据。
        _screenWindow->resetScrollCount();
    }

    _resizing = (oldLines != _lines) || (oldColumns != _columns);
    if (_resizing)
        emit changedContentSizeSignal(_contentHeight, _contentWidth);
    _resizing = false;
}

void TerminalDisplay::_propagateSize()
{
    if (_isFixedSize) {
        setSize(_columns, _lines);
        QWidget::setFixedSize(sizeHint());
        if (QWidget *p = parentWidget()) {
            p->adjustSize();
            p->setFixedSize(p->sizeHint());
        }
        return;
    }

    if (!_image.isEmpty())
        _updateImageSize();
}

void TerminalDisplay::_calcGeometry()
{
    _scrollBar->resize(_scrollBar->sizeHint().width(), contentsRect().height());
    const int scrollBarWidth =
        _scrollBar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, _scrollBar)
            ? 0
            : _scrollBar->width();

    if (_scrollbarLocation == NoScrollBar) {
        _leftMargin = _leftBaseMargin;
        _contentWidth = contentsRect().width() - 2 * _leftBaseMargin;
    } else if (_scrollbarLocation == ScrollBarLeft) {
        _leftMargin = _leftBaseMargin + scrollBarWidth;
        _contentWidth = contentsRect().width() - 2 * _leftBaseMargin - scrollBarWidth;
        _scrollBar->move(contentsRect().topLeft());
    } else {
        _leftMargin = _leftBaseMargin;
        _contentWidth = contentsRect().width() - 2 * _leftBaseMargin - scrollBarWidth;
        _scrollBar->move(contentsRect().topRight() - QPoint(_scrollBar->width() - 1, 0));
    }

    _topMargin = _topBaseMargin;
    _contentHeight = contentsRect().height() - 2 * _topBaseMargin + 1;

    if (!_isFixedSize) {
        _columns = qMax(1, _contentWidth / _fontWidth);
        _usedColumns = qMin(_usedColumns, _columns);
        _lines = qMax(1, _contentHeight / _fontHeight);
        _usedLines = qMin(_usedLines, _lines);
    }
}

void TerminalDisplay::_makeImage()
{
    _calcGeometry();

    Q_ASSERT(_lines > 0 && _columns > 0);
    Q_ASSERT(_usedLines <= _lines && _usedColumns <= _columns);

    _imageSize = _lines * _columns;
    _image.resize(_imageSize + 1);
    _clearImage();
}

void TerminalDisplay::_clearImage()
{
    if (_image.isEmpty())
        return;

    const Character defaultChar(QLatin1Char(' ').unicode(),
                                CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR),
                                CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR),
                                DEFAULT_RENDITION);

    for (int i = 0; i < _image.size(); ++i)
        _image[i] = defaultChar;
}

void TerminalDisplay::_showResizeNotification()
{
    if (!_terminalSizeHint || !isVisible())
        return;

    if (_terminalSizeStartup) {
        _terminalSizeStartup = false;
        return;
    }

    if (!_resizeWidget) {
        const QString label = tr("Size: XXX x XXX");
        _resizeWidget = new QLabel(label, this);
        _resizeWidget->setMinimumWidth(_resizeWidget->fontMetrics().horizontalAdvance(label));
        _resizeWidget->setMinimumHeight(_resizeWidget->sizeHint().height());
        _resizeWidget->setAlignment(Qt::AlignCenter);
        _resizeWidget->setStyleSheet(QStringLiteral(
            "background-color:palette(window);border-style:solid;border-width:1px;border-color:palette(dark)"));

        _resizeTimer = new QTimer(this);
        _resizeTimer->setSingleShot(true);
        connect(_resizeTimer, &QTimer::timeout, _resizeWidget, &QLabel::hide);
    }

    _resizeWidget->setText(tr("Size: %1 x %2").arg(_columns).arg(_lines));
    _resizeWidget->move((width() - _resizeWidget->width()) / 2,
                        (height() - _resizeWidget->height()) / 2 + 20);
    _resizeWidget->show();
    _resizeTimer->start(1000);
}

// ---------------------------------------------------------------------------
// Focus events
// ---------------------------------------------------------------------------

void TerminalDisplay::focusInEvent(QFocusEvent *event)
{
    if (_hasBlinkingCursor) {
        const int flashTime = qMax(QApplication::cursorFlashTime(), 1000);
        _blinkCursorTimer->start(flashTime / 2);
    }

    _updateCursor();

    if (_hasBlinker)
        _blinkTimer->start(TEXT_BLINK_DELAY);

    emit termGetFocus();
    QWidget::focusInEvent(event);
}

void TerminalDisplay::focusOutEvent(QFocusEvent *event)
{
    _cursorBlinking = false;
    _updateCursor();
    _blinkCursorTimer->stop();

    if (_blinking)
        _blinkEvent();
    _blinkTimer->stop();

    emit termLostFocus();
    QWidget::focusOutEvent(event);

    if (event->reason() != Qt::PopupFocusReason) {
        if (_screenWindow) {
            _screenWindow->clearSelection();
            updateImage();
        }
    }
}

void TerminalDisplay::showEvent(QShowEvent *event)
{
    emit changedContentSizeSignal(_contentHeight, _contentWidth);
    QWidget::showEvent(event);
}

void TerminalDisplay::hideEvent(QHideEvent *event)
{
    emit changedContentSizeSignal(_contentHeight, _contentWidth);
    QWidget::hideEvent(event);
}

// ---------------------------------------------------------------------------
// Clipboard operations
// ---------------------------------------------------------------------------

void TerminalDisplay::copyClipboard()
{
    if (!_screenWindow)
        return;

    const QString text = _screenWindow->selectedText(_preserveLineBreaks);
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
#ifdef CUBESHELL_PLATFORM_OHOS
        // 系统粘贴板读不回（READ_PASTEBOARD 被拒），复制时同步留一份进程内镜像，
        // 供粘贴回退用（见 internalClipboardText）。
        setInternalClipboardText(text);
#endif
    }
}

#ifdef CUBESHELL_PLATFORM_OHOS
// 进程内剪贴板镜像（仅鸿蒙用）。
static QString g_ohosInternalClipboard;

QString TerminalDisplay::internalClipboardText()
{
    return g_ohosInternalClipboard;
}

void TerminalDisplay::setInternalClipboardText(const QString &text)
{
    g_ohosInternalClipboard = text;
}
#endif


void TerminalDisplay::pasteClipboard()
{
    _emitSelection(false, false);
}

void TerminalDisplay::pasteSelection()
{
    _emitSelection(true, false);
}

void TerminalDisplay::_emitSelection(bool useSelection, bool appendReturn)
{
    if (!_screenWindow)
        return;

    const QClipboard::Mode mode = useSelection ? QClipboard::Selection : QClipboard::Clipboard;
    QString text = QApplication::clipboard()->text(mode);

#ifdef CUBESHELL_PLATFORM_OHOS
    // 系统粘贴板读取被 READ_PASTEBOARD 拒绝而返回空时，回退到进程内镜像，
    // 让「应用内复制→粘贴」可用。系统能读到（如未来拿到权限）则优先用系统的。
    if (text.isEmpty() && !useSelection)
        text = internalClipboardText();
#endif
    if (text.isEmpty())
        return;

    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));

    if (_trimPastedTrailingNewlines) {
        while (text.endsWith(QLatin1Char('\r')))
            text.chop(1);
    }

    if (_confirmMultilinePaste && text.contains(QLatin1Char('\r'))) {
        if (!_multilineConfirmation(text))
            return;
    }

    text = _bracketText(text);

    if (appendReturn)
        text += QLatin1Char('\r');

    QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, text);
    emit keyPressedSignal(&keyEvent, true);

    _screenWindow->clearSelection();

    if (_motionAfterPasting == MoveStartScreenWindow) {
        _screenWindow->setTrackOutput(false);
        _screenWindow->scrollTo(0);
    } else if (_motionAfterPasting == MoveEndScreenWindow) {
        scrollToEnd();
    }
}

QString TerminalDisplay::_bracketText(const QString &text) const
{
    if (bracketedPasteMode() && !_disabledBracketedPasteMode)
        return QStringLiteral("\033[200~") + text + QStringLiteral("\033[201~");
    return text;
}

bool TerminalDisplay::_multilineConfirmation(const QString &text)
{
    QMessageBox confirmation(this);
    confirmation.setWindowTitle(tr("Paste multiline text"));
    confirmation.setText(tr("Are you sure you want to paste this text?"));
    confirmation.setDetailedText(text);
    confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    confirmation.setDefaultButton(QMessageBox::Yes);

    confirmation.exec();
    return confirmation.standardButton(confirmation.clickedButton()) == QMessageBox::Yes;
}

void TerminalDisplay::setSelection(const QString &text)
{
    if (QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(text, QClipboard::Selection);
}

void TerminalDisplay::selectionChanged()
{
    if (_screenWindow)
        emit copyAvailable(!_screenWindow->selectedText(false).isEmpty());
}

// ---------------------------------------------------------------------------
// Flow control warning
// ---------------------------------------------------------------------------

void TerminalDisplay::setFlowControlWarningEnabled(bool enabled)
{
    _flowControlWarningEnabled = enabled;
    if (!enabled)
        outputSuspended(false);
}

void TerminalDisplay::outputSuspended(bool suspended)
{
    if (!_outputSuspendedLabel) {
        _outputSuspendedLabel = new QLabel(
            tr("<qt>Output has been <a href=\"http://en.wikipedia.org/wiki/Flow_control\">suspended</a>"
               " by pressing Ctrl+S. Press <b>Ctrl+Q</b> to resume.</qt>"),
            this);
        _outputSuspendedLabel->setAutoFillBackground(true);
        _outputSuspendedLabel->setBackgroundRole(QPalette::Base);
        _outputSuspendedLabel->setFont(QApplication::font());
        _outputSuspendedLabel->setContentsMargins(5, 5, 5, 5);
        _outputSuspendedLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                                       Qt::LinksAccessibleByKeyboard);
        _outputSuspendedLabel->setOpenExternalLinks(true);
        _outputSuspendedLabel->setVisible(false);

        _gridLayout->addWidget(_outputSuspendedLabel);
        _gridLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding), 1, 0);
    }

    _outputSuspendedLabel->setVisible(suspended);
}

// ---------------------------------------------------------------------------
// Mouse auto-hide
// ---------------------------------------------------------------------------

void TerminalDisplay::autoHideMouseAfter(int delay)
{
    if (delay > -1 && !_hideMouseTimer) {
        _hideMouseTimer = new QTimer(this);
        _hideMouseTimer->setSingleShot(true);
    }

    if ((_mouseAutohideDelay < 0) == (delay < 0)) {
        _mouseAutohideDelay = delay;
        return;
    }

    if (delay > -1)
        connect(_hideMouseTimer, &QTimer::timeout, this, &TerminalDisplay::_hideStaleMouse);
    else if (_hideMouseTimer)
        _hideMouseTimer->disconnect(this);

    _mouseAutohideDelay = delay;
}

void TerminalDisplay::_hideStaleMouse()
{
    if (!underMouse())
        return;
    if (QApplication::activeWindow() && QApplication::activeWindow() != window())
        return;
    if (_scrollBar->underMouse())
        return;

    QApplication::setOverrideCursor(Qt::BlankCursor);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void TerminalDisplay::mousePressEvent(QMouseEvent *ev)
{
    if (_possibleTripleClick && ev->button() == Qt::LeftButton) {
        _mouseTripleClickEvent(ev);
        return;
    }

    if (!contentsRect().contains(ev->pos()))
        return;
    if (!_screenWindow)
        return;

    int charLine, charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn);
    QPoint pos(charColumn, charLine);

    if (ev->button() == Qt::LeftButton) {
        _lineSelectionMode = false;
        _wordSelectionMode = false;

        emit isBusySelecting(true);

        const bool selected = _screenWindow->isSelected(pos.x(), pos.y());

        if ((!_ctrlDrag || (ev->modifiers() & Qt::ControlModifier)) && selected) {
            _dragState = diPending;
            _dragStart = ev->pos();
        } else {
            if ((ev->modifiers() & Qt::ShiftModifier) && !_iPntSel.isNull()) {
                _actSel = 2;
                _extendSelection(ev->pos());
                _dragState = diNone;
            } else {
                _dragState = diNone;

                _preserveLineBreaks = !((ev->modifiers() & Qt::ControlModifier) &&
                                        !(ev->modifiers() & Qt::AltModifier));
                _columnSelectionMode = (ev->modifiers() & Qt::AltModifier) &&
                                       (ev->modifiers() & Qt::ControlModifier);

                if (_mouseMarks || (ev->modifiers() & Qt::ShiftModifier)) {
                    _screenWindow->clearSelection();
                    pos.setY(pos.y() + _scrollBar->value());
                    _iPntSel = _pntSel = pos;
                    _actSel = 1;
                } else {
                    _screenWindow->clearSelection();
                    emit mouseSignal(0, charColumn + 1,
                                     charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 0);
                    pos.setY(pos.y() + _scrollBar->value());
                    _iPntSel = _pntSel = pos;
                    _actSel = 0;
                }
            }

            // 热点激活。
            Filter::HotSpot *hotspot = _filterChain->hotSpotAt(charLine, charColumn);
            if (hotspot && hotspot->type() == Filter::HotSpot::Link)
                hotspot->activate(QStringLiteral("click-action"));
        }
    } else if (ev->button() == Qt::MiddleButton) {
        if (_mouseMarks || (ev->modifiers() & Qt::ShiftModifier))
            _emitSelection(true, ev->modifiers() & Qt::ControlModifier);
        else
            emit mouseSignal(1, charColumn + 1,
                             charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 0);
    } else if (ev->button() == Qt::RightButton) {
        if (_mouseMarks || (ev->modifiers() & Qt::ShiftModifier))
            emit configureRequest(ev->pos());
        else
            emit mouseSignal(2, charColumn + 1,
                             charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 0);
    }
}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent *ev)
{
    if (!_screenWindow)
        return;

    int charLine, charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn);

    if (ev->button() == Qt::LeftButton) {
        emit isBusySelecting(false);

        if (_dragState == diPending) {
            _screenWindow->clearSelection();
        } else {
            if (_actSel > 1)
                setSelection(_screenWindow->selectedText(_preserveLineBreaks));

            _actSel = 0;

            if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier))
                emit mouseSignal(0, charColumn + 1,
                                 charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 2);
        }
        _dragState = diNone;
    }

    if (!_mouseMarks &&
        ((ev->button() == Qt::RightButton && !(ev->modifiers() & Qt::ShiftModifier)) ||
         ev->button() == Qt::MiddleButton)) {
        const int button = (ev->button() == Qt::MiddleButton) ? 1 : 2;
        emit mouseSignal(button, charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 2);
    }
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent *ev)
{
    int charLine, charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn);

    // 热点悬停。
    Filter::HotSpot *hotspot = _filterChain->hotSpotAt(charLine, charColumn);
    if (hotspot && hotspot->type() == Filter::HotSpot::Link) {
        const QRegion previousArea = _mouseOverHotspotArea;
        _mouseOverHotspotArea = _getHotspotRegion(hotspot);
        update(_mouseOverHotspotArea | previousArea);
    } else if (!_mouseOverHotspotArea.isEmpty()) {
        update(_mouseOverHotspotArea);
        _mouseOverHotspotArea = QRegion();
    }

    if (ev->buttons() == Qt::NoButton)
        return;

    if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier)) {
        int button = 3;
        if (ev->buttons() & Qt::LeftButton)
            button = 0;
        else if (ev->buttons() & Qt::MiddleButton)
            button = 1;
        else if (ev->buttons() & Qt::RightButton)
            button = 2;

        emit mouseSignal(button, charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 1);
        return;
    }

    if (_dragState == diPending) {
        const int distance = QApplication::startDragDistance();
        if (qAbs(ev->pos().x() - _dragStart.x()) > distance ||
            qAbs(ev->pos().y() - _dragStart.y()) > distance) {
            emit isBusySelecting(false);
            if (_screenWindow)
                _screenWindow->clearSelection();
            _doDrag();
        }
        return;
    } else if (_dragState == diDragging) {
        return;
    }

    if (_actSel == 0)
        return;
    if (ev->buttons() & Qt::MiddleButton)
        return;

    _extendSelection(ev->pos());
}

QRegion TerminalDisplay::_getHotspotRegion(Filter::HotSpot *hotspot) const
{
    QRegion region;
    int leftMargin = _leftBaseMargin;
    if (_scrollbarLocation == ScrollBarLeft &&
        !_scrollBar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, _scrollBar))
        leftMargin += _scrollBar->width();

    for (int line = hotspot->startLine(); line <= hotspot->endLine(); ++line) {
        const int startCol = (line == hotspot->startLine()) ? hotspot->startColumn() : 0;
        const int endCol = (line == hotspot->endLine()) ? hotspot->endColumn() : _columns;
        const QRect r(startCol * _fontWidth + leftMargin,
                      line * _fontHeight + _topBaseMargin,
                      (endCol - startCol) * _fontWidth,
                      _fontHeight);
        region |= r;
    }
    return region;
}

QRect TerminalDisplay::_getHotspotRect(Filter::HotSpot *spot) const
{
    int leftMargin = _leftBaseMargin;
    if (_scrollbarLocation == ScrollBarLeft &&
        !_scrollBar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, _scrollBar))
        leftMargin += _scrollBar->width();

    return QRect(spot->startColumn() * _fontWidth + 1 + leftMargin,
                 spot->startLine() * _fontHeight + 1 + _topBaseMargin,
                 (spot->endColumn() - spot->startColumn()) * _fontWidth - 1,
                 (spot->endLine() - spot->startLine() + 1) * _fontHeight - 1);
}

void TerminalDisplay::_extendSelection(const QPoint &position)
{
    if (!_screenWindow)
        return;

    const QPoint tl = contentsRect().topLeft();
    const QRect textBounds(tl.x() + _leftMargin,
                           tl.y() + _topMargin,
                           _usedColumns * _fontWidth - 1,
                           _usedLines * _fontHeight - 1);

    QPoint pos = position;
    pos.setX(qBound(textBounds.left(), pos.x(), textBounds.right()));
    pos.setY(qBound(textBounds.top(), pos.y(), textBounds.bottom()));

    if (position.y() > textBounds.bottom()) {
        const int linesBeyond = (position.y() - textBounds.bottom()) / _fontHeight;
        _scrollBar->setValue(_scrollBar->value() + linesBeyond + 1);
    } else if (position.y() < textBounds.top()) {
        const int linesBeyond = (textBounds.top() - position.y()) / _fontHeight;
        _scrollBar->setValue(_scrollBar->value() - linesBeyond - 1);
    }

    int charLine, charColumn;
    getCharacterPosition(pos, charLine, charColumn);
    const QPoint here(charColumn, charLine);

    if (_wordSelectionMode)
        _extendWordSelection(here);
    else if (_lineSelectionMode)
        _extendLineSelection(here);
    else
        _extendCharacterSelection(here);
}

void TerminalDisplay::_extendWordSelection(const QPoint &hereIn)
{
    if (!_screenWindow || _image.isEmpty())
        return;

    QPoint here = hereIn;
    const QPoint iPntSelCorr(_iPntSel.x(), _iPntSel.y() - _scrollBar->value());
    const QPoint pntSelCorr(_pntSel.x(), _pntSel.y() - _scrollBar->value());

    const bool leftNotRight = (here.y() < iPntSelCorr.y() ||
                               (here.y() == iPntSelCorr.y() && here.x() < iPntSelCorr.x()));
    const bool oldLeftNotRight = (pntSelCorr.y() < iPntSelCorr.y() ||
                                  (pntSelCorr.y() == iPntSelCorr.y() && pntSelCorr.x() < iPntSelCorr.x()));
    const bool swapping = leftNotRight != oldLeftNotRight;

    QPoint left = leftNotRight ? here : iPntSelCorr;
    QPoint right = leftNotRight ? iPntSelCorr : here;

    // 扩展到单词开始。
    int lineStart = left.y() * _columns;
    int charIdx = lineStart + left.x();
    if (charIdx >= 0 && charIdx < _image.size()) {
        const QChar selClass = _charClass(_image[charIdx]);
        while (left.x() > 0 ||
               (left.y() > 0 && left.y() - 1 < _lineProperties.size() &&
                (_lineProperties[left.y() - 1] & LINE_WRAPPED))) {
            if (left.x() > 0) {
                if (_charClass(_image[charIdx - 1]) != selClass)
                    break;
                --charIdx;
                left.setX(left.x() - 1);
            } else {
                left.setX(_usedColumns - 1);
                left.setY(left.y() - 1);
                lineStart = left.y() * _columns;
                charIdx = lineStart + left.x();
            }
        }
    }

    // 扩展到单词结束。
    lineStart = right.y() * _columns;
    charIdx = lineStart + right.x();
    if (charIdx >= 0 && charIdx < _image.size()) {
        const QChar selClass = _charClass(_image[charIdx]);
        while (right.x() < _usedColumns - 1 ||
               (right.y() < _usedLines - 1 && right.y() < _lineProperties.size() &&
                (_lineProperties[right.y()] & LINE_WRAPPED))) {
            if (right.x() < _usedColumns - 1) {
                if (_charClass(_image[charIdx + 1]) != selClass)
                    break;
                ++charIdx;
                right.setX(right.x() + 1);
            } else {
                right.setX(0);
                right.setY(right.y() + 1);
                lineStart = right.y() * _columns;
                charIdx = lineStart + right.x();
            }
        }
    }

    QPoint ohere;
    if (leftNotRight) {
        ohere = right;
        here = left;
    } else {
        ohere = left;
        here = right;
    }

    if (_actSel < 2 || swapping)
        _screenWindow->setSelectionStart(ohere.x(), ohere.y(), false);

    _actSel = 2;
    _pntSel = here;
    _pntSel.setY(_pntSel.y() + _scrollBar->value());
    _screenWindow->setSelectionEnd(here.x(), here.y());
}

void TerminalDisplay::_extendLineSelection(const QPoint &hereIn)
{
    if (!_screenWindow)
        return;

    QPoint here = hereIn;
    const QPoint iPntSelCorr(_iPntSel.x(), _iPntSel.y() - _scrollBar->value());

    const bool aboveNotBelow = here.y() < iPntSelCorr.y();

    QPoint above = aboveNotBelow ? here : iPntSelCorr;
    QPoint below = aboveNotBelow ? iPntSelCorr : here;

    while (above.y() > 0 && above.y() - 1 < _lineProperties.size() &&
           (_lineProperties[above.y() - 1] & LINE_WRAPPED))
        above.setY(above.y() - 1);

    while (below.y() < _usedLines - 1 && below.y() < _lineProperties.size() &&
           (_lineProperties[below.y()] & LINE_WRAPPED))
        below.setY(below.y() + 1);

    above.setX(0);
    below.setX(_usedColumns - 1);

    QPoint ohere;
    if (aboveNotBelow) {
        ohere = below;
        here = above;
    } else {
        ohere = above;
        here = below;
    }

    const QPoint newSelBegin(ohere.x(), ohere.y());
    const bool swapping = _tripleSelBegin != newSelBegin;
    _tripleSelBegin = newSelBegin;

    if (_actSel < 2 || swapping)
        _screenWindow->setSelectionStart(ohere.x(), ohere.y(), false);

    _actSel = 2;
    _pntSel = here;
    _pntSel.setY(_pntSel.y() + _scrollBar->value());
    _screenWindow->setSelectionEnd(here.x(), here.y());
}

void TerminalDisplay::_extendCharacterSelection(const QPoint &here)
{
    if (!_screenWindow)
        return;

    const QPoint iPntSelCorr(_iPntSel.x(), _iPntSel.y() - _scrollBar->value());
    const QPoint pntSelCorr(_pntSel.x(), _pntSel.y() - _scrollBar->value());

    const bool leftNotRight = (here.y() < iPntSelCorr.y() ||
                               (here.y() == iPntSelCorr.y() && here.x() < iPntSelCorr.x()));
    const bool oldLeftNotRight = (pntSelCorr.y() < iPntSelCorr.y() ||
                                  (pntSelCorr.y() == iPntSelCorr.y() && pntSelCorr.x() < iPntSelCorr.x()));
    const bool swapping = leftNotRight != oldLeftNotRight;

    const QPoint ohere = iPntSelCorr;
    const int offset = leftNotRight ? 0 : -1;

    if (here == pntSelCorr && _scrollBar->value() == _prevSelScrollValue)
        return;
    _prevSelScrollValue = _scrollBar->value();

    if (here == ohere)
        return;

    if (_actSel < 2 || swapping) {
        if (_columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode)
            _screenWindow->setSelectionStart(ohere.x(), ohere.y(), true);
        else
            _screenWindow->setSelectionStart(ohere.x() - 1 - offset, ohere.y(), false);
    }

    _actSel = 2;
    _pntSel = here;
    _pntSel.setY(_pntSel.y() + _scrollBar->value());

    if (_columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode)
        _screenWindow->setSelectionEnd(here.x(), here.y());
    else
        _screenWindow->setSelectionEnd(here.x() + offset, here.y());

    update();
}

void TerminalDisplay::mouseDoubleClickEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton)
        return;
    if (!_screenWindow)
        return;

    int charLine, charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn);
    const QPoint pos(charColumn, charLine);

    if (!_mouseMarks && !(ev->modifiers() & Qt::ShiftModifier)) {
        emit mouseSignal(0, pos.x() + 1,
                         pos.y() + 1 + _scrollBar->value() - _scrollBar->maximum(), 0);
        return;
    }

    _screenWindow->clearSelection();
    _iPntSel = pos;
    _iPntSel.setY(_iPntSel.y() + _scrollBar->value());

    _wordSelectionMode = true;
    _selectWordAtPosition(pos);

    _possibleTripleClick = true;
    QTimer::singleShot(QApplication::doubleClickInterval(), this, &TerminalDisplay::_tripleClickTimeout);
}

void TerminalDisplay::_selectWordAtPosition(QPoint pos)
{
    if (!_screenWindow || _image.isEmpty())
        return;

    int lineStart = pos.y() * _columns;
    if (lineStart + pos.x() >= _image.size())
        return;

    int charIdx = lineStart + pos.x();
    const QChar selClass = _charClass(_image[charIdx]);

    int startX = pos.x();
    while (startX > 0 ||
           (pos.y() > 0 && pos.y() - 1 < _lineProperties.size() &&
            (_lineProperties[pos.y() - 1] & LINE_WRAPPED))) {
        if (startX > 0) {
            if (_charClass(_image[charIdx - 1]) != selClass)
                break;
            --charIdx;
            --startX;
        } else {
            startX = _usedColumns - 1;
            pos.setY(pos.y() - 1);
            lineStart = pos.y() * _columns;
            charIdx = lineStart + startX;
        }
    }
    const QPoint beginSel(startX, pos.y());

    int endX = pos.x();
    charIdx = lineStart + pos.x();
    while (endX < _usedColumns - 1 ||
           (pos.y() < _usedLines - 1 && pos.y() < _lineProperties.size() &&
            (_lineProperties[pos.y()] & LINE_WRAPPED))) {
        if (endX < _usedColumns - 1) {
            if (_charClass(_image[charIdx + 1]) != selClass)
                break;
            ++charIdx;
            ++endX;
        } else {
            endX = 0;
            pos.setY(pos.y() + 1);
            lineStart = pos.y() * _columns;
            charIdx = lineStart + endX;
        }
    }
    const QPoint endSel(endX, pos.y());

    _actSel = 2;
    _screenWindow->setSelectionStart(beginSel.x(), beginSel.y(), false);
    _screenWindow->setSelectionEnd(endSel.x(), endSel.y());
    setSelection(_screenWindow->selectedText(_preserveLineBreaks));
}

QChar TerminalDisplay::_charClass(const Character &ch) const
{
    // 宽字符第二部分 (character==0) 视为字母数字。
    if (ch.character == 0)
        return QLatin1Char('a');

    const QChar c = (ch.character > 0) ? QChar(ch.character) : QChar(QLatin1Char(' '));

    if (c.isSpace())
        return QLatin1Char(' ');
    if (c.isLetterOrNumber() || _wordCharacters.contains(c))
        return QLatin1Char('a');
    return c;
}

void TerminalDisplay::_mouseTripleClickEvent(QMouseEvent *ev)
{
    if (!_screenWindow)
        return;

    int charLine, charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn);
    _iPntSel = QPoint(charColumn, charLine);

    _screenWindow->clearSelection();
    _lineSelectionMode = true;
    _wordSelectionMode = false;
    _actSel = 2;

    emit isBusySelecting(true);

    while (_iPntSel.y() > 0 && _iPntSel.y() - 1 < _lineProperties.size() &&
           (_lineProperties[_iPntSel.y() - 1] & LINE_WRAPPED))
        _iPntSel.setY(_iPntSel.y() - 1);

    if (_tripleClickMode == SelectWholeLine) {
        _screenWindow->setSelectionStart(0, _iPntSel.y(), false);
        _tripleSelBegin = QPoint(0, _iPntSel.y());
    } else {
        _selectWordAtPosition(_iPntSel);
        _tripleSelBegin = QPoint(_iPntSel.x(), _iPntSel.y());
    }

    while (_iPntSel.y() < _lines - 1 && _iPntSel.y() < _lineProperties.size() &&
           (_lineProperties[_iPntSel.y()] & LINE_WRAPPED))
        _iPntSel.setY(_iPntSel.y() + 1);

    _screenWindow->setSelectionEnd(_columns - 1, _iPntSel.y());
    setSelection(_screenWindow->selectedText(_preserveLineBreaks));

    _iPntSel.setY(_iPntSel.y() + _scrollBar->value());
}

void TerminalDisplay::_tripleClickTimeout()
{
    _possibleTripleClick = false;
}

void TerminalDisplay::wheelEvent(QWheelEvent *ev)
{
    if (ev->angleDelta().y() == 0)
        return;

    // Ctrl(macOS 上 Cmd 默认映射为 ControlModifier) + 滚轮 → 缩放字体，
    // 消费事件避免继续滚动内容。
    // 对应Python: cube-shell.py::SSHQTermWidget.eventFilter (QEvent.Wheel + ControlModifier)
    if (ev->modifiers() & Qt::ControlModifier) {
        emit zoomRequested(ev->angleDelta().y() > 0);
        ev->accept();
        return;
    }

    const int wheelDelta = ev->angleDelta().y();

    if (_mouseMarks) {
        const bool canScroll = _scrollBar->maximum() > 0;
        if (canScroll) {
            const int currentValue = _scrollBar->value();
            const int wheelDegrees = wheelDelta / 8;
            const int linesToScroll = qMax(1, qAbs(wheelDegrees) / 120) * (wheelDelta > 0 ? 1 : -1);

            const int newValue = qBound(0, currentValue - linesToScroll * 3, _scrollBar->maximum());
            if (newValue != currentValue)
                _scrollBar->setValue(newValue);
        } else {
            const int key = (wheelDelta > 0) ? Qt::Key_Up : Qt::Key_Down;
            const int wheelDegrees = qAbs(wheelDelta) / 8;
            const int linesToScroll = qMax(1, wheelDegrees / 120);
            for (int i = 0; i < linesToScroll; ++i) {
                QKeyEvent keyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                emit keyPressedSignal(&keyEvent, false);
            }
        }
    } else {
        int charLine, charColumn;
        getCharacterPosition(ev->position(), charLine, charColumn);
        const int button = (wheelDelta > 0) ? 4 : 5;
        emit mouseSignal(button, charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum(), 0);
    }
}

void TerminalDisplay::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
}

void TerminalDisplay::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// Keyboard events
// ---------------------------------------------------------------------------

void TerminalDisplay::keyPressEvent(QKeyEvent *event)
{
#ifdef CUBESHELL_PLATFORM_OHOS
    // 鸿蒙 PC：菜单栏快捷键在某些焦点/平台路径下可能不触发 ShortcutOverride→shortcut，
    // 这里在终端侧直接兜底处理复制/粘贴，不依赖菜单 shortcut。覆盖 Ctrl+Shift+C/V
    // 与 Ctrl+Insert / Shift+Insert 两套常见终端组合键。只有菜单快捷键没消费该键时
    // keyPressEvent 才会被调到，因此桌面端零影响、也不会与菜单复制重复触发。
    // 修饰键用掩码判断（== 精确匹配会被鸿蒙 QPA 可能附加的 Keypad 等标志挡掉）。
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrlShift = (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)
                           && !(mods & Qt::AltModifier) && !(mods & Qt::MetaModifier);
    const bool ctrlOnly  = (mods & Qt::ControlModifier) && !(mods & Qt::ShiftModifier)
                           && !(mods & Qt::AltModifier) && !(mods & Qt::MetaModifier);
    const bool shiftOnly = (mods & Qt::ShiftModifier) && !(mods & Qt::ControlModifier)
                           && !(mods & Qt::AltModifier) && !(mods & Qt::MetaModifier);
    if (ctrlShift && event->key() == Qt::Key_C) { copyClipboard();  event->accept(); return; }
    if (ctrlShift && event->key() == Qt::Key_V) { pasteClipboard(); event->accept(); return; }
    if (ctrlOnly  && event->key() == Qt::Key_Insert) { copyClipboard();  event->accept(); return; }
    if (shiftOnly && event->key() == Qt::Key_Insert) { pasteClipboard(); event->accept(); return; }
#endif
    emit keyPressedSignal(event, false);
    event->accept();
}

bool TerminalDisplay::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride)
        return _handleShortcutOverrideEvent(static_cast<QKeyEvent *>(event));
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        _scrollBar->setPalette(QApplication::palette());

    return QWidget::event(event);
}

bool TerminalDisplay::_handleShortcutOverrideEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (modifiers != Qt::NoModifier) {
        int modifierCount = 0;
        const auto m = modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier |
                                    Qt::MetaModifier | Qt::KeypadModifier | Qt::GroupSwitchModifier);
        if (m & Qt::ShiftModifier) ++modifierCount;
        if (m & Qt::ControlModifier) ++modifierCount;
        if (m & Qt::AltModifier) ++modifierCount;
        if (m & Qt::MetaModifier) ++modifierCount;
        if (m & Qt::KeypadModifier) ++modifierCount;
        if (m & Qt::GroupSwitchModifier) ++modifierCount;

        if (modifierCount < 2) {
            bool overrideShortcut = false;
            emit overrideShortcutCheck(event, overrideShortcut);
            if (overrideShortcut) {
                event->accept();
                return true;
            }
        }
    }

    switch (event->key()) {
    case Qt::Key_Tab:
    case Qt::Key_Delete:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Backspace:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Escape:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        event->accept();
        return true;
    default:
        break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Input method
// ---------------------------------------------------------------------------

void TerminalDisplay::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty())
        emit sendStringToEmu(event->commitString().toUtf8());

    _preeditString = event->preeditString();
    update(_preeditRect() | _previousPreeditRect);

    event->accept();
}

QVariant TerminalDisplay::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const QPoint cursorPos = _cursorPosition();

    switch (query) {
    case Qt::ImCursorRectangle:
        return _imageToWidget(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
    case Qt::ImFont:
        return font();
    case Qt::ImCursorPosition:
        return cursorPos.x();
    case Qt::ImSurroundingText: {
        QString lineText;
        if (!_image.isEmpty() && cursorPos.y() < _image.size() / _columns) {
            const int lineStart = cursorPos.y() * _columns;
            for (int x = 0; x < _usedColumns; ++x) {
                if (lineStart + x < _image.size()) {
                    const quint16 c = _image[lineStart + x].character;
                    if (c > 0)
                        lineText += QChar(c);
                }
            }
        }
        return lineText;
    }
    case Qt::ImCurrentSelection:
        return QString();
    default:
        break;
    }
    return QVariant();
}

// ---------------------------------------------------------------------------
// Drag and drop
// ---------------------------------------------------------------------------

void TerminalDisplay::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("text/plain")) || event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void TerminalDisplay::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    QString dropText;

    if (!urls.isEmpty()) {
        for (const QUrl &url : urls) {
            QString urlText;
            if (url.isLocalFile())
                urlText = url.toLocalFile();
            else
                urlText = url.toString();

            // 引用 URL。
            urlText.replace(QLatin1Char('\''), QLatin1String("'\\''"));
            dropText += QLatin1Char('\'') + urlText + QStringLiteral("' ");
        }
    } else {
        dropText = event->mimeData()->text();
        dropText.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        dropText.replace(QLatin1Char('\n'), QLatin1Char('\r'));

        if (_trimPastedTrailingNewlines) {
            while (dropText.endsWith(QLatin1Char('\r')))
                dropText.chop(1);
        }

        if (_confirmMultilinePaste && dropText.contains(QLatin1Char('\r'))) {
            if (!_multilineConfirmation(dropText))
                return;
        }
    }

    emit sendStringToEmu(dropText.toUtf8());
}

void TerminalDisplay::_doDrag()
{
    _dragState = diDragging;
    auto *drag = new QDrag(this);
    auto *mimeData = new QMimeData();

    if (QApplication::clipboard()->supportsSelection())
        mimeData->setText(QApplication::clipboard()->text(QClipboard::Selection));

    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
}

bool TerminalDisplay::focusNextPrevChild(bool next)
{
    if (next)
        return false; // 禁用 Tab 焦点导航
    return QWidget::focusNextPrevChild(next);
}

} // namespace Konsole
