#pragma once

// Screen.h — C++ port of qtermwidget/screen.py
//
// The terminal screen model: a 2D grid of Character cells plus a scrollback
// History, the cursor, scrolling margins and the selection. All CSI / OSC
// operations act on this class. Ported from the Python PySide6 version, which
// was itself converted from Konsole / QTermWidget (upstream Screen.h).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robert.knight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QRect>
#include <QSet>
#include <QVector>
#include <QtGlobal>

#include "Character.h"
#include "CharacterColor.h"
#include "History.h"
#include "konsole_wcwidth.h"

namespace Konsole {

class TerminalCharacterDecoder; // fwd — used by the *ToStream() methods

// Mode constants (indices into currentModes / savedModes).
// 对应C++: #define MODE_Origin 0 等
inline constexpr int MODE_Origin  = 0;
inline constexpr int MODE_Wrap    = 1;
inline constexpr int MODE_Insert  = 2;
inline constexpr int MODE_Screen  = 3;
inline constexpr int MODE_Cursor  = 4;
inline constexpr int MODE_NewLine = 5;
inline constexpr int MODES_SCREEN = 6;

// Converts an (x,y) position to a linear image offset.
// 对应C++: #define loc(x,y) ((y)*columns+(x))
// Implemented as a free inline taking columns explicitly (Python: loc(x,y,columns)).
inline int loc(int x, int y, int columns) { return y * columns + x; }

// Saved cursor state (cursor position + rendition + colors).
// 对应C++: struct SavedState
struct SavedState {
    int cursorColumn = 0;
    int cursorLine   = 0;
    quint16 rendition = DEFAULT_RENDITION;
    CharacterColor foreground = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    CharacterColor background = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
};

// 对应C++: class Screen
class Screen {
public:
    // 对应C++: Screen::Screen(int l, int c)
    Screen(int lines, int columns);
    // 对应C++: virtual ~Screen()
    ~Screen();

    // --- cursor movement -------------------------------------------------
    // 对应C++: void Screen::cursorUp(int n)
    void cursorUp(int n = 1);
    // 对应C++: void Screen::cursorDown(int n)
    void cursorDown(int n = 1);
    // 对应C++: void Screen::cursorLeft(int n)
    void cursorLeft(int n = 1);
    // 对应C++: void Screen::cursorRight(int n)
    void cursorRight(int n = 1);
    // 对应C++: void Screen::cursorNextLine(int n)
    void cursorNextLine(int n = 1);
    // 对应C++: void Screen::cursorPreviousLine(int n)
    void cursorPreviousLine(int n = 1);
    // 对应C++: void Screen::setCursorX(int x)   (1-based)
    void setCursorX(int x);
    // 对应C++: void Screen::setCursorY(int y)   (1-based)
    void setCursorY(int y);
    // 对应C++: void Screen::setCursorYX(int y, int x)
    void setCursorYX(int y, int x);
    // 对应C++: int Screen::getCursorX() const   (0-based)
    int getCursorX() const { return cuX; }
    // 对应C++: int Screen::getCursorY() const   (0-based)
    int getCursorY() const { return cuY; }
    // 对应C++: void Screen::home()
    void home();
    // 对应C++: void Screen::toStartOfLine()
    void toStartOfLine();

    // --- margins ---------------------------------------------------------
    // 对应C++: void Screen::setMargins(int top, int bot)
    void setMargins(int topLine, int bottomLine);
    // 对应C++: int Screen::topMargin() const
    int topMargin() const { return _topMargin; }
    // 对应C++: int Screen::bottomMargin() const
    int bottomMargin() const { return _bottomMargin; }
    // 对应C++: void Screen::setDefaultMargins()
    void setDefaultMargins();

    // --- modes -----------------------------------------------------------
    // 对应C++: void Screen::setMode(int m)
    void setMode(int mode);
    // 对应C++: void Screen::resetMode(int m)
    void resetMode(int mode);
    // 对应C++: void Screen::saveMode(int m)
    void saveMode(int mode);
    // 对应C++: void Screen::restoreMode(int m)
    void restoreMode(int mode);
    // 对应C++: bool Screen::getMode(int m) const
    bool getMode(int mode) const { return currentModes[mode]; }

    // --- cursor save/restore ---------------------------------------------
    // 对应C++: void Screen::saveCursor()
    void saveCursor();
    // 对应C++: void Screen::restoreCursor()
    void restoreCursor();

    // --- tab stops -------------------------------------------------------
    // 对应C++: void Screen::initTabStops()
    void initTabStops();
    // 对应C++: void Screen::clearTabStops()
    void clearTabStops();
    // 对应C++: void Screen::changeTabStop(bool set)
    void changeTabStop(bool setStop);
    // 对应C++: void Screen::tab(int n)
    void tab(int n = 1);
    // 对应C++: void Screen::backtab(int n)
    void backtab(int n = 1);

    // --- basic attributes ------------------------------------------------
    // 对应C++: int Screen::getLines() const
    int getLines() const { return lines; }
    // 对应C++: int Screen::getColumns() const
    int getColumns() const { return columns; }
    // 对应C++: int Screen::getHistLines() const
    int getHistLines() const;

    // --- scrollback history ----------------------------------------------
    // 对应C++: void Screen::setScroll(const HistoryType& t, bool copyPreviousScroll)
    void setScroll(const HistoryType &historyType, bool copyPreviousScroll = true);
    // 对应C++: const HistoryType& Screen::getScroll() const
    const HistoryType &getScroll() const;
    // 对应C++: bool Screen::hasScroll() const
    bool hasScroll() const;

    // --- selection -------------------------------------------------------
    // 对应C++: void Screen::clearSelection()
    void clearSelection();
    // 对应C++: void Screen::setSelectionStart(const int x, const int y, const bool mode)
    void setSelectionStart(int column, int line, bool blockMode);
    // 对应C++: void Screen::setSelectionEnd(const int x, const int y)
    void setSelectionEnd(int column, int line);
    // 对应C++: bool Screen::isSelected(const int x, const int y) const
    bool isSelected(int column, int line) const;
    // 对应C++: bool Screen::isSelectionValid() const
    bool isSelectionValid() const;
    // 是否为块(列)选择模式。TerminalDisplay 绘制选区时需要 (Python: screen.blockSelectionMode)。
    bool isBlockSelectionMode() const { return blockSelectionMode; }

    // --- rendition / color ------------------------------------------------
    // 对应C++: void Screen::updateEffectiveRendition()
    void updateEffectiveRendition();
    // 对应C++: void Screen::setForegroundColor(int space, int color)
    void setForegroundColor(int space, int color);
    // 对应C++: void Screen::setBackgroundColor(int space, int color)
    void setBackgroundColor(int space, int color);
    // 对应C++: void Screen::setRendition(int rendition)
    void setRendition(int rendition);
    // 对应C++: void Screen::resetRendition(int rendition)
    void resetRendition(int rendition);
    // 对应C++: void Screen::setDefaultRendition()
    void setDefaultRendition();

    // --- clear / reset ----------------------------------------------------
    // 对应C++: void Screen::clear()
    void clear();
    // 对应C++: void Screen::reset(bool clearScreen = true)
    void reset(bool clearScreen = true);
    // 对应C++: void Screen::clearEntireScreen()
    void clearEntireScreen();
    // 对应C++: void Screen::clearToEndOfScreen()
    void clearToEndOfScreen();
    // 对应C++: void Screen::clearToBeginOfScreen()
    void clearToBeginOfScreen();
    // 对应C++: void Screen::clearEntireLine()
    void clearEntireLine();
    // 对应C++: void Screen::clearToEndOfLine()
    void clearToEndOfLine();
    // 对应C++: void Screen::clearToBeginOfLine()
    void clearToBeginOfLine();
    // 对应C++: void Screen::helpAlign()
    void helpAlign();
    // 对应C++: void Screen::backspace()
    void backspace();

    // --- internal helpers (public in upstream) -----------------------------
    // 对应C++: void Screen::clearImage(int loca, int loce, char c)
    void clearImage(int loca, int loce, QChar c);
    // 对应C++: void Screen::addHistLine()
    void addHistLine();
    // 对应C++: void Screen::scrollUp(int from, int n)  (region, no history)
    void scrollUp(int fromLine, int n);
    // 对应C++: void Screen::moveImage(int dest, int sourceBegin, int sourceEnd)
    void moveImage(int dest, int sourceBegin, int sourceEnd);

    // --- scrolling counters -----------------------------------------------
    // 对应C++: int Screen::scrolledLines() const
    int scrolledLines() const { return _scrolledLines; }
    // 对应C++: int Screen::droppedLines() const
    int droppedLines() const { return _droppedLines; }
    // 对应C++: void Screen::resetScrolledLines()
    void resetScrolledLines() { _scrolledLines = 0; }
    // 对应C++: void Screen::resetDroppedLines()
    void resetDroppedLines() { _droppedLines = 0; }
    // 对应C++: QRect Screen::lastScrolledRegion() const
    QRect lastScrolledRegion() const { return _lastScrolledRegion; }

    // 对应C++: static void Screen::fillWithDefaultChar(Character* dest, int count)
    static void fillWithDefaultChar(Character *dest, int count);

    // --- character display --------------------------------------------------
    // 对应C++: void Screen::displayCharacter(wchar_t c)
    // Accepts a full code point (uint); surrogates handled by callers.
    void displayCharacter(uint c);

    // --- editing -------------------------------------------------------------
    // 对应C++: void Screen::eraseChars(int n)
    void eraseChars(int n);
    // 对应C++: void Screen::deleteChars(int n)
    void deleteChars(int n);
    // 对应C++: void Screen::insertChars(int n)
    void insertChars(int n);
    // 对应C++: void Screen::repeatChars(int count)
    void repeatChars(int count);
    // 对应C++: void Screen::deleteLines(int n)
    void deleteLines(int n);
    // 对应C++: void Screen::insertLines(int n)
    void insertLines(int n);

    // --- scrolling (region / index) -------------------------------------------
    // 对应C++: void Screen::index()
    void index();
    // 对应C++: void Screen::reverseIndex()
    void reverseIndex();
    // 对应C++: void Screen::nextLine()
    void nextLine();
    // 对应C++: void Screen::newLine()
    void newLine();
    // 对应C++: void Screen::scrollUp(int n)   (region, adds to history) -> scrollUpRegion
    void scrollUpRegion(int n = 1);
    // 对应C++: void Screen::scrollDown(int n) (region) -> scrollDownRegion
    void scrollDownRegion(int n = 1);
    // 对应C++: void Screen::scrollDown(int from, int n) (region, no history)
    void scrollDown(int fromLine, int n);

    // --- image copy -------------------------------------------------------------
    // 对应C++: void Screen::getImage(Character* dest, int size, int startLine, int endLine) const
    void getImage(Character *dest, int size, int startLine, int endLine) const;
    // 对应C++: void Screen::copyFromHistory(Character* dest, int startLine, int count) const
    void copyFromHistory(Character *dest, int startLine, int count, bool forceCopy = false) const;
    // 对应C++: void Screen::copyFromScreen(Character* dest, int startLine, int count) const
    void copyFromScreen(Character *dest, int startLine, int count, bool forceCopy = false) const;
    // 对应C++: void Screen::reverseRendition(Character& p) const
    static void reverseRendition(Character &c);

    // --- selection helpers -------------------------------------------------------
    // 对应C++: void Screen::checkSelection(int from, int to)
    void checkSelection(int fromPos, int toPos);
    // 对应C++: void Screen::getSelectionStart(int& column, int& line) const
    void getSelectionStart(int &column, int &line) const;
    // 对应C++: void Screen::getSelectionEnd(int& column, int& line) const
    void getSelectionEnd(int &column, int &line) const;
    // 对应C++: QString Screen::selectedText(bool preserveLineBreaks) const
    QString selectedText(bool preserveLineBreaks = true) const;
    // 对应C++: void Screen::writeSelectionToStream(TerminalCharacterDecoder* decoder, bool preserveLineBreaks) const
    void writeSelectionToStream(TerminalCharacterDecoder *decoder, bool preserveLineBreaks = true) const;
    // 对应C++: void Screen::writeToStream(TerminalCharacterDecoder* decoder, int startIndex, int endIndex, bool preserveLineBreaks) const
    void writeToStream(TerminalCharacterDecoder *decoder, int startIndex, int endIndex,
                       bool preserveLineBreaks = true) const;
    // 对应C++: int Screen::copyLineToStream(int line, int start, int count, TerminalCharacterDecoder* decoder, bool appendNewLine, bool preserveLineBreaks) const
    int copyLineToStream(int line, int start, int count, TerminalCharacterDecoder *decoder,
                         bool appendNewLine, bool preserveLineBreaks) const;

    // --- resize --------------------------------------------------------------------
    // 对应C++: void Screen::resizeImage(int new_lines, int new_columns)
    // reflow=true 时按新列宽对 history + screenLines 做完整文本重排（宽字符感知）；
    // 默认 false 保持既有调用点行为不变（直接截断/回填）。
    void resizeImage(int newLines, int newColumns, bool reflow = false);

    // --- line properties -------------------------------------------------------------
    // 对应C++: void Screen::setLineProperty(LineProperty property, bool enable)
    void setLineProperty(LineProperty property, bool enable);
    // 对应C++: QVector<LineProperty> Screen::getLineProperties(int startLine, int endLine) const
    QVector<LineProperty> getLineProperties(int startLine, int endLine) const;

    // 对应C++: void Screen::compose(const QString& compose)
    void compose(const QString &composeString);

    // 对应C++: QSet<uint> Screen::usedExtendedChars() const
    QSet<uint> usedExtendedChars() const;

    // 对应C++: void Screen::writeLinesToStream(TerminalCharacterDecoder* decoder, int fromLine, int toLine) const
    void writeLinesToStream(TerminalCharacterDecoder *decoder, int fromLine, int toLine) const;

    // 对应C++: void Screen::setForeColor(int space, int color)
    void setForeColor(int space, int color) { setForegroundColor(space, color); }
    // 对应C++: void Screen::setBackColor(int space, int color)
    void setBackColor(int space, int color) { setBackgroundColor(space, color); }

    // The default character used to pad empty space.
    // 对应C++: static Character defaultChar
    static Character defaultChar;

private:
    // Internal linear-offset variant of clearImage already uses loca/loce.
    // Private copy-from-screen with explicit destination offset (Python:
    // copyFromScreenWithOffset), used by getImage() to merge history+screen.
    void copyFromScreenWithOffset(Character *dest, int destOffset, int startLine,
                                  int count, bool forceCopy) const;

    // --- reflow helpers（resizeImage 文本重排专用）---------------------------
    // 合并 history + screenLines 为逻辑行列表（按 wrapped 标志拼接物理行），
    // 同时算出光标所在逻辑行下标与行内偏移。includeHistory=false 时只收集
    // 屏上行（路径 B 安全阀：超大历史跳过历史重排）。
    QList<TextLine> collectLogicalLines(int &cursorLogicalLine, int &cursorLogicalOffset,
                                        bool includeHistory = true) const;
    // 单条逻辑行按新列宽重切为物理行（宽字符感知：切割点不落在双宽字符
    // 首格与占位格之间），结果追加到 outRows/outWrapped。
    static void rewrapLogicalLine(const TextLine &logical, int newColumns,
                                  QVector<TextLine> &outRows, QVector<bool> &outWrapped);
    // 将重切结果切分回 history + screenLines 并落位光标。rebuildHistory=true 时
    // 新建 history 并替换旧的（路径 C）；false 时溢出前缀行追加进现有 history（路径 B）。
    void rebuildFromRows(const QVector<TextLine> &rows, const QVector<bool> &wrapped,
                         int newLines, int newColumns, int cursorRow, int cursorCol,
                         bool rebuildHistory);

    // Number of lines and columns in the visible screen.
    int lines;
    int columns;

    // The screen image. Only the first `lines` rows are visible; the extra
    // row mirrors the Python allocation (lines + 1).
    // 对应C++: QVector< QVector<Character> > screenLines (Python: List[List[Character]])
    QVector<TextLine> screenLines;

    int _scrolledLines;
    int _droppedLines;
    QRect _lastScrolledRegion;

    // Scrollback storage.
    // 对应C++: HistoryScroll* history
    HistoryScroll *history;

    // Cursor position (0-based).
    int cuX;
    int cuY;

    // Current rendition + colors.
    quint16 currentRendition;
    CharacterColor currentForeground;
    CharacterColor currentBackground;

    // Scroll margins.
    int _topMargin;
    int _bottomMargin;

    // Mode arrays.
    bool currentModes[MODES_SCREEN];
    bool savedModes[MODES_SCREEN];

    // Per-line properties (wrapped, double width/height).
    // 对应C++: QVector<LineProperty> lineProperties
    QVector<LineProperty> lineProperties;

    // Tab stops.
    QVector<bool> tabStops;

    // Selection (linear offsets into the merged history+screen image).
    int selBegin;
    int selTopLeft;
    int selBottomRight;
    bool blockSelectionMode;

    // Effective rendition + colors (after RE_REVERSE / RE_BOLD handling).
    CharacterColor effectiveForeground;
    CharacterColor effectiveBackground;
    quint16 effectiveRendition;

    // Saved cursor state (DECSC / DECRC).
    SavedState savedState;

    // Last written cell offset and last drawn code point (for repeatChars).
    int lastPos;
    uint lastDrawnChar;
};

} // namespace Konsole
