// Screen.cpp — C++ port of qtermwidget/screen.py
//
// Implementation of the terminal screen model. Ported from the Python
// PySide6 version, which was itself converted from Konsole / QTermWidget
// (upstream Screen.cpp).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robert.knight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include "Screen.h"

#include <QTextStream>

#include "TerminalCharacterDecoder.h"

using namespace Konsole;

// 对应C++: Character Screen::defaultChar
Character Screen::defaultChar = Character(static_cast<quint16>(u' '),
                                          CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR),
                                          CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR),
                                          DEFAULT_RENDITION);

// 对应C++: Screen::Screen(int l, int c)
Screen::Screen(int l, int c)
    : lines(l),
      columns(c),
      screenLines(l + 1),
      _scrolledLines(0),
      _droppedLines(0),
      history(new HistoryScrollNone()),
      cuX(0),
      cuY(0),
      currentRendition(DEFAULT_RENDITION),
      currentForeground(CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR)),
      currentBackground(CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR)),
      _topMargin(0),
      _bottomMargin(0),
      lineProperties(l + 1, LINE_DEFAULT),
      selBegin(-1),
      selTopLeft(-1),
      selBottomRight(-1),
      blockSelectionMode(false),
      effectiveRendition(DEFAULT_RENDITION),
      lastPos(-1),
      lastDrawnChar(0)
{
    for (int i = 0; i < MODES_SCREEN; ++i) {
        currentModes[i] = false;
        savedModes[i] = false;
    }

    initTabStops();
    clearSelection();
    reset();
}

// 对应C++: Screen::~Screen()
Screen::~Screen()
{
    delete history;
}

// ---------------------------------------------------------------------------
// Cursor movement
// ---------------------------------------------------------------------------

// 对应C++: void Screen::cursorUp(int n)
void Screen::cursorUp(int n)
{
    if (n == 0) n = 1;
    const int stop = cuY < _topMargin ? 0 : _topMargin;
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuY = qMax(stop, cuY - n);
}

// 对应C++: void Screen::cursorDown(int n)
void Screen::cursorDown(int n)
{
    if (n == 0) n = 1;
    const int stop = cuY > _bottomMargin ? lines - 1 : _bottomMargin;
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuY = qMin(stop, cuY + n);
}

// 对应C++: void Screen::cursorLeft(int n)
void Screen::cursorLeft(int n)
{
    if (n == 0) n = 1;
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuX = qMax(0, cuX - n);
}

// 对应C++: void Screen::cursorRight(int n)
void Screen::cursorRight(int n)
{
    if (n == 0) n = 1;
    cuX = qMin(columns - 1, cuX + n);
}

// 对应C++: void Screen::cursorNextLine(int n)
void Screen::cursorNextLine(int n)
{
    if (n == 0) n = 1;
    cuX = 0;
    while (n > 0) {
        if (cuY < lines - 1) cuY += 1;
        n -= 1;
    }
}

// 对应C++: void Screen::cursorPreviousLine(int n)
void Screen::cursorPreviousLine(int n)
{
    if (n == 0) n = 1;
    cuX = 0;
    while (n > 0) {
        if (cuY > 0) cuY -= 1;
        n -= 1;
    }
}

// 对应C++: void Screen::setCursorX(int x)
void Screen::setCursorX(int x)
{
    if (x == 0) x = 1; // Default
    x -= 1;            // Adjust to 0-based
    cuX = qMax(0, qMin(columns - 1, x));
}

// 对应C++: void Screen::setCursorY(int y)
void Screen::setCursorY(int y)
{
    if (y == 0) y = 1; // Default
    y -= 1;            // Adjust to 0-based
    const int originOffset = getMode(MODE_Origin) ? _topMargin : 0;
    cuY = qMax(0, qMin(lines - 1, y + originOffset));
}

// 对应C++: void Screen::setCursorYX(int y, int x)
void Screen::setCursorYX(int y, int x)
{
    setCursorY(y);
    setCursorX(x);
}

// 对应C++: void Screen::home()
void Screen::home()
{
    cuX = 0;
    cuY = 0;
}

// 对应C++: void Screen::toStartOfLine()
void Screen::toStartOfLine()
{
    cuX = 0;
}

// ---------------------------------------------------------------------------
// Margins
// ---------------------------------------------------------------------------

// 对应C++: void Screen::setMargins(int top, int bot)
void Screen::setMargins(int topLine, int bottomLine)
{
    if (topLine == 0) topLine = 1;             // Default
    if (bottomLine == 0) bottomLine = lines;   // Default

    const int top = topLine - 1;    // Adjust to 0-based
    const int bot = bottomLine - 1; // Adjust to 0-based

    if (!(0 <= top && top < bot && bot < lines))
        return; // Invalid range, ignore

    _topMargin = top;
    _bottomMargin = bot;
    cuX = 0;
    cuY = getMode(MODE_Origin) ? top : 0;
}

// 对应C++: void Screen::setDefaultMargins()
void Screen::setDefaultMargins()
{
    _topMargin = 0;
    _bottomMargin = lines - 1;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

// 对应C++: void Screen::setMode(int m)
void Screen::setMode(int mode)
{
    currentModes[mode] = true;
    if (mode == MODE_Origin) {
        cuX = 0;
        cuY = _topMargin;
    }
}

// 对应C++: void Screen::resetMode(int m)
void Screen::resetMode(int mode)
{
    currentModes[mode] = false;
    if (mode == MODE_Origin) {
        cuX = 0;
        cuY = 0;
    }
}

// 对应C++: void Screen::saveMode(int m)
void Screen::saveMode(int mode)
{
    savedModes[mode] = currentModes[mode];
}

// 对应C++: void Screen::restoreMode(int m)
void Screen::restoreMode(int mode)
{
    currentModes[mode] = savedModes[mode];
}

// ---------------------------------------------------------------------------
// Cursor save/restore
// ---------------------------------------------------------------------------

// 对应C++: void Screen::saveCursor()
void Screen::saveCursor()
{
    savedState.cursorColumn = cuX;
    savedState.cursorLine = cuY;
    savedState.rendition = currentRendition;
    savedState.foreground = currentForeground;
    savedState.background = currentBackground;
}

// 对应C++: void Screen::restoreCursor()
void Screen::restoreCursor()
{
    cuX = qMin(savedState.cursorColumn, columns - 1);
    cuY = qMin(savedState.cursorLine, lines - 1);
    currentRendition = savedState.rendition;
    currentForeground = savedState.foreground;
    currentBackground = savedState.background;
    updateEffectiveRendition();
}

// ---------------------------------------------------------------------------
// Tab stops
// ---------------------------------------------------------------------------

// 对应C++: void Screen::initTabStops()
void Screen::initTabStops()
{
    tabStops = QVector<bool>(columns, false);
    for (int i = 0; i < columns; ++i)
        tabStops[i] = (i % 8 == 0 && i != 0);
}

// 对应C++: void Screen::clearTabStops()
void Screen::clearTabStops()
{
    tabStops = QVector<bool>(columns, false);
}

// 对应C++: void Screen::changeTabStop(bool set)
void Screen::changeTabStop(bool setStop)
{
    if (cuX >= columns) return;
    tabStops[cuX] = setStop;
}

// 对应C++: void Screen::tab(int n)
void Screen::tab(int n)
{
    if (n == 0) n = 1;
    while (n > 0 && cuX < columns - 1) {
        cursorRight(1);
        while (cuX < columns - 1 && !tabStops[cuX])
            cursorRight(1);
        n -= 1;
    }
}

// 对应C++: void Screen::backtab(int n)
void Screen::backtab(int n)
{
    if (n == 0) n = 1;
    while (n > 0 && cuX > 0) {
        cursorLeft(1);
        while (cuX > 0 && !tabStops[cuX])
            cursorLeft(1);
        n -= 1;
    }
}

// ---------------------------------------------------------------------------
// Basic attributes / history
// ---------------------------------------------------------------------------

// 对应C++: int Screen::getHistLines() const
int Screen::getHistLines() const
{
    return history->getLines();
}

// 对应C++: void Screen::setScroll(const HistoryType& t, bool copyPreviousScroll)
void Screen::setScroll(const HistoryType &historyType, bool copyPreviousScroll)
{
    clearSelection();

    if (copyPreviousScroll) {
        history = historyType.scroll(history);
    } else {
        HistoryScroll *oldScroll = history;
        history = historyType.scroll(nullptr);
        delete oldScroll;
    }
}

// 对应C++: const HistoryType& Screen::getScroll() const
const HistoryType &Screen::getScroll() const
{
    return history->getType();
}

// 对应C++: bool Screen::hasScroll() const
bool Screen::hasScroll() const
{
    return history->hasScroll();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

// 对应C++: void Screen::clearSelection()
void Screen::clearSelection()
{
    selBottomRight = -1;
    selTopLeft = -1;
    selBegin = -1;
}

// 对应C++: void Screen::setSelectionStart(const int x, const int y, const bool mode)
void Screen::setSelectionStart(int column, int line, bool blockMode)
{
    selBegin = loc(column, line, columns);
    // Fix up when the start is just past the right edge.
    if (column == columns)
        selBegin -= 1;

    selBottomRight = selBegin;
    selTopLeft = selBegin;
    blockSelectionMode = blockMode;
}

// 对应C++: void Screen::setSelectionEnd(const int x, const int y)
void Screen::setSelectionEnd(int column, int line)
{
    if (selBegin == -1) return;

    int endPos = loc(column, line, columns);

    if (endPos < selBegin) {
        selTopLeft = endPos;
        selBottomRight = selBegin;
    } else {
        // Fix up when the end is just past the right edge.
        if (column == columns)
            endPos -= 1;

        selTopLeft = selBegin;
        selBottomRight = endPos;
    }

    // Normalize block-mode selection.
    if (blockSelectionMode) {
        const int topRow = selTopLeft / columns;
        const int topColumn = selTopLeft % columns;
        const int bottomRow = selBottomRight / columns;
        const int bottomColumn = selBottomRight % columns;

        selTopLeft = loc(qMin(topColumn, bottomColumn), topRow, columns);
        selBottomRight = loc(qMax(topColumn, bottomColumn), bottomRow, columns);
    }
}

// 对应C++: bool Screen::isSelected(const int x, const int y) const
bool Screen::isSelected(int column, int line) const
{
    bool columnInSelection = true;
    if (blockSelectionMode) {
        columnInSelection = (column >= (selTopLeft % columns) &&
                             column <= (selBottomRight % columns));
    }

    const int pos = loc(column, line, columns);
    return (pos >= selTopLeft && pos <= selBottomRight && columnInSelection);
}

// 对应C++: bool Screen::isSelectionValid() const
bool Screen::isSelectionValid() const
{
    return selTopLeft >= 0 && selBottomRight >= 0;
}

// ---------------------------------------------------------------------------
// Rendition / color
// ---------------------------------------------------------------------------

// 对应C++: void Screen::updateEffectiveRendition()
void Screen::updateEffectiveRendition()
{
    effectiveRendition = currentRendition;
    if (currentRendition & RE_REVERSE) {
        effectiveForeground = currentBackground;
        effectiveBackground = currentForeground;
    } else {
        effectiveForeground = currentForeground;
        effectiveBackground = currentBackground;
    }

    if (currentRendition & RE_BOLD)
        effectiveForeground.setIntensive();
}

// 对应C++: void Screen::setForegroundColor(int space, int color)
void Screen::setForegroundColor(int space, int color)
{
    currentForeground = CharacterColor(static_cast<quint8>(space), color);
    if (currentForeground.isValid())
        updateEffectiveRendition();
    else
        setForegroundColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
}

// 对应C++: void Screen::setBackgroundColor(int space, int color)
void Screen::setBackgroundColor(int space, int color)
{
    currentBackground = CharacterColor(static_cast<quint8>(space), color);
    if (currentBackground.isValid())
        updateEffectiveRendition();
    else
        setBackgroundColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
}

// 对应C++: void Screen::setRendition(int rendition)
void Screen::setRendition(int rendition)
{
    currentRendition |= static_cast<quint16>(rendition);
    updateEffectiveRendition();
}

// 对应C++: void Screen::resetRendition(int rendition)
void Screen::resetRendition(int rendition)
{
    currentRendition &= static_cast<quint16>(~rendition);
    updateEffectiveRendition();
}

// 对应C++: void Screen::setDefaultRendition()
void Screen::setDefaultRendition()
{
    setForegroundColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR);
    setBackgroundColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR);
    currentRendition = DEFAULT_RENDITION;
    updateEffectiveRendition();
}

// ---------------------------------------------------------------------------
// Clear / reset
// ---------------------------------------------------------------------------

// 对应C++: void Screen::clear()
void Screen::clear()
{
    clearEntireScreen();
    home();
}

// 对应C++: void Screen::reset(bool clearScreen)
void Screen::reset(bool clearScreen)
{
    setMode(MODE_Wrap);
    saveMode(MODE_Wrap);     // wrap at end of margin
    resetMode(MODE_Origin);
    saveMode(MODE_Origin);   // position reference [1,1]
    resetMode(MODE_Insert);
    saveMode(MODE_Insert);   // overstroke
    setMode(MODE_Cursor);    // cursor visible
    resetMode(MODE_Screen);  // screen not inverse
    resetMode(MODE_NewLine);

    _topMargin = 0;
    _bottomMargin = lines - 1;

    setDefaultRendition();
    saveCursor();

    if (clearScreen)
        clear();
}

// 对应C++: void Screen::clearEntireScreen()
void Screen::clearEntireScreen()
{
    // Add entire screen to history.
    for (int i = 0; i < (lines - 1); i++) {
        addHistLine();
        scrollUp(0, 1);
    }

    clearImage(loc(0, 0, columns), loc(columns - 1, lines - 1, columns), QChar(' '));
}

// 对应C++: void Screen::clearToEndOfScreen()
void Screen::clearToEndOfScreen()
{
    clearImage(loc(cuX, cuY, columns),
               loc(columns - 1, lines - 1, columns), QChar(' '));
}

// 对应C++: void Screen::clearToBeginOfScreen()
void Screen::clearToBeginOfScreen()
{
    clearImage(loc(0, 0, columns),
               loc(cuX, cuY, columns), QChar(' '));
}

// 对应C++: void Screen::clearEntireLine()
void Screen::clearEntireLine()
{
    clearImage(loc(0, cuY, columns),
               loc(columns - 1, cuY, columns), QChar(' '));
}

// 对应C++: void Screen::clearToEndOfLine()
void Screen::clearToEndOfLine()
{
    clearImage(loc(cuX, cuY, columns),
               loc(columns - 1, cuY, columns), QChar(' '));
}

// 对应C++: void Screen::clearToBeginOfLine()
void Screen::clearToBeginOfLine()
{
    clearImage(loc(0, cuY, columns),
               loc(cuX, cuY, columns), QChar(' '));
}

// 对应C++: void Screen::helpAlign()
void Screen::helpAlign()
{
    clearImage(loc(0, 0, columns),
               loc(columns - 1, lines - 1, columns), QChar('E'));
}

// 对应C++: void Screen::backspace()
void Screen::backspace()
{
    cuX = qMin(columns - 1, cuX); // nowrap!
    cuX = qMax(0, cuX - 1);

    if (screenLines[cuY].size() < cuX + 1)
        screenLines[cuY].resize(cuX + 1);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// 对应C++: void Screen::clearImage(int loca, int loce, char c)
void Screen::clearImage(int loca, int loce, QChar c)
{
    const int scr_tl = loc(0, history->getLines(), columns);

    // Clear the selection if it overlaps the cleared region.
    if ((selBottomRight > (loca + scr_tl)) &&
        (selTopLeft < (loce + scr_tl))) {
        clearSelection();
    }

    const int topLine = loca / columns;
    const int bottomLine = loce / columns;

    const Character clearCh(static_cast<quint16>(c.unicode()), currentForeground, currentBackground, DEFAULT_RENDITION);

    // Whether the clear char is the default (blank) cell, allowing a fast path.
    const bool isDefaultCh = (clearCh == Character());

    for (int y = topLine; y <= bottomLine; ++y) {
        lineProperties[y] = LINE_DEFAULT;

        const int endCol = (y == bottomLine) ? (loce % columns) : columns - 1;
        const int startCol = (y == topLine) ? (loca % columns) : 0;

        TextLine &line = screenLines[y];

        if (isDefaultCh && endCol == columns - 1) {
            // Fast path: shrink the line.
            if (startCol == 0)
                line.clear();
            else
                line.resize(startCol);
        } else {
            // Ensure the line is long enough.
            if (line.size() < endCol + 1)
                line.resize(endCol + 1);

            for (int i = startCol; i <= endCol; ++i)
                line[i] = clearCh;
        }
    }
}

// 对应C++: void Screen::addHistLine()
void Screen::addHistLine()
{
    if (hasScroll()) {
        const int oldHistLines = history->getLines();

        history->addCellsVector(screenLines[0]);
        history->addLine((lineProperties[0] & LINE_WRAPPED) != 0);

        const int newHistLines = history->getLines();

        const bool beginIsTl = (selBegin == selTopLeft);

        // If the history is full, increment the dropped-line count.
        if (newHistLines == oldHistLines)
            _droppedLines += 1;

        // Adjust selection for the new reference point.
        if (newHistLines > oldHistLines) {
            if (selBegin != -1) {
                selTopLeft += columns;
                selBottomRight += columns;
            }
        }

        if (selBegin != -1) {
            // Scroll the selection up in the history.
            const int topBr = loc(0, 1 + newHistLines, columns);

            if (selTopLeft < topBr)
                selTopLeft -= columns;

            if (selBottomRight < topBr)
                selBottomRight -= columns;

            if (selBottomRight < 0)
                clearSelection();
            else if (selTopLeft < 0)
                selTopLeft = 0;

            selBegin = beginIsTl ? selTopLeft : selBottomRight;
        }
    }
}

// 对应C++: void Screen::scrollUp(int from, int n)  (region, no history)
void Screen::scrollUp(int fromLine, int n)
{
    if (n <= 0) return;
    if (fromLine > _bottomMargin) return;
    if (fromLine + n > _bottomMargin)
        n = _bottomMargin + 1 - fromLine;

    _scrolledLines -= n;
    _lastScrolledRegion = QRect(0, _topMargin,
                                columns - 1,
                                _bottomMargin - _topMargin);

    moveImage(loc(0, fromLine, columns),
              loc(0, fromLine + n, columns),
              loc(columns, _bottomMargin, columns));
    clearImage(loc(0, _bottomMargin - n + 1, columns),
               loc(columns - 1, _bottomMargin, columns), QChar(' '));
}

// 对应C++: void Screen::moveImage(int dest, int sourceBegin, int sourceEnd)
void Screen::moveImage(int dest, int sourceBegin, int sourceEnd)
{
    Q_ASSERT(sourceBegin <= sourceEnd);

    const int linesCount = (sourceEnd - sourceBegin) / columns;

    // Choose the copy order based on the move direction.
    if (dest < sourceBegin) {
        // Moving forward (up).
        for (int i = 0; i <= linesCount; ++i) {
            const int srcLine = (sourceBegin / columns) + i;
            const int destLine = (dest / columns) + i;
            if (srcLine < screenLines.size() && destLine < screenLines.size()) {
                screenLines[destLine] = screenLines[srcLine];
                lineProperties[destLine] = lineProperties[srcLine];
            }
        }
    } else {
        // Moving backward (down).
        for (int i = linesCount; i >= 0; --i) {
            const int srcLine = (sourceBegin / columns) + i;
            const int destLine = (dest / columns) + i;
            if (srcLine < screenLines.size() && destLine < screenLines.size()) {
                screenLines[destLine] = screenLines[srcLine];
                lineProperties[destLine] = lineProperties[srcLine];
            }
        }
    }

    // Adjust the last-position marker.
    if (lastPos != -1) {
        const int diff = dest - sourceBegin;
        lastPos += diff;
        if (lastPos < 0 || lastPos >= (lines * columns))
            lastPos = -1;
    }

    // Adjust the selection.
    if (selBegin != -1) {
        const bool beginIsTl = (selBegin == selTopLeft);
        const int diff = dest - sourceBegin;
        const int scr_tl = loc(0, history->getLines(), columns);
        const int srca = sourceBegin + scr_tl;
        const int srce = sourceEnd + scr_tl;
        const int desta = srca + diff;
        const int deste = srce + diff;

        if (srca <= selTopLeft && selTopLeft <= srce)
            selTopLeft += diff;
        else if (desta <= selTopLeft && selTopLeft <= deste)
            selBottomRight = -1; // clear selection

        if (srca <= selBottomRight && selBottomRight <= srce)
            selBottomRight += diff;
        else if (desta <= selBottomRight && selBottomRight <= deste)
            selBottomRight = -1; // clear selection

        if (selBottomRight < 0)
            clearSelection();
        else if (selTopLeft < 0)
            selTopLeft = 0;

        selBegin = beginIsTl ? selTopLeft : selBottomRight;
    }
}

// ---------------------------------------------------------------------------
// fillWithDefaultChar
// ---------------------------------------------------------------------------

// 对应C++: static void Screen::fillWithDefaultChar(Character* dest, int count)
void Screen::fillWithDefaultChar(Character *dest, int count)
{
    for (int i = 0; i < count; ++i)
        dest[i] = defaultChar;
}

// ---------------------------------------------------------------------------
// Character display
// ---------------------------------------------------------------------------

// 对应C++: void Screen::displayCharacter(wchar_t c)
void Screen::displayCharacter(uint c)
{
    // NOTE: control characters and combining marks are filtered out below.

    const int w = konsole_wcwidth(c);
    if (w < 0)
        return; // Non-printable character

    // Zero-width (combining) characters are ignored, mirroring the Python
    // port (which does not implement the extended-char table composition).
    if (w == 0)
        return;

    // Handle line wrapping.
    if (cuX + w > columns) {
        if (getMode(MODE_Wrap)) {
            lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] | LINE_WRAPPED);
            nextLine();
        } else {
            cuX = columns - w;
        }
    }

    // Ensure the current line is long enough.
    while (screenLines[cuY].size() < cuX + w)
        screenLines[cuY].append(Character());

    // Insert-mode handling.
    if (getMode(MODE_Insert))
        insertChars(w);

    lastPos = loc(cuX, cuY, columns);

    // Check whether the selection is still valid.
    checkSelection(lastPos, lastPos);

    // Create the cell for the main character.
    const Character currentChar(static_cast<quint16>(c),
                                effectiveForeground,
                                effectiveBackground,
                                effectiveRendition);

    // Set the main character.
    screenLines[cuY][cuX] = currentChar;
    lastDrawnChar = c;

    // Handle the second cell of a wide character.
    if (w == 2) {
        if (screenLines[cuY].size() < cuX + 2)
            screenLines[cuY].append(Character());

        // Second half of a wide character: a NUL placeholder cell.
        const Character secondChar(static_cast<quint16>(0),
                                   effectiveForeground,
                                   effectiveBackground,
                                   effectiveRendition);
        screenLines[cuY][cuX + 1] = secondChar;
    }

    cuX += w;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// 对应C++: void Screen::eraseChars(int n)
void Screen::eraseChars(int n)
{
    if (n == 0) n = 1;
    const int p = qMax(0, qMin(cuX + n - 1, columns - 1));
    clearImage(loc(cuX, cuY, columns), loc(p, cuY, columns), QChar(' '));
}

// 对应C++: void Screen::deleteChars(int n)
void Screen::deleteChars(int n)
{
    if (n == 0) n = 1;

    TextLine &line = screenLines[cuY];

    // Nothing to do if the cursor is past the end of the line.
    if (cuX >= line.size()) return;

    if (cuX + n > line.size())
        n = line.size() - cuX;

    if (n > 0)
        line.remove(cuX, n);
}

// 对应C++: void Screen::insertChars(int n)
void Screen::insertChars(int n)
{
    if (n == 0) n = 1;

    TextLine &line = screenLines[cuY];

    // Ensure the line is long enough.
    while (line.size() < cuX)
        line.append(defaultChar);

    // Insert blank cells.
    for (int i = 0; i < n; ++i)
        line.insert(cuX, Character());

    // Limit the line length to the number of columns.
    if (line.size() > columns)
        line.resize(columns);
}

// 对应C++: void Screen::repeatChars(int count)
void Screen::repeatChars(int count)
{
    if (count == 0) count = 1;

    for (int i = 0; i < count; ++i) {
        if (lastDrawnChar > 0)
            displayCharacter(lastDrawnChar);
    }
}

// 对应C++: void Screen::deleteLines(int n)
void Screen::deleteLines(int n)
{
    if (n == 0) n = 1;
    scrollUp(cuY, n);
}

// 对应C++: void Screen::insertLines(int n)
void Screen::insertLines(int n)
{
    if (n == 0) n = 1;
    scrollDown(cuY, n);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

// 对应C++: void Screen::index()
void Screen::index()
{
    if (cuY == _bottomMargin)
        scrollUpRegion(1);
    else if (cuY < lines - 1)
        cuY += 1;
}

// 对应C++: void Screen::reverseIndex()
void Screen::reverseIndex()
{
    if (cuY == _topMargin)
        scrollDown(_topMargin, 1);
    else if (cuY > 0)
        cuY -= 1;
}

// 对应C++: void Screen::nextLine()
void Screen::nextLine()
{
    toStartOfLine();
    index();
}

// 对应C++: void Screen::newLine()
void Screen::newLine()
{
    if (getMode(MODE_NewLine))
        toStartOfLine();
    index();
}

// 对应C++: void Screen::scrollUp(int n)  (region, adds to history)
void Screen::scrollUpRegion(int n)
{
    if (n == 0) n = 1;
    if (_topMargin == 0)
        addHistLine(); // add to history
    scrollUp(_topMargin, n);
}

// 对应C++: void Screen::scrollDown(int n)  (region)
void Screen::scrollDownRegion(int n)
{
    if (n == 0) n = 1;
    scrollDown(_topMargin, n);
}

// 对应C++: void Screen::scrollDown(int from, int n)  (region, no history)
void Screen::scrollDown(int fromLine, int n)
{
    _scrolledLines += n;

    if (n <= 0) return;
    if (fromLine > _bottomMargin) return;
    if (fromLine + n > _bottomMargin)
        n = _bottomMargin - fromLine;

    moveImage(loc(0, fromLine + n, columns),
              loc(0, fromLine, columns),
              loc(columns - 1, _bottomMargin - n, columns));
    clearImage(loc(0, fromLine, columns),
               loc(columns - 1, fromLine + n - 1, columns), QChar(' '));
}

// ---------------------------------------------------------------------------
// Image copy
// ---------------------------------------------------------------------------

// 对应C++: void Screen::getImage(Character* dest, int size, int startLine, int endLine) const
void Screen::getImage(Character *dest, int size, int startLine, int endLine) const
{
    Q_ASSERT(startLine >= 0);
    Q_ASSERT(endLine >= startLine && endLine < history->getLines() + lines);

    const int mergedLines = endLine - startLine + 1;
    Q_ASSERT(size >= mergedLines * columns);

    const int linesInHistoryBuffer = qMax(0, qMin(history->getLines() - startLine, mergedLines));
    const int linesInScreenBuffer = mergedLines - linesInHistoryBuffer;

    // Copy lines from the history buffer.
    const bool forceCopy = getMode(MODE_Screen);
    if (linesInHistoryBuffer > 0)
        copyFromHistory(dest, startLine, linesInHistoryBuffer, forceCopy);

    // Copy lines from the screen buffer.
    if (linesInScreenBuffer > 0) {
        const int screenStart = startLine + linesInHistoryBuffer - history->getLines();
        const int destOffset = linesInHistoryBuffer * columns;
        copyFromScreenWithOffset(dest, destOffset, screenStart, linesInScreenBuffer, forceCopy);
    }

    // Inverse display mode.
    if (forceCopy) {
        for (int i = 0; i < mergedLines * columns; ++i)
            reverseRendition(dest[i]);
    }

    // Mark the current cursor position (only when the cursor is visible).
    const int cursorIndex = loc(cuX, cuY + linesInHistoryBuffer, columns);
    if (getMode(MODE_Cursor) && cursorIndex < columns * mergedLines) {
        dest[cursorIndex].rendition =
            static_cast<quint16>(dest[cursorIndex].rendition | RE_CURSOR);
    }
}

// 对应C++: void Screen::copyFromHistory(Character* dest, int startLine, int count) const
void Screen::copyFromHistory(Character *dest, int startLine, int count, bool forceCopy) const
{
    Q_ASSERT(startLine >= 0 && count > 0 && startLine + count <= history->getLines());

    for (int line = startLine; line < startLine + count; ++line) {
        const int length = qMin(columns, history->getLineLen(line));
        const int destLineOffset = (line - startLine) * columns;

        // Fetch the cells from history.
        if (length > 0) {
            history->getCells(line, 0, length, dest + destLineOffset);
            // history->getCells writes Character values directly; forceCopy is
            // implicit (the caller owns the buffer).
            Q_UNUSED(forceCopy);
        }

        // Fill the remaining columns with blanks.
        for (int column = length; column < columns; ++column)
            dest[destLineOffset + column] = Character();

        // Invert the selected text.
        if (selBegin != -1) {
            for (int column = 0; column < columns; ++column) {
                if (isSelected(column, line))
                    reverseRendition(dest[destLineOffset + column]);
            }
        }
    }
}

// 对应C++: void Screen::copyFromScreen(Character* dest, int startLine, int count) const
void Screen::copyFromScreen(Character *dest, int startLine, int count, bool forceCopy) const
{
    copyFromScreenWithOffset(dest, 0, startLine, count, forceCopy);
}

// Private offset variant used by getImage() (Python: copyFromScreenWithOffset).
void Screen::copyFromScreenWithOffset(Character *dest, int destOffset, int startLine,
                                      int count, bool forceCopy) const
{
    Q_ASSERT(startLine >= 0 && count > 0 && startLine + count <= lines);
    Q_UNUSED(forceCopy); // C++ Character is a value type; copy is implicit

    for (int line = startLine; line < startLine + count; ++line) {
        const int destLineStartIndex = destOffset + (line - startLine) * columns;

        for (int column = 0; column < columns; ++column) {
            const int destIndex = destLineStartIndex + column;

            // Fetch the cell from the screen line.
            if (line < screenLines.size() && column < screenLines[line].size())
                dest[destIndex] = screenLines[line][column];
            else
                dest[destIndex] = defaultChar;

            // Invert the selected text.
            if (selBegin != -1 && isSelected(column, line + history->getLines()))
                reverseRendition(dest[destIndex]);
        }
    }
}

// 对应C++: void Screen::reverseRendition(Character& p) const
void Screen::reverseRendition(Character &c)
{
    const CharacterColor f = c.foregroundColor;
    const CharacterColor b = c.backgroundColor;
    c.foregroundColor = b;
    c.backgroundColor = f;
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

// 对应C++: void Screen::checkSelection(int from, int to)
void Screen::checkSelection(int fromPos, int toPos)
{
    if (selBegin == -1) return;

    const int scr_tl = loc(0, history->getLines(), columns);
    // Clear the whole selection if it overlaps [from, to].
    if ((selBottomRight >= (fromPos + scr_tl)) &&
        (selTopLeft <= (toPos + scr_tl))) {
        clearSelection();
    }
}

// 对应C++: void Screen::getSelectionStart(int& column, int& line) const
void Screen::getSelectionStart(int &column, int &line) const
{
    if (selTopLeft != -1) {
        column = selTopLeft % columns;
        line = selTopLeft / columns;
    } else {
        column = cuX + getHistLines();
        line = cuY + getHistLines();
    }
}

// 对应C++: void Screen::getSelectionEnd(int& column, int& line) const
void Screen::getSelectionEnd(int &column, int &line) const
{
    if (selBottomRight != -1) {
        column = selBottomRight % columns;
        line = selBottomRight / columns;
    } else {
        column = cuX + getHistLines();
        line = cuY + getHistLines();
    }
}

// 对应C++: QString Screen::selectedText(bool preserveLineBreaks) const
QString Screen::selectedText(bool preserveLineBreaks) const
{
    if (!isSelectionValid())
        return QString();

    QString result;

    const int top = selTopLeft / columns;
    const int left = selTopLeft % columns;
    const int bottom = selBottomRight / columns;
    const int right = selBottomRight % columns;

    for (int y = top; y <= bottom; ++y) {
        int start = 0;
        if (y == top || blockSelectionMode)
            start = left;

        int count = -1;
        if (y == bottom || blockSelectionMode)
            count = right - start + 1;

        QString lineText;

        // Fetch the line text from history or screen.
        if (y < history->getLines()) {
            const int lineLength = history->getLineLen(y);
            start = qMin(start, qMax(0, lineLength - 1));
            if (count == -1)
                count = lineLength - start;
            else
                count = qMin(start + count, lineLength) - start;

            if (count > 0) {
                QVector<Character> buf(count);
                history->getCells(y, start, count, buf.data());
                for (int i = 0; i < count; ++i) {
                    // Skip the NUL placeholder cell of wide characters.
                    if (buf[i].character != 0)
                        lineText.append(QChar(buf[i].character));
                }
            }
        } else {
            const int screenLine = y - history->getLines();
            if (screenLine < screenLines.size()) {
                const TextLine &lineData = screenLines[screenLine];
                if (count == -1)
                    count = lineData.size() - start;

                if (start < lineData.size() && count > 0) {
                    const int endPos = qMin(start + count, lineData.size());
                    for (int i = start; i < endPos; ++i) {
                        // Skip the NUL placeholder cell of wide characters.
                        if (lineData[i].character != 0)
                            lineText.append(QChar(lineData[i].character));
                    }
                }
            }
        }

        // Append a newline (except for the last line).
        if (y != bottom && preserveLineBreaks) {
            int lineProps = LINE_DEFAULT;
            if (y < history->getLines()) {
                if (history->isWrappedLine(y))
                    lineProps |= LINE_WRAPPED;
            } else {
                const int screenLine = y - history->getLines();
                if (screenLine < lineProperties.size())
                    lineProps = lineProperties[screenLine];
            }

            if (!(lineProps & LINE_WRAPPED))
                lineText.append(QLatin1Char('\n'));
        }

        result += lineText;
    }

    return result;
}

// 对应C++: void Screen::writeSelectionToStream(TerminalCharacterDecoder* decoder, bool preserveLineBreaks) const
void Screen::writeSelectionToStream(TerminalCharacterDecoder *decoder, bool preserveLineBreaks) const
{
    if (!isSelectionValid())
        return;

    writeToStream(decoder, selTopLeft, selBottomRight, preserveLineBreaks);
}

// 对应C++: void Screen::writeToStream(TerminalCharacterDecoder* decoder, int startIndex, int endIndex, bool preserveLineBreaks) const
void Screen::writeToStream(TerminalCharacterDecoder *decoder, int startIndex, int endIndex,
                           bool preserveLineBreaks) const
{
    const int top = startIndex / columns;
    const int left = startIndex % columns;
    const int bottom = endIndex / columns;
    const int right = endIndex % columns;

    Q_ASSERT(top >= 0 && left >= 0 && bottom >= 0 && right >= 0);

    for (int y = top; y <= bottom; ++y) {
        int start = 0;
        if (y == top || blockSelectionMode)
            start = left;

        int count = -1;
        if (y == bottom || blockSelectionMode)
            count = right - start + 1;

        const bool appendNewLine = (y != bottom);
        const int copied = copyLineToStream(y, start, count, decoder,
                                            appendNewLine, preserveLineBreaks);

        // If the selection goes past the end of the last line, add a newline.
        if (y == bottom && copied < count) {
            const Character newLineChar(static_cast<quint16>(u'\n'));
            decoder->decodeLine(&newLineChar, 1, 0);
        }
    }
}

// 对应C++: int Screen::copyLineToStream(int line, int start, int count, TerminalCharacterDecoder* decoder, bool appendNewLine, bool preserveLineBreaks) const
int Screen::copyLineToStream(int line, int start, int count, TerminalCharacterDecoder *decoder,
                             bool appendNewLine, bool preserveLineBreaks) const
{
    int currentLineProperties = LINE_DEFAULT;

    QVector<Character> characterBuffer;

    // Determine whether the line is in the history buffer or the screen image.
    if (line < history->getLines()) {
        const int lineLength = history->getLineLen(line);

        // Ensure the start position is before the end of the line.
        start = qMin(start, qMax(0, lineLength - 1));

        if (count == -1)
            count = lineLength - start;
        else
            count = qMin(start + count, lineLength) - start;

        // Safety checks.
        Q_ASSERT(start >= 0);
        Q_ASSERT(count >= 0);
        Q_ASSERT((start + count) <= history->getLineLen(line));

        // Fetch the cells from history.
        characterBuffer.resize(count);
        if (count > 0)
            history->getCells(line, start, count, characterBuffer.data());

        if (history->isWrappedLine(line))
            currentLineProperties |= LINE_WRAPPED;
    } else {
        if (count == -1)
            count = columns - start;

        Q_ASSERT(count >= 0);

        const int screenLine = line - history->getLines();

        if (screenLine < screenLines.size()) {
            const TextLine &lineData = screenLines[screenLine];
            const int length = lineData.size();

            characterBuffer.resize(count);
            const int copyCount = qMin(count, length - start);
            for (int i = 0; i < copyCount; ++i)
                characterBuffer[i] = lineData[start + i];

            // count cannot exceed the available length.
            count = qMax(0, qMin(count, length - start));
            characterBuffer.resize(count);
        } else {
            count = 0;
            characterBuffer.clear();
        }

        Q_ASSERT(screenLine < lineProperties.size());
        currentLineProperties |= lineProperties[screenLine];
    }

    // Append a newline at the end of the line.
    const bool omitLineBreak = ((currentLineProperties & LINE_WRAPPED) ||
                                !preserveLineBreaks);

    if (!omitLineBreak && appendNewLine) {
        characterBuffer.append(Character(static_cast<quint16>(u'\n')));
        count += 1;
    }

    // Decode the line and write to the text stream.
    decoder->decodeLine(characterBuffer.constData(), count, currentLineProperties);

    return count;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

// 对应C++: void Screen::resizeImage(int new_lines, int new_columns)
// reflow=true 时对主屏做完整文本重排（历史 + 屏上行按新列宽重切），修复
// 窗口缩小时内容被截断、放大后不恢复的问题；reflow=false 保持原截断/回填行为。
void Screen::resizeImage(int newLines, int newColumns, bool reflow)
{
    if (newLines == lines && newColumns == columns)
        return;

    // 路径 A（原路径）：不重排，或列数不变（只变行数）时走既有逻辑。
    if (!reflow || newColumns == columns) {
        if (cuY > newLines - 1) {
            // Try to keep the focus and lines.
            _bottomMargin = lines - 1; // margins lost
            for (int i = 0; i < (cuY - (newLines - 1)); ++i) {
                addHistLine();
                scrollUp(0, 1);
            }
        }

        // Create the new screen lines and copy from the old ones.
        QVector<TextLine> newScreenLines(newLines + 1);
        for (int i = 0; i < newLines + 1; ++i) {
            if (i < screenLines.size()) {
                TextLine line = screenLines[i]; // copy existing line
                if (line.size() < newColumns)
                    line.resize(newColumns);
                else if (line.size() > newColumns)
                    line.resize(newColumns);
                newScreenLines[i] = line;
            } else {
                // New line, filled with the default character.
                newScreenLines[i] = TextLine(newColumns, defaultChar);
            }
        }

        // Adjust the line properties.
        QVector<LineProperty> newLineProperties(newLines + 1, LINE_DEFAULT);
        for (int i = 0; i < newLines + 1; ++i) {
            if (i < lineProperties.size())
                newLineProperties[i] = lineProperties[i];
        }

        clearSelection();

        screenLines = newScreenLines;
        lineProperties = newLineProperties;

        lines = newLines;
        columns = newColumns;
        cuX = qMin(cuX, columns - 1);
        cuY = qMin(cuY, lines - 1);

        // Reset the margins.
        _topMargin = 0;
        _bottomMargin = lines - 1;
        initTabStops();
        clearSelection();
        return;
    }

    // 路径 B（安全阀）：超大历史（如 HistoryScrollFile 无限历史）时跳过历史重排，
    // 只对屏上行重切，放不下屏幕的溢出前缀行追加进现有历史，保证不丢屏上数据。
    // 路径 C（完整 reflow）：历史行一并参与重排并整体重建。
    const bool rebuildHistory = (history->getLines() <= 100000);

    // C1 收集逻辑行（按 wrapped 标志拼接物理行；路径 B 时只收集屏上行），
    // 同时算出光标所在逻辑行与行内偏移。
    int cursorLogicalLine = 0;
    int cursorLogicalOffset = 0;
    const QList<TextLine> logicalLines =
        collectLogicalLines(cursorLogicalLine, cursorLogicalOffset, rebuildHistory);

    // C2 逐条逻辑行按新列宽重切（宽字符感知），并跟踪光标新坐标：
    // 行内偏移按各物理行实际长度累计换算（宽字符前移导致的短行按实际行长计）。
    QVector<TextLine> rows;
    QVector<bool> rowWrapped;
    int cursorRow = 0;
    int cursorCol = 0;
    for (int i = 0; i < logicalLines.size(); ++i) {
        const int firstRow = rows.size();
        rewrapLogicalLine(logicalLines.at(i), newColumns, rows, rowWrapped);
        if (i == cursorLogicalLine) {
            int remaining = cursorLogicalOffset;
            int r = firstRow;
            const int lastRowOfLine = rows.size() - 1;
            while (r < lastRowOfLine && remaining >= rows.at(r).size()) {
                remaining -= rows.at(r).size();
                ++r;
            }
            cursorRow = r;
            cursorCol = remaining;
        }
    }

    // C3 将重切结果切分回 history + screenLines 并落位光标。
    rebuildFromRows(rows, rowWrapped, newLines, newColumns, cursorRow, cursorCol,
                    rebuildHistory);
}

// 合并 history + screenLines 为逻辑行列表（reflow C1 步骤）。
// includeHistory=false 时只收集屏上行（路径 B 安全阀）。
QList<TextLine> Screen::collectLogicalLines(int &cursorLogicalLine, int &cursorLogicalOffset,
                                            bool includeHistory) const
{
    QList<TextLine> logicalLines;
    TextLine current; // 正在拼接的逻辑行
    cursorLogicalLine = 0;
    cursorLogicalOffset = 0;

    // 非 wrapped 行修剪尾部 padding：从行尾剔除连续等于默认 Character() 的单元；
    // wrapped 行保持原样不修剪（满宽或宽字符前移后的短行均按实际存储长度拼接）。
    auto trimTrailingPadding = [](TextLine &line) {
        int end = line.size();
        while (end > 0 && line.at(end - 1) == Character())
            --end;
        line.resize(end);
    };

    // 1) 历史行：isWrappedLine 为 true 则与下一行拼接为同一逻辑行；
    //    history 最后一行 wrapped 时（addHistLine 保存了该标志），
    //    current 不落盘，自然与 screenLines[0] 拼接为同一逻辑行。
    if (includeHistory) {
        const int histLines = history->getLines();
        for (int i = 0; i < histLines; ++i) {
            const int len = history->getLineLen(i);
            if (len > 0) {
                const int oldSize = current.size();
                current.resize(oldSize + len);
                history->getCells(i, 0, len, current.data() + oldSize);
            }
            if (!history->isWrappedLine(i)) {
                trimTrailingPadding(current);
                logicalLines.append(current);
                current.clear();
            }
        }
    }

    // 2) 有效屏上行范围：从 qMax(cuY, 最后一个非空行) 向下的纯空行全部丢弃，
    //    避免空行灌入历史。
    int lastMeaningfulRow = cuY;
    for (int i = qMin(lines, screenLines.size()) - 1; i > lastMeaningfulRow; --i) {
        const TextLine &row = screenLines.at(i);
        bool blank = true;
        for (const Character &c : row) {
            if (!(c == Character())) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            lastMeaningfulRow = i;
            break;
        }
    }

    for (int i = 0; i <= lastMeaningfulRow && i < screenLines.size(); ++i) {
        // 光标定位：此刻 current 中的内容即光标逻辑行内位于 cuY 之前的物理行
        // （含 history 尾部 wrapped 拼接），故偏移 = current.size() + cuX。
        if (i == cuY) {
            cursorLogicalLine = logicalLines.size();
            cursorLogicalOffset = current.size() + cuX;
        }
        TextLine row = screenLines.at(i);
        if (row.size() > columns)
            row.resize(columns);
        current += row;
        const bool isRowWrapped = (i < lineProperties.size()) &&
                                  ((lineProperties.at(i) & LINE_WRAPPED) != 0);
        if (!isRowWrapped || i == lastMeaningfulRow) {
            trimTrailingPadding(current);
            logicalLines.append(current);
            current.clear();
        }
    }

    // 防御性收尾：理论上此处 current 已空。
    if (!current.isEmpty()) {
        trimTrailingPadding(current);
        logicalLines.append(current);
    }

    return logicalLines;
}

// 单条逻辑行按新列宽重切为物理行（reflow C2 步骤，宽字符感知）。
void Screen::rewrapLogicalLine(const TextLine &logical, int newColumns,
                               QVector<TextLine> &outRows, QVector<bool> &outWrapped)
{
    // 空逻辑行输出一条空物理行（wrapped=false）。
    if (logical.isEmpty()) {
        outRows.append(TextLine());
        outWrapped.append(false);
        return;
    }

    const int total = logical.size();
    int pos = 0;
    while (pos < total) {
        int take = qMax(1, qMin(newColumns, total - pos));
        // 宽字符规则：双宽字符首格存实字符、次格为 NUL 占位格（character==0，
        // 见 displayCharacter）。切割点若落在两格之间，前移 1 格，短行仍标 wrapped。
        if (take > 1 && pos + take < total &&
            logical.at(pos + take).character == 0 &&
            logical.at(pos + take - 1).character != 0) {
            take -= 1;
        }
        outRows.append(logical.mid(pos, take));
        pos += take;
        outWrapped.append(pos < total);
    }
}

// 将重切结果切分回 history + screenLines 并落位光标（reflow C3 步骤）。
void Screen::rebuildFromRows(const QVector<TextLine> &rows, const QVector<bool> &wrapped,
                             int newLines, int newColumns, int cursorRow, int cursorCol,
                             bool rebuildHistory)
{
    const int totalRows = rows.size();

    // 屏上保留末尾 min(totalRows, newLines) 行；若光标行更靠上，以光标行为准
    // 上移窗口（光标可见优先于底部对齐）。
    const int screenRowCount = qMin(totalRows, newLines);
    int firstScreenRow = totalRows - screenRowCount;
    if (cursorRow < firstScreenRow)
        firstScreenRow = cursorRow;

    // 历史重建：屏外前缀行逐条写入。
    if (rebuildHistory) {
        // 所有权核验结论：各 HistoryType::scroll(nullptr) 实现均返回全新的
        // HistoryScroll（构造时 new 自己的 HistoryType，析构时 delete 之），
        // 与旧实例无共享——先建新、再 delete 旧，不会 double-free 或悬垂。
        // 参考 Screen::setScroll 已有的使用模式。
        HistoryScroll *newHist = history->getType().scroll(nullptr);
        // 与 addHistLine 的 hasScroll() 守卫语义一致：无历史（None，getLines 恒 0）
        // 与无限历史（isUnlimited）不参与丢行计数，仅有限历史容量满时计数。
        const bool countDropped = newHist->hasScroll() && !newHist->getType().isUnlimited();
        for (int i = 0; i < firstScreenRow; ++i) {
            // 与 addHistLine 一致：写入后行数未增长说明历史已满、最旧行被丢弃。
            const int oldHistLines = newHist->getLines();
            newHist->addCellsVector(rows.at(i));
            newHist->addLine(wrapped.at(i));
            if (countDropped && newHist->getLines() == oldHistLines)
                _droppedLines += 1;
        }
        HistoryScroll *oldHist = history;
        history = newHist;
        delete oldHist;
    } else {
        // 路径 B：溢出前缀行经现有接口追加进现有历史。
        // 与 addHistLine 的 hasScroll() 守卫语义一致：无历史（None，getLines 恒 0）
        // 与无限历史（isUnlimited）不参与丢行计数，仅有限历史容量满时计数。
        const bool countDropped = history->hasScroll() && !history->getType().isUnlimited();
        for (int i = 0; i < firstScreenRow; ++i) {
            // 与 addHistLine 一致：写入后行数未增长说明历史已满、最旧行被丢弃。
            const int oldHistLines = history->getLines();
            history->addCellsVector(rows.at(i));
            history->addLine(wrapped.at(i));
            if (countDropped && history->getLines() == oldHistLines)
                _droppedLines += 1;
        }
    }

    // screenLines 重建为 newLines + 1 行（保持现有 +1 哨兵行约定），
    // 不足补 TextLine(newColumns, defaultChar)；lineProperties 按 wrapped 重建。
    QVector<TextLine> newScreenLines(newLines + 1);
    QVector<LineProperty> newLineProperties(newLines + 1, LINE_DEFAULT);
    for (int i = 0; i < newLines + 1; ++i) {
        const int srcIdx = firstScreenRow + i;
        if (i < screenRowCount && srcIdx < totalRows) {
            TextLine line = rows.at(srcIdx);
            if (line.size() > newColumns)
                line.resize(newColumns);
            newScreenLines[i] = line;
            if (wrapped.at(srcIdx))
                newLineProperties[i] = LINE_WRAPPED;
        } else {
            newScreenLines[i] = TextLine(newColumns, defaultChar);
        }
    }

    // 收尾与原路径一致：选中直接清空、更新尺寸、光标落位并夹紧、重置边距与制表位。
    clearSelection();

    screenLines = newScreenLines;
    lineProperties = newLineProperties;

    lines = newLines;
    columns = newColumns;
    cuY = qBound(0, cursorRow - firstScreenRow, lines - 1);
    cuX = qBound(0, cursorCol, columns - 1);

    _topMargin = 0;
    _bottomMargin = lines - 1;
    initTabStops();
    clearSelection();
}

// ---------------------------------------------------------------------------
// Line properties
// ---------------------------------------------------------------------------

// 对应C++: void Screen::setLineProperty(LineProperty property, bool enable)
void Screen::setLineProperty(LineProperty property, bool enable)
{
    if (enable)
        lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] | property);
    else
        lineProperties[cuY] = static_cast<LineProperty>(lineProperties[cuY] & ~property);
}

// 对应C++: QVector<LineProperty> Screen::getLineProperties(int startLine, int endLine) const
QVector<LineProperty> Screen::getLineProperties(int startLine, int endLine) const
{
    Q_ASSERT(startLine >= 0);
    Q_ASSERT(endLine >= startLine && endLine < history->getLines() + lines);

    const int mergedLines = endLine - startLine + 1;
    const int linesInHistory = qMax(0, qMin(history->getLines() - startLine, mergedLines));
    const int linesInScreen = mergedLines - linesInHistory;

    QVector<LineProperty> result(mergedLines, LINE_DEFAULT);
    int index = 0;

    // Copy properties of lines from history.
    for (int line = startLine; line < startLine + linesInHistory; ++line) {
        // TODO: support line properties other than wrapped.
        if (history->isWrappedLine(line))
            result[index] = static_cast<LineProperty>(result[index] | LINE_WRAPPED);
        index += 1;
    }

    // Copy properties of lines from the screen buffer.
    const int firstScreenLine = startLine + linesInHistory - history->getLines();
    for (int line = firstScreenLine; line < firstScreenLine + linesInScreen; ++line) {
        if (line < lineProperties.size())
            result[index] = lineProperties[line];
        index += 1;
    }

    return result;
}

// 对应C++: void Screen::compose(const QString& compose)
void Screen::compose(const QString &composeString)
{
    Q_UNUSED(composeString);
    // The original C++ version does not implement this (only an assert).
    Q_ASSERT(false); // compose method not implemented - matches upstream
}

// 对应C++: QSet<uint> Screen::usedExtendedChars() const
QSet<uint> Screen::usedExtendedChars() const
{
    QSet<uint> result;
    for (int i = 0; i < lines; ++i) {
        if (i < screenLines.size()) {
            const TextLine &line = screenLines[i];
            const int limit = qMin(line.size(), columns);
            for (int j = 0; j < limit; ++j) {
                const Character &c = line[j];
                if (c.rendition & RE_EXTENDED_CHAR)
                    result.insert(c.character);
            }
        }
    }
    return result;
}

// 对应C++: void Screen::writeLinesToStream(TerminalCharacterDecoder* decoder, int fromLine, int toLine) const
void Screen::writeLinesToStream(TerminalCharacterDecoder *decoder, int fromLine, int toLine) const
{
    const int startIndex = loc(0, fromLine, columns);
    const int endIndex = loc(columns - 1, toLine, columns);
    writeToStream(decoder, startIndex, endIndex);
}
