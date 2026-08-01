// ScreenWindow.cpp — C++ port of qtermwidget/screen_window.py
//
// Provides a window/view onto a section of a terminal Screen. Ported from the
// Python PySide6 version, which was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include "ScreenWindow.h"

// Screen is ported in parallel (screen.py -> Screen.h/.cpp). Its public API is
// assumed here per the porting contract.
#include "Screen.h"

namespace Konsole {

// 对应C++: ScreenWindow::ScreenWindow(QObject* parent)
ScreenWindow::ScreenWindow(QObject *parent)
    : QObject(parent)
    , _screen(nullptr)
    , _windowBuffer(nullptr)
    , _windowBufferSize(0)
    , _bufferNeedsUpdate(true)
    , _windowLines(1)
    , _currentLine(0)
    , _trackOutput(true)
    , _scrollCount(0)
{
}

// 对应C++: ScreenWindow::~ScreenWindow()
ScreenWindow::~ScreenWindow()
{
    delete[] _windowBuffer;
}

// 对应C++: void ScreenWindow::setScreen(Screen* screen)
void ScreenWindow::setScreen(Screen *screen)
{
    Q_ASSERT(screen);
    _screen = screen;
}

// 对应C++: Screen* ScreenWindow::screen() const
Screen *ScreenWindow::screen() const
{
    return _screen;
}

// 对应C++: void ScreenWindow::setSelectionStart(int column, int line, bool columnMode)
void ScreenWindow::setSelectionStart(int column, int line, bool columnMode)
{
    if (_screen == nullptr) {
        return;
    }

    _screen->setSelectionStart(column, qMin(line + currentLine(), endWindowLine()), columnMode);

    _bufferNeedsUpdate = true;
    emit selectionChanged();
}

// 对应C++: void ScreenWindow::setSelectionEnd(int column, int line)
void ScreenWindow::setSelectionEnd(int column, int line)
{
    if (_screen == nullptr) {
        return;
    }

    _screen->setSelectionEnd(column, qMin(line + currentLine(), endWindowLine()));

    _bufferNeedsUpdate = true;
    emit selectionChanged();
}

// 对应C++: void ScreenWindow::getSelectionStart(int& column, int& line)
void ScreenWindow::getSelectionStart(int &column, int &line) const
{
    if (_screen == nullptr) {
        column = 0;
        line = 0;
        return;
    }

    _screen->getSelectionStart(column, line);
    line -= currentLine();
}

// 对应C++: void ScreenWindow::getSelectionEnd(int& column, int& line)
void ScreenWindow::getSelectionEnd(int &column, int &line) const
{
    if (_screen == nullptr) {
        column = 0;
        line = 0;
        return;
    }

    _screen->getSelectionEnd(column, line);
    line -= currentLine();
}

// 对应C++: bool ScreenWindow::isSelected(int column, int line)
bool ScreenWindow::isSelected(int column, int line) const
{
    if (_screen == nullptr) {
        return false;
    }

    return _screen->isSelected(column, qMin(line + currentLine(), endWindowLine()));
}

// 对应C++: void ScreenWindow::clearSelection()
void ScreenWindow::clearSelection()
{
    if (_screen == nullptr) {
        return;
    }

    _screen->clearSelection();
    emit selectionChanged();
}

// 对应C++: void ScreenWindow::setWindowLines(int lines)
void ScreenWindow::setWindowLines(int lines)
{
    Q_ASSERT(lines > 0);
    _windowLines = lines;
}

// 对应C++: int ScreenWindow::windowLines() const
int ScreenWindow::windowLines() const
{
    return _windowLines;
}

// 对应C++: int ScreenWindow::windowColumns() const
int ScreenWindow::windowColumns() const
{
    if (_screen == nullptr) {
        return 80; // default
    }
    return _screen->getColumns();
}

// 对应C++: int ScreenWindow::lineCount() const
int ScreenWindow::lineCount() const
{
    if (_screen == nullptr) {
        return 1; // default
    }
    return _screen->getHistLines() + _screen->getLines();
}

// 对应C++: int ScreenWindow::columnCount() const
int ScreenWindow::columnCount() const
{
    if (_screen == nullptr) {
        return 80; // default
    }
    return _screen->getColumns();
}

// 对应C++: int ScreenWindow::currentLine() const
int ScreenWindow::currentLine() const
{
    const int maxLine = qMax(0, lineCount() - windowLines());
    return qBound(0, _currentLine, maxLine);
}

// 对应C++: void ScreenWindow::scrollBy(RelativeScrollMode mode, int amount)
void ScreenWindow::scrollBy(RelativeScrollMode mode, int amount)
{
    if (mode == ScrollLines) {
        scrollTo(currentLine() + amount);
    } else if (mode == ScrollPages) {
        scrollTo(currentLine() + amount * (windowLines() / 2));
    }
}

// 对应C++: bool ScreenWindow::trackOutput() const
bool ScreenWindow::trackOutput() const
{
    return _trackOutput;
}

// 对应C++: void ScreenWindow::setTrackOutput(bool trackOutput)
void ScreenWindow::setTrackOutput(bool trackOutput)
{
    _trackOutput = trackOutput;
}

// 对应C++: int ScreenWindow::scrollCount() const
int ScreenWindow::scrollCount() const
{
    return _scrollCount;
}

// 对应C++: void ScreenWindow::resetScrollCount()
void ScreenWindow::resetScrollCount()
{
    _scrollCount = 0;
}

// 对应C++: QRect ScreenWindow::scrollRegion() const
QRect ScreenWindow::scrollRegion() const
{
    if (_screen == nullptr) {
        return QRect(0, 0, windowColumns(), windowLines());
    }

    const bool equalToScreenSize = (windowLines() == _screen->getLines());

    if (atEndOfOutput() && equalToScreenSize) {
        return _screen->lastScrolledRegion();
    } else {
        return QRect(0, 0, windowColumns(), windowLines());
    }
}

// 对应C++: void ScreenWindow::notifyOutputChanged()
void ScreenWindow::notifyOutputChanged()
{
    if (_screen == nullptr) {
        emit outputChanged();
        return;
    }

    // Move window to the bottom of the screen and update scroll count if this
    // window is currently tracking the bottom of the screen.
    if (_trackOutput) {
        _scrollCount -= _screen->scrolledLines();
        _currentLine = qMax(0, _screen->getHistLines() - (windowLines() - _screen->getLines()));
    } else {
        // If the history is not unlimited then it may have run out of space and
        // dropped the oldest lines of output - in this case the screen window's
        // current line number will need to be adjusted - otherwise the output
        // will scroll.
        _currentLine = qMax(0, _currentLine - _screen->droppedLines());

        // Ensure that the screen window's current position does not go beyond
        // the bottom of the screen.
        _currentLine = qMin(_currentLine, _screen->getHistLines());
    }

    _bufferNeedsUpdate = true;

    emit outputChanged();
}

// 对应C++: void ScreenWindow::handleCommandFromKeyboard(KeyboardTranslator::Command command)
void ScreenWindow::handleCommandFromKeyboard(KeyboardTranslatorCommands command)
{
    // Keyboard-based navigation
    bool update = false;

    // EraseCommand is handled in Vt102Emulation
    if (command & ScrollPageUpCommand) {
        scrollBy(ScreenWindow::ScrollPages, -1);
        update = true;
    }
    if (command & ScrollPageDownCommand) {
        scrollBy(ScreenWindow::ScrollPages, 1);
        update = true;
    }
    if (command & ScrollLineUpCommand) {
        scrollBy(ScreenWindow::ScrollLines, -1);
        update = true;
    }
    if (command & ScrollLineDownCommand) {
        scrollBy(ScreenWindow::ScrollLines, 1);
        update = true;
    }
    if (command & ScrollDownToBottomCommand) {
        emit scrollToEnd();
        update = true;
    }
    if (command & ScrollUpToTopCommand) {
        scrollTo(0);
        update = true;
    }
    // TODO: KeyboardTranslator::ScrollLockCommand
    // TODO: KeyboardTranslator::SendCommand

    if (update) {
        setTrackOutput(atEndOfOutput());
        emit outputChanged();
    }
}

// 对应C++: Character* ScreenWindow::getImage()
Character *ScreenWindow::getImage()
{
    if (_screen == nullptr) {
        return nullptr;
    }

    // Reallocate internal buffer if the window size has changed
    const int size = windowLines() * windowColumns();
    if (_windowBuffer == nullptr || _windowBufferSize != size) {
        delete[] _windowBuffer;
        _windowBuffer = new Character[size];
        _windowBufferSize = size;
        _bufferNeedsUpdate = true;
    }

    if (!_bufferNeedsUpdate) {
        return _windowBuffer;
    }

    _screen->getImage(_windowBuffer, size, currentLine(), endWindowLine());

    // This window may extend beyond the end of the screen, in which case there
    // is an unused area which needs to be filled with blank characters.
    fillUnusedArea();

    _bufferNeedsUpdate = false;
    return _windowBuffer;
}

// 对应C++: QVector<LineProperty> ScreenWindow::getLineProperties()
QVector<LineProperty> ScreenWindow::getLineProperties()
{
    if (_screen == nullptr) {
        return QVector<LineProperty>(windowLines(), LINE_DEFAULT);
    }

    QVector<LineProperty> result = _screen->getLineProperties(currentLine(), endWindowLine());

    if (result.count() != windowLines()) {
        result.resize(windowLines());
    }

    return result;
}

// 对应C++: QString ScreenWindow::selectedText(bool preserveLineBreaks) const
QString ScreenWindow::selectedText(bool preserveLineBreaks) const
{
    if (_screen == nullptr) {
        return QString();
    }

    return _screen->selectedText(preserveLineBreaks);
}

// 对应C++: int ScreenWindow::endWindowLine() const
int ScreenWindow::endWindowLine() const
{
    return qMin(currentLine() + windowLines() - 1, lineCount() - 1);
}

// 对应C++: QPoint ScreenWindow::cursorPosition() const
QPoint ScreenWindow::cursorPosition() const
{
    QPoint position;

    if (_screen != nullptr) {
        position.setX(_screen->getCursorX());
        position.setY(_screen->getCursorY());
    } else {
        position.setX(0);
        position.setY(0);
    }

    return position;
}

// 对应C++: bool ScreenWindow::atEndOfOutput() const
bool ScreenWindow::atEndOfOutput() const
{
    return currentLine() == qMax(0, lineCount() - windowLines());
}

// 对应C++: void ScreenWindow::scrollTo(int line)
void ScreenWindow::scrollTo(int line)
{
    const int maxCurrentLineNumber = qMax(0, lineCount() - windowLines());
    line = qBound(0, line, maxCurrentLineNumber);

    const int delta = line - _currentLine;
    _currentLine = line;

    // Keep track of the number of lines scrolled by; can be reset by resetScrollCount().
    _scrollCount += delta;

    _bufferNeedsUpdate = true;

    emit scrolled(_currentLine);
}

// 对应C++: void ScreenWindow::fillUnusedArea()
void ScreenWindow::fillUnusedArea()
{
    if (_screen == nullptr || _windowBuffer == nullptr) {
        return;
    }

    const int screenEndLine = _screen->getHistLines() + _screen->getLines() - 1;
    const int windowEndLine = currentLine() + windowLines() - 1;

    const int unusedLines = windowEndLine - screenEndLine;
    const int charsToFill = unusedLines * windowColumns();

    if (charsToFill > 0) {
        Screen::fillWithDefaultChar(_windowBuffer + _windowBufferSize - charsToFill, charsToFill);
    }
}

} // namespace Konsole
