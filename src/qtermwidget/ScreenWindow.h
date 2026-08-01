#pragma once

// ScreenWindow.h — C++ port of qtermwidget/screen_window.py
//
// Provides a window/view onto a section of a terminal Screen. The terminal
// widget renders the window contents and uses the window to change the screen
// selection in response to mouse or keyboard input. Ported from the Python
// PySide6 version, which was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QVector>

#include "Character.h"
#include "KeyboardTranslator.h"

namespace Konsole {

class Screen; // fwd — the screen this window views (ported in parallel)

// Provides a window onto a section of a terminal screen.
// 对应C++: class ScreenWindow : public QObject
class ScreenWindow : public QObject {
    Q_OBJECT

public:
    // Describes the units used by scrollBy().
    // 对应C++: enum RelativeScrollMode { ScrollLines, ScrollPages }
    enum RelativeScrollMode {
        ScrollLines = 0, // Scroll the window by a number of lines
        ScrollPages = 1  // Scroll the window by a number of pages (one page = windowLines())
    };

    // Constructs a new screen window. setScreen() must be called before
    // getImage() or getLineProperties().
    //
    // You should not call this constructor directly; use Emulation::createWindow()
    // instead so the emulation can notify the window of changes and synchronize
    // selection updates across all views of a session.
    // 对应C++: ScreenWindow::ScreenWindow(QObject* parent = nullptr)
    explicit ScreenWindow(QObject *parent = nullptr);
    // 对应C++: ScreenWindow::~ScreenWindow()
    ~ScreenWindow() override;

    // Sets the screen which this window looks onto.
    // 对应C++: void ScreenWindow::setScreen(Screen* screen)
    void setScreen(Screen *screen);
    // Returns the screen which this window looks onto.
    // 对应C++: Screen* ScreenWindow::screen() const
    Screen *screen() const;

    // Returns the image of characters currently visible through this window.
    // The returned buffer is managed by the ScreenWindow instance.
    // 对应C++: Character* ScreenWindow::getImage()
    Character *getImage();

    // Returns the line properties associated with the lines currently visible
    // through this window.
    // 对应C++: QVector<LineProperty> ScreenWindow::getLineProperties()
    QVector<LineProperty> getLineProperties();

    // Returns the number of lines which the region of the window specified by
    // scrollRegion() has been scrolled by since the last call to resetScrollCount().
    // Not guaranteed to be accurate; allows views to optimize rendering.
    // 对应C++: int ScreenWindow::scrollCount() const
    int scrollCount() const;

    // Resets the count of scrolled lines returned by scrollCount().
    // 对应C++: void ScreenWindow::resetScrollCount()
    void resetScrollCount();

    // Returns the area of the window which was last scrolled. Usually the whole
    // window area. Not guaranteed to be accurate; allows views to optimize rendering.
    // 对应C++: QRect ScreenWindow::scrollRegion() const
    QRect scrollRegion() const;

    // Sets the start of the selection within the window.
    // 对应C++: void ScreenWindow::setSelectionStart(int column, int line, bool columnMode)
    void setSelectionStart(int column, int line, bool columnMode);
    // Sets the end of the selection within the window.
    // 对应C++: void ScreenWindow::setSelectionEnd(int column, int line)
    void setSelectionEnd(int column, int line);
    // Retrieves the start of the selection within the window.
    // 对应C++: void ScreenWindow::getSelectionStart(int& column, int& line)
    void getSelectionStart(int &column, int &line) const;
    // Retrieves the end of the selection within the window.
    // 对应C++: void ScreenWindow::getSelectionEnd(int& column, int& line)
    void getSelectionEnd(int &column, int &line) const;
    // Returns true if the character at (column, line) is part of the selection.
    // 对应C++: bool ScreenWindow::isSelected(int column, int line)
    bool isSelected(int column, int line) const;
    // Clears the current selection.
    // 对应C++: void ScreenWindow::clearSelection()
    void clearSelection();

    // Sets the number of lines in the window.
    // 对应C++: void ScreenWindow::setWindowLines(int lines)
    void setWindowLines(int lines);
    // Returns the number of lines in the window.
    // 对应C++: int ScreenWindow::windowLines() const
    int windowLines() const;
    // Returns the number of columns in the window.
    // 对应C++: int ScreenWindow::windowColumns() const
    int windowColumns() const;

    // Returns the total number of lines in the screen.
    // 对应C++: int ScreenWindow::lineCount() const
    int lineCount() const;
    // Returns the total number of columns in the screen.
    // 对应C++: int ScreenWindow::columnCount() const
    int columnCount() const;

    // Returns the index of the line which is currently at the top of this window.
    // 对应C++: int ScreenWindow::currentLine() const
    int currentLine() const;

    // Returns the position of the cursor within the window.
    // 对应C++: QPoint ScreenWindow::cursorPosition() const
    QPoint cursorPosition() const;

    // Convenience method. Returns true if the window is currently at the bottom
    // of the screen.
    // 对应C++: bool ScreenWindow::atEndOfOutput() const
    bool atEndOfOutput() const;

    // Scrolls the window so that @p line is at the top of the window.
    // 对应C++: void ScreenWindow::scrollTo(int line)
    void scrollTo(int line);

    // Scrolls the window relative to its current position on the screen.
    // 对应C++: void ScreenWindow::scrollBy(RelativeScrollMode mode, int amount)
    void scrollBy(RelativeScrollMode mode, int amount);

    // Specifies whether the window should automatically move to the bottom of
    // the screen when new output is added.
    // 对应C++: void ScreenWindow::setTrackOutput(bool trackOutput)
    void setTrackOutput(bool trackOutput);
    // Returns whether the window automatically moves to the bottom of the screen
    // when new output is added.
    // 对应C++: bool ScreenWindow::trackOutput() const
    bool trackOutput() const;

    // Returns the text which is currently selected.
    // 对应C++: QString ScreenWindow::selectedText(bool preserveLineBreaks) const
    QString selectedText(bool preserveLineBreaks) const;

public slots:
    // Notifies the window that the contents of the associated terminal screen
    // have changed. If trackOutput() is true this moves the window to the bottom
    // of the screen and emits outputChanged().
    // 对应C++: void ScreenWindow::notifyOutputChanged()
    void notifyOutputChanged();

    // Handles a command from the keyboard (keyboard-based navigation).
    // 对应C++: void ScreenWindow::handleCommandFromKeyboard(KeyboardTranslator::Command command)
    void handleCommandFromKeyboard(KeyboardTranslatorCommands command);

signals:
    // Emitted when the contents of the associated terminal screen change.
    // 对应C++: void outputChanged()
    void outputChanged();
    // Emitted when the screen window is scrolled to a different position.
    // Argument is the line number of the top of the window.
    // 对应C++: void scrolled(int line)
    void scrolled(int line);
    // Emitted when the selection is changed.
    // 对应C++: void selectionChanged()
    void selectionChanged();
    // Emitted to request a scroll to the end of the output.
    // 对应C++: void scrollToEnd()
    void scrollToEnd();

private:
    // Returns the index of the line at the end of this window, or the index of
    // the last line of the screen if this window goes beyond the end of the screen.
    // 对应C++: int ScreenWindow::endWindowLine() const
    int endWindowLine() const;

    // Fills the unused area at the bottom of the window (when the window extends
    // beyond the end of the screen) with default characters.
    // 对应C++: void ScreenWindow::fillUnusedArea()
    void fillUnusedArea();

    Screen *_screen;
    Character *_windowBuffer;
    int _windowBufferSize;
    bool _bufferNeedsUpdate;

    int _windowLines;
    int _currentLine;   // line to display at the top of the window
    bool _trackOutput;  // whether the window moves to the bottom on new output
    int _scrollCount;   // count of lines scrolled since resetScrollCount()
};

} // namespace Konsole
