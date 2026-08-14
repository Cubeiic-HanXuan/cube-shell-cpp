#pragma once

// TerminalDisplay.h — C++ port of qtermwidget/terminal_display.py
//
// The QWidget that renders the terminal grid: paints characters with colors /
// rendition, manages the selection, scroll region, filter hotspots, keyboard /
// mouse / IME input, the scroll bar and the blinking cursor. Ported from the
// Python PySide6 version, which was itself converted from Konsole /
// QTermWidget (upstream TerminalDisplay.h).
//
// Original copyright:
//   Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <QHash>
#include <QMap>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QScrollBar>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>
#include <QtGlobal>

#include "Character.h"        // Character, LineProperty, LINE_WRAPPED, RE_*
#include "CharacterColor.h"   // ColorEntry, TABLE_COLORS, DEFAULT_FORE/BACK_COLOR
#include "Emulation.h"        // KeyboardCursorShape (already ported)
#include "Filter.h"           // FilterChain, TerminalImageFilterChain, Filter::HotSpot

class QGridLayout;
class QLabel;
class QTimer;

namespace Konsole {

class ScreenWindow; // fwd — the view's window onto the Screen (already ported)

// ---------------------------------------------------------------------------
// Enums (defined here, upstream TerminalDisplay.h owns them; qtermwidget.h
// re-uses them by contract).
// ---------------------------------------------------------------------------

// 对应C++: enum ScrollBarPosition { NoScrollBar, ScrollBarLeft, ScrollBarRight }
enum ScrollBarPosition {
    NoScrollBar    = 0,
    ScrollBarLeft  = 1,
    ScrollBarRight = 2
};

// 对应Python: class MotionAfterPasting(Enum)
enum MotionAfterPasting {
    NoMoveScreenWindow    = 0,
    MoveStartScreenWindow = 1,
    MoveEndScreenWindow   = 2
};

// 对应Python: class BackgroundMode(Enum)
enum BackgroundMode {
    BackgroundNone   = 0,
    BackgroundStretch = 1,
    BackgroundZoom   = 2,
    BackgroundFit    = 3,
    BackgroundCenter = 4
};

// 对应Python: class BellMode(Enum)
enum BellMode {
    SystemBeepBell = 0,
    NotifyBell     = 1,
    VisualBell     = 2,
    NoBell         = 3
};

// 对应Python: class TripleClickMode(Enum)
enum TripleClickMode {
    SelectWholeLine          = 0,
    SelectForwardsFromCursor = 1
};

// 对应Python: class DragState(Enum) (internal drag bookkeeping)
enum DragState {
    diNone     = 0,
    diPending  = 1,
    diDragging = 2
};

// 对应Python: class ScrollBar(QScrollBar)
// Custom scroll bar; upstream overrides enterEvent to un-hide the mouse cursor.
class ScrollBar : public QScrollBar {
    Q_OBJECT
public:
    explicit ScrollBar(QWidget *parent = nullptr);

protected:
    void enterEvent(QEnterEvent *event) override;
};

// 对应Python: class AutoScrollHandler(QObject)
// Scrolls the view while the user drags the mouse outside the widget during
// a selection.
class AutoScrollHandler : public QObject {
    Q_OBJECT
public:
    explicit AutoScrollHandler(QObject *parent);

protected:
    void timerEvent(QTimerEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    int _timerId = 0;
};

// A widget which displays output from a terminal emulation and sends input
// keypresses and mouse activity to the terminal.
//
// 对应C++: class TerminalDisplay : public QWidget
class TerminalDisplay : public QWidget {
    Q_OBJECT

public:
    explicit TerminalDisplay(QWidget *parent = nullptr);
    ~TerminalDisplay() override;

    // --- screen window -------------------------------------------------
    // 对应C++: ScreenWindow* screenWindow() const
    ScreenWindow *screenWindow() const { return _screenWindow; }
    // 对应C++: void setScreenWindow(ScreenWindow* window)
    void setScreenWindow(ScreenWindow *window);

    // --- color table (cube-shell reads this via the accessor) ----------
    // 对应C++: const ColorEntry* colorTable() const
    const ColorEntry *colorTable() const { return _colorTable; }
    // cube-shell 钩子:可写的颜色表访问器 (Python: self._color_table)。
    ColorEntry *colorTableWritable() { return _colorTable; }
    // 对应C++: void setColorTable(const ColorEntry table[])
    void setColorTable(const ColorEntry table[]);

    // 对应C++: void setBackgroundColor(const QColor& color)
    void setBackgroundColor(const QColor &color);
    // 对应C++: void setForegroundColor(const QColor& color)
    void setForegroundColor(const QColor &color);
    // 对应C++: void setSuppressProgramBackgroundColors(bool)
    void setSuppressProgramBackgroundColors(bool suppress);

    // 对应C++: void setRandomSeed(int) / int randomSeed() const
    void setRandomSeed(int seed) { _randomSeed = seed; }
    int randomSeed() const { return _randomSeed; }

    // 对应C++: void setOpacity(qreal)
    void setOpacity(qreal opacity);
    // 对应Python: setBackgroundImage(str)
    void setBackgroundImage(const QString &backgroundImage);
    // 对应Python: setBackgroundMode(BackgroundMode)
    void setBackgroundMode(BackgroundMode mode) { _backgroundMode = mode; }

    // --- scroll bar ------------------------------------------------------
    // 对应C++: void setScrollBarPosition(ScrollBarPosition)
    void setScrollBarPosition(ScrollBarPosition position);
    // 对应C++: void setScroll(int cursor, int lines)
    void setScroll(int cursor, int lines);

    // --- filters ---------------------------------------------------------
    // 对应C++: FilterChain* filterChain() const
    FilterChain *filterChain() const { return _filterChain; }
    // 对应C++: void processFilters()
    void processFilters();
    // 对应C++: QList<QAction*> filterActions(const QPoint& position)
    QList<QAction *> filterActions(const QPoint &position);

    // --- cursor ----------------------------------------------------------
    // 对应C++: void setKeyboardCursorShape(KeyboardCursorShape)
    void setKeyboardCursorShape(KeyboardCursorShape shape) { _cursorShape = shape; }
    KeyboardCursorShape keyboardCursorShape() const { return _cursorShape; }
    // 对应C++: void setKeyboardCursorColor(bool useForegroundColor, const QColor&)
    void setKeyboardCursorColor(bool useForegroundColor, const QColor &color = QColor());
    QColor keyboardCursorColor() const { return _cursorColor; }
    // 对应C++: void setBlinkingCursor(bool)
    void setBlinkingCursor(bool blink);
    bool blinkingCursor() const { return _hasBlinkingCursor; }
    // 对应C++: void setBlinkingTextEnabled(bool)
    void setBlinkingTextEnabled(bool blink);

    // --- font ------------------------------------------------------------
    // 对应C++: void setVTFont(const QFont&)
    void setVTFont(const QFont &font);
    // Ignore setFont() not coming from setVTFont().
    using QWidget::setFont;
    void setFont(const QFont &) { /* intentionally empty — Python override */ }
    QFont getVTFont() const { return font(); }

private:
    void _fontChange(const QFont &font);

public:

    // --- size / layout -----------------------------------------------------
    int lines() const { return _lines; }
    int columns() const { return _columns; }
    int fontHeight() const { return _fontHeight; }
    int fontWidth() const { return _fontWidth; }
    int fontAscent() const { return _fontAscent; }
    // 对应C++: void setSize(int cols, int lins)
    void setSize(int cols, int lins);
    // 对应C++: void setFixedSize(int cols, int lins)
    void setFixedSize(int cols, int lins);
    QSize sizeHint() const override;

    // --- word selection ----------------------------------------------------
    void setWordCharacters(const QString &wc) { _wordCharacters = wc; }
    QString wordCharacters() const { return _wordCharacters; }
    void setTripleClickMode(TripleClickMode mode) { _tripleClickMode = mode; }
    TripleClickMode tripleClickMode() const { return _tripleClickMode; }

    // --- bell --------------------------------------------------------------
    // 对应C++: void setBellMode(int mode)
    void setBellMode(int mode) { _bellMode = static_cast<BellMode>(mode); }
    int bellMode() const { return static_cast<int>(_bellMode); }

    // --- line spacing / margins --------------------------------------------
    void setLineSpacing(int spacing);
    int lineSpacing() const { return _lineSpacing; }
    void setMargin(int margin) { _topBaseMargin = margin; _leftBaseMargin = margin; }
    int margin() const { return _topBaseMargin; }

    // --- mouse / selection ---------------------------------------------------
    // 对应C++: void setUsesMouse(bool)
    void setUsesMouse(bool usesMouse);
    bool usesMouse() const { return _mouseMarks; }

    // cube-shell:主屏/备用屏切换通知 (Emulation.primaryScreenInUse 驱动)。
    void setPrimaryScreenInUse(bool primary);
    // cube-shell:焦点上报模式(?1004)切换通知。
    void setReportFocusMode(bool enabled);

    void setBracketedPasteMode(bool enabled) { _bracketedPasteMode = enabled; }
    bool bracketedPasteMode() const { return _bracketedPasteMode; }
    void disableBracketedPasteMode(bool disable) { _disabledBracketedPasteMode = disable; }
    bool bracketedPasteModeIsDisabled() const { return _disabledBracketedPasteMode; }

    void setCtrlDrag(bool enabled) { _ctrlDrag = enabled; }
    bool ctrlDrag() const { return _ctrlDrag; }

    // 对应C++: QRect calculateTextArea(...) — cube-shell/上游公共方法
    QRect calculateTextArea(int topLeftX, int topLeftY, int startColumn, int line, int length);

    // 对应C++: void bracketText(QString& text) const (returns processed text here)
    QString bracketText(const QString &text) const;

    // 对应C++: QChar charClass(const Character&) const
    QChar charClass(const Character &ch) const;

    // --- paste / motion ------------------------------------------------------
    void setMotionAfterPasting(MotionAfterPasting action) { _motionAfterPasting = action; }
    int motionAfterPasting() const { return static_cast<int>(_motionAfterPasting); }
    void setConfirmMultilinePaste(bool confirm) { _confirmMultilinePaste = confirm; }
    void setTrimPastedTrailingNewlines(bool trim) { _trimPastedTrailingNewlines = trim; }

    // --- flow control warning -------------------------------------------------
    void setFlowControlWarningEnabled(bool enabled);
    bool flowControlWarningEnabled() const { return _flowControlWarningEnabled; }

    // --- terminal size hint -----------------------------------------------------
    void setTerminalSizeHint(bool enabled) { _terminalSizeHint = enabled; }
    bool terminalSizeHint() const { return _terminalSizeHint; }
    void setTerminalSizeStartup(bool enabled) { _terminalSizeStartup = enabled; }

    // --- BiDi -------------------------------------------------------------------
    void setBidiEnabled(bool enabled) { _bidiEnabled = enabled; }
    bool isBidiEnabled() const { return _bidiEnabled; }

    // --- drawing settings ---------------------------------------------------------
    void setDrawLineChars(bool draw) { _drawLineChars = draw; }
    void setBoldIntense(bool bold) { _boldIntense = bold; }
    bool getBoldIntense() const { return _boldIntense; }

    // --- static display settings ----------------------------------------------------
    static void setAntialias(bool antialias) { s_antialiasText = antialias; }
    static bool antialias() { return s_antialiasText; }
    static void setTransparencyEnabled(bool enabled) { s_haveTransparency = enabled; }

    // --- mouse auto-hide ------------------------------------------------------------
    void autoHideMouseAfter(int delay);
    int mouseAutohideDelay() const { return _mouseAutohideDelay; }

    // --- character position ---------------------------------------------------------
    // 对应Python: getCharacterPosition(QPointF) -> (line, column)
    void getCharacterPosition(const QPointF &widgetPoint, int &line, int &column) const;

public slots:
    // 对应C++: void updateImage()
    void updateImage();
    // 对应C++: void updateFilters()
    void updateFilters();
    // 对应C++: void updateLineProperties()
    void updateLineProperties();

    // 对应C++: void scrollToEnd()
    void scrollToEnd();
    // 对应C++: void scrollBarPositionChanged(int value)
    void scrollBarPositionChanged(int value);

    // 对应C++: void copyClipboard()
    void copyClipboard();
    // 对应C++: void pasteClipboard()
    void pasteClipboard();
    // 对应C++: void pasteSelection()
    void pasteSelection();

#ifdef CUBESHELL_PLATFORM_OHOS
    // 鸿蒙：系统粘贴板的「读」被 READ_PASTEBOARD（受限 ACL 权限）拒绝，普通应用
    // 声明了也拿不到；但「写」(SetData) 是放行的。因此复制时在本进程留一份镜像，
    // 粘贴/右键菜单在系统读不到内容时回退到这份镜像，保证「应用内复制→粘贴」可用。
    // 跨应用粘贴（从别的 App 复制贴进终端）仍受鸿蒙权限限制，此镜像帮不上。
    static QString internalClipboardText();
    static void setInternalClipboardText(const QString &text);
#endif

    // 对应C++: void setSelection(const QString&)
    void setSelection(const QString &text);
    // 对应C++: void selectionChanged()
    void selectionChanged();

    // 对应C++: void bell(const QString& message)
    void bell(const QString &message = QString());
    // 对应C++: void outputSuspended(bool suspended)
    void outputSuspended(bool suspended);

signals:
    // 对应C++: void keyPressedSignal(QKeyEvent* keyEvent, bool fromPaste)
    void keyPressedSignal(QKeyEvent *keyEvent, bool fromPaste);
    // 对应C++: void mouseSignal(int button, int column, int line, int eventType)
    void mouseSignal(int button, int column, int line, int eventType);
    // 对应C++: void changedFontMetricSignal(int height, int width)
    void changedFontMetricSignal(int height, int width);
    // 对应C++: void changedContentSizeSignal(int height, int width)
    void changedContentSizeSignal(int height, int width);
    // 对应C++: void configureRequest(const QPoint& position)
    void configureRequest(const QPoint &position);
    // 对应C++: void overrideShortcutCheck(QKeyEvent* keyEvent, bool& override)
    void overrideShortcutCheck(QKeyEvent *keyEvent, bool &override);
    // 对应C++: void isBusySelecting(bool)
    void isBusySelecting(bool);
    // 对应C++: void sendStringToEmu(const char*, int) — cube-shell 走 QByteArray
    void sendStringToEmu(const QByteArray &data);
    // 对应C++: void copyAvailable(bool)
    void copyAvailable(bool);
    // 对应C++: void termGetFocus()
    void termGetFocus();
    // 对应C++: void termLostFocus()
    void termLostFocus();
    // 对应C++: void notifyBell(const QString&)
    void notifyBell(const QString &message);
    // 对应C++: void usesMouseChanged()
    void usesMouseChanged();
    // Ctrl/Cmd + 滚轮请求缩放终端字体，由 QTermWidget 处理。
    // 对应Python: cube-shell.py::SSHQTermWidget.eventFilter (QEvent.Wheel + ControlModifier)
    void zoomRequested(bool zoomIn);

protected:
    // --- Qt event overrides ---------------------------------------------------
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private slots:
    void _onOutputChanged();
    void _onSelectionChanged();
    void _onScrolled(int line);
    void _flushPendingOutputUpdates();
    void _flushPendingFilterUpdates();
    void _delayedScrollUpdate();
    void _blinkEvent();
    void _blinkCursorEvent();
    void _enableBell();
    void _swapColorTable();
    void _tripleClickTimeout();
    void _hideStaleMouse();

private:
    // --- initialization -----------------------------------------------------
    void _initWidget();
    void _setupTimers();

    // --- scheduling / coalescing --------------------------------------------
    void _scheduleOutputUpdate();
    void _scheduleFilterUpdate();

    // --- scrolling ------------------------------------------------------------
    void _setScroll(int cursor, int lines);
    void _scrollImage(int lines, const QRect &region);

    // --- painting -------------------------------------------------------------
    void _drawBackground(QPainter &painter, const QRect &rect, const QColor &backgroundColor, bool useOpacity);
    void _drawBackgroundZoom(QPainter &painter, const QRect &cr);
    void _drawBackgroundFit(QPainter &painter, const QRect &cr);
    void _drawBackgroundCenter(QPainter &painter, const QRect &cr);
    void _drawContents(QPainter &painter, const QRect &rect);
    void _drawLine(QPainter &painter, int y, int lux, int rlx, int tlx, int tly, const QFontMetrics &fm);
    void _drawTextFragment(QPainter &painter, const QRect &rect, const QString &text,
                           const Character &style, bool invertColors, const QColor &overrideFg);
    void _drawCharacters(QPainter &painter, const QRect &rect, const QString &text,
                         const Character &style, bool invertColors);
    bool _drawCursor(QPainter &painter, const QRect &rect, const QColor &foregroundColor,
                     const QColor &backgroundColor);
    void _drawLineCharString(QPainter &painter, int x, int y, const QString &text,
                             const Character &attributes);
    void _calcDrawTextAdditionHeight(QPainter &painter);
    void _paintFilters(QPainter &painter);
    void _drawHotspotHighlight(QPainter &painter, Filter::HotSpot *spot);
    void _drawInputMethodPreeditString(QPainter &painter, const QRect &rect);

    // --- syntax highlight (cube-shell) ------------------------------------------
    // 返回 {line: {col: QColor}} 前景着色映射;备用屏/交互式 TUI 返回空。
    QMap<int, QMap<int, QColor>> _computeHighlightMap();

    // --- selection ------------------------------------------------------------
    struct SelectionCache {
        bool block;
        int topCol, topLine, botCol, botLine;
    };
    bool _computeSelectionCache(SelectionCache &out) const;
    bool _selectionRangeForLine(int y, int &left, int &right) const;
    void _extendSelection(const QPoint &position);
    void _extendWordSelection(const QPoint &here);
    void _extendLineSelection(const QPoint &here);
    void _extendCharacterSelection(const QPoint &here);
    void _selectWordAtPosition(QPoint pos);
    void _mouseTripleClickEvent(QMouseEvent *event);
    QChar _charClass(const Character &ch) const;

    // --- hotspots -------------------------------------------------------------
    QRegion _hotSpotRegion() const;
    QRegion _getHotspotRegion(Filter::HotSpot *hotspot) const;
    QRect _getHotspotRect(Filter::HotSpot *spot) const;

    // --- geometry / image -------------------------------------------------------
    void _updateImageSize();
    void _propagateSize();
    void _calcGeometry();
    void _makeImage();
    void _clearImage();
    QRect _imageToWidget(const QRect &imageArea) const;
    int _textWidth(int startColumn, int length, int line) const;
    bool _isLineChar(uint code) const;
    bool _isLineCharString(const QString &text) const;
    QRect _preeditRect() const;
    void _showResizeNotification();

    // --- color helpers ----------------------------------------------------------
    static qreal _brightness(const QColor &color);
    static QColor _bestBwForBg(const QColor &bg);
    static QColor _getSmartCursorColor(const QColor &fgColor, const QColor &bgColor);
    static void _cursorPaintColors(const QColor &effectiveFg, const QColor &effectiveBg,
                                   const QColor &configuredCursor, QColor &fill, QColor &text);

    // --- paste / drag -------------------------------------------------------------
    void _emitSelection(bool useSelection, bool appendReturn);
    QString _bracketText(const QString &text) const;
    bool _multilineConfirmation(const QString &text);
    void _doDrag();
    bool _handleShortcutOverrideEvent(QKeyEvent *event);

    // --- misc -----------------------------------------------------------------------
    void _updateCursor();
    QPoint _cursorPosition() const;
    void _enableBellImpl();

    // =========================================================================
    // Members
    // =========================================================================

    ScreenWindow *_screenWindow = nullptr;

    // Character image (grid) — owned by the view.
    QVector<Character> _image;
    int _imageSize = 0;
    QSize _size;
    int _lines = 24;
    int _columns = 80;
    int _usedLines = 0;
    int _usedColumns = 0;
    int _contentHeight = 0;
    int _contentWidth = 0;
    int _fontHeight = 15;
    int _fontWidth = 7;
    int _fontAscent = 13;
    bool _boldIntense = true;
    QVector<LineProperty> _lineProperties;

    // Color and drawing. cube-shell 读取的颜色表。
    ColorEntry _colorTable[TABLE_COLORS];
    uint _randomSeed = 0;
    int _margin = 1;
    int _topMargin = 1;
    int _leftMargin = 1;
    int _drawTextAdditionHeight = 0;

    // Layout
    QGridLayout *_gridLayout = nullptr;
    bool _resizing = false;
    bool _terminalSizeHint = false;
    bool _terminalSizeStartup = true;
    bool _bidiEnabled = true;
    bool _mouseMarks = true;
    bool _bracketedPasteMode = false;
    bool _disabledBracketedPasteMode = false;

    // Selection
    QPoint _iPntSel;
    QPoint _pntSel;
    QPoint _tripleSelBegin;
    int _actSel = 0;
    bool _wordSelectionMode = false;
    bool _lineSelectionMode = false;
    bool _preserveLineBreaks = false;
    bool _columnSelectionMode = false;
    SelectionCache _selectionCache{};
    bool _hasSelectionCache = false;
    int _prevSelScrollValue = -1;

    // 语法高亮 (cube-shell)
    QMap<int, QMap<int, QColor>> _highlightMap;
    bool _hasHighlightMap = false;
    bool _primaryScreenInUse = true;
    bool _programReportFocus = false;

    // Scrollbar
    ScrollBarPosition _scrollbarLocation = NoScrollBar;
    ScrollBar *_scrollBar = nullptr;
    int _pendingScrollValue = 0;

    // Other settings
    QString _wordCharacters = QStringLiteral(":@-./_~");
    BellMode _bellMode = SystemBeepBell;
    bool _allowBell = true;

    // Blinking
    bool _blinking = false;
    bool _hasBlinker = false;
    bool _cursorBlinking = false;
    bool _hasBlinkingCursor = false;
    bool _allowBlinkingText = true;
    bool _ctrlDrag = false;
    TripleClickMode _tripleClickMode = SelectWholeLine;
    bool _isFixedSize = false;

    // Timers
    QTimer *_blinkTimer = nullptr;
    QTimer *_blinkCursorTimer = nullptr;

    // UI elements
    bool _possibleTripleClick = false;
    QLabel *_resizeWidget = nullptr;
    QTimer *_resizeTimer = nullptr;
    bool _flowControlWarningEnabled = false;
    QLabel *_outputSuspendedLabel = nullptr;

    // Display settings
    int _lineSpacing = 0;
    bool _colorsInverted = false;
    qreal _opacity = 1.0;
    QPixmap _backgroundImage;
    BackgroundMode _backgroundMode = BackgroundNone;
    bool _suppressProgramBackgroundColors = false;

    // Filter chain
    TerminalImageFilterChain *_filterChain = nullptr;
    QRegion _mouseOverHotspotArea;
    bool _pendingUpdateImage = false;
    bool _pendingUpdateLineProperties = false;
    bool _pendingUpdateFilters = false;
    int _outputUpdateIntervalMs = 16;
    int _filterUpdateIntervalMs = 200;
    QTimer *_outputUpdateTimer = nullptr;
    QTimer *_filterUpdateTimer = nullptr;
    QTimer *_scrollUpdateTimer = nullptr;

    // Cursor
    KeyboardCursorShape _cursorShape = KeyboardCursorShape::BlockCursor;
    QColor _cursorColor;

    // Paste settings
    MotionAfterPasting _motionAfterPasting = NoMoveScreenWindow;
    bool _confirmMultilinePaste = false;
    bool _trimPastedTrailingNewlines = false;

    // Input method
    QString _preeditString;
    QRect _previousPreeditRect;

    // Font settings
    bool _fixedFont = true;
    bool _fixedFontOriginal = true;
    int _leftBaseMargin = 1;
    int _topBaseMargin = 1;
    bool _drawLineChars = true;
    int _mouseAutohideDelay = -1;
    QTimer *_hideMouseTimer = nullptr;

    // Drag info
    DragState _dragState = diNone;
    QPoint _dragStart;

    // Auto-scroll handler (created lazily, parented to this).
    AutoScrollHandler *_autoScrollHandler = nullptr;

    // Class-level statics
    static bool s_antialiasText;
    static bool s_haveTransparency;
};

} // namespace Konsole
