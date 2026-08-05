#pragma once

// Emulation.h — C++ port of qtermwidget/emulation.py
//
// Abstract base class for terminal emulation back-ends. The back-end is
// responsible for decoding an incoming character stream and producing an
// output image of characters. Ported from the Python PySide6 version (itself
// converted from Konsole / QTermWidget, upstream Emulation.h).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
//   Copyright 1996 by Matthias Ettrich <ettrich@kde.org>

#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringDecoder>
#include <QTimer>

#include "Character.h"        // ExtendedCharTable (单一权威定义,不要重复定义)
#include "History.h"          // HistoryType
#include "KeyboardTranslator.h"

class QKeyEvent;

namespace Konsole {

class Screen;
class ScreenWindow;
class TerminalCharacterDecoder;

// Emulation state constants.
// 对应C++: enum { NOTIFYNORMAL=0, NOTIFYBELL=1, NOTIFYACTIVITY=2, NOTIFYSILENCE=3 };
inline constexpr int NOTIFYNORMAL   = 0;
inline constexpr int NOTIFYBELL     = 1;
inline constexpr int NOTIFYACTIVITY = 2;
inline constexpr int NOTIFYSILENCE  = 3;

// Available shapes for the keyboard cursor.
// 对应C++: enum class KeyboardCursorShape { BlockCursor = 0, UnderlineCursor = 1, IBeamCursor = 2 };
enum class KeyboardCursorShape {
    // A rectangular block which covers the entire cursor area.
    BlockCursor = 0,
    // A single flat line which occupies the bottom of the cursor area.
    UnderlineCursor = 1,
    // An cursor shaped like the capital letter 'I', similar to the IBeam
    // cursor used in Qt/KDE text editors.
    IBeamCursor = 2
};

// Base class for terminal emulation back-ends.
//
// The back-end is responsible for decoding an incoming character stream and
// producing an output image of characters.
//
// 对应C++: class Emulation : public QObject
class Emulation : public QObject {
    Q_OBJECT

public:
    // 对应C++: Emulation()
    explicit Emulation(QObject *parent = nullptr);
    // 对应C++: ~Emulation()
    ~Emulation() override;

    // Creates a new window onto the output from this emulation.
    // 对应C++: ScreenWindow* createWindow()
    ScreenWindow *createWindow();

    // Returns the screen currently shown by the emulation (primary or alternate).
    // 对应C++: Screen* currentScreen() const
    Screen *currentScreen() const { return _currentScreen; }

    // Returns the size of the screen image which the emulation produces.
    // 对应C++: QSize imageSize() const
    QSize imageSize() const;

    // Returns the total number of lines, including those stored in the history.
    // 对应C++: int lineCount() const
    int lineCount() const;

    // Sets the history store used by this emulation.
    // 对应C++: void setHistory(const HistoryType& t)
    void setHistory(const HistoryType &t);
    // Returns the history store used by this emulation.
    // 对应C++: const HistoryType& history() const
    const HistoryType &history() const;
    // Clears the history scroll.
    // 对应C++: void clearHistory()
    void clearHistory();

    // Copies the output history into a stream using the decoder.
    // 对应C++: virtual void writeToStream(TerminalCharacterDecoder* decoder, int startLine, int endLine)
    virtual void writeToStream(TerminalCharacterDecoder *decoder, int startLine, int endLine);

    // Returns the character used to erase with.
    // 对应C++: virtual char eraseChar() const
    virtual char eraseChar() const;

    // Sets the key bindings used to key events.
    // 对应C++: void setKeyBindings(const QString& name)
    void setKeyBindings(const QString &name);
    // Returns the name of the emulation's current key bindings.
    // 对应C++: QString keyBindings() const
    QString keyBindings() const;

    // Copies the current image into the history and clears the screen.
    // 对应C++: virtual void clearEntireScreen() =0
    virtual void clearEntireScreen() = 0;

    // 把光标移回左上角(0,0)。
    // 刻意与 clearEntireScreen() 分开:VT 的 ED(ESC[2J) 只擦内容不动光标,
    // 而 UI 上的"清屏"动作两件事都要做。cube-shell 扩展,上游 Konsole 无此方法。
    virtual void home() = 0;

    // Resets the state of the terminal.
    // 对应C++: virtual void reset() =0
    virtual void reset() = 0;

    // Returns true if the active terminal program wants mouse input events.
    // 对应C++: bool programUsesMouse() const
    bool programUsesMouse() const;
    // Returns true if bracketed paste mode is enabled.
    // 对应C++: bool programBracketedPasteMode() const
    bool programBracketedPasteMode() const;

public Q_SLOTS:
    // Change the size of the emulation's image.
    // 对应C++: virtual void setImageSize(int lines, int columns)
    virtual void setImageSize(int lines, int columns);

    // Interprets a sequence of characters and sends the result to the terminal.
    // 对应C++: virtual void sendText(const QString& text) = 0
    virtual void sendText(const QString &text) = 0;

    // Interprets a key press event and emits the sendData() signal with the
    // resulting byte stream.
    // 对应C++: virtual void sendKeyEvent(QKeyEvent* ev, bool fromPaste = false)
    virtual void sendKeyEvent(QKeyEvent *ev, bool fromPaste = false);

    // Converts information about a mouse event into an xterm-compatible escape
    // sequence and emits the sendData() signal. Default implementation does
    // nothing; subclasses override.
    // 对应C++: virtual void sendMouseEvent(int buttons, int column, int row, int eventType)
    virtual void sendMouseEvent(int buttons, int column, int row, int eventType);

    // Sends a string of characters to the foreground terminal process.
    // 对应C++: virtual void sendString(const char* string, int length = -1) = 0
    virtual void sendString(const char *string, int length = -1) = 0;

    // Processes an incoming character stream.
    // 对应C++: void receiveData(const char* buffer, int len)
    void receiveData(const char *buffer, int len);

Q_SIGNALS:
    // Emitted when the emulation has data to send to the program running
    // inside the terminal. This is the critical signal the SSH bridge connects
    // to (emulation.sendData.connect(...) in the Python version).
    // 对应C++: void sendData(const char* data, int len)
    void sendData(const char *data, int len);

    // Requests that the pty used by the terminal process be suspended/resumed.
    // 对应C++: void lockPtyRequest(bool suspend)
    void lockPtyRequest(bool suspend);

    // Requests that the pty's UTF-8 mode be turned on/off.
    // 对应C++: void useUtf8Request(bool)
    void useUtf8Request(bool);

    // Emitted when the state of the emulation changes (NOTIFYNORMAL etc.).
    // 对应C++: void stateSet(int state)
    void stateSet(int state);

    // Emitted when a zmodem transfer is detected in the output.
    // 对应C++: void zmodemDetected()
    void zmodemDetected();

    // Emitted when the program requests a change to the tab text color.
    // 对应C++: void changeTabTextColorRequest(int color)
    void changeTabTextColorRequest(int color);

    // Emitted when the program running in the terminal indicates whether it
    // wants mouse input.
    // 对应C++: void programUsesMouseChanged(bool usesMouse)
    void programUsesMouseChanged(bool usesMouse);

    // True=主屏(primary screen),False=备用屏(alternate screen,vim/less/htop)。
    // 显示层据此决定是否对内容做语法高亮。cube-shell 扩展信号,上游没有。
    void primaryScreenInUse(bool inUse);

    // 焦点上报模式 (DEC ?1004)。Claude Code 等在主屏内重绘的交互式 TUI 会开启此模式。
    // 显示层据此识别"主屏内的交互式应用"。cube-shell 扩展信号,上游没有。
    void programReportFocusChanged(bool reportFocus);

    // Emitted when bracketed paste mode is enabled/disabled.
    // 对应C++: void programBracketedPasteModeChanged(bool bracketedPasteMode)
    void programBracketedPasteModeChanged(bool bracketedPasteMode);

    // Emitted when the contents of the screen image change.
    // 对应C++: void outputChanged()
    void outputChanged();

    // Emitted when the terminal program requests to change the title/icon.
    // 对应C++: void titleChanged(int title, const QString& newTitle)
    void titleChanged(int title, const QString &newTitle);

    // Emitted when the set of lines/columns on the screen changes.
    // 对应C++: void imageSizeChanged(int lineCount, int columnCount)
    void imageSizeChanged(int lineCount, int columnCount);

    // Emitted when the image size is first set.
    // 对应C++: void imageSizeInitialized()
    void imageSizeInitialized();

    // Emitted when the terminal requests a resize.
    // 对应C++: void imageResizeRequest(const QSize& size)
    void imageResizeRequest(const QSize &size);

    // Emitted when the terminal program issues a profile change command.
    // 对应C++: void profileChangeCommandReceived(const QString& text)
    void profileChangeCommandReceived(const QString &text);

    // Emitted when a flow control key (Ctrl+S / Ctrl+Q) is pressed.
    // 对应C++: void flowControlKeyPressed(bool suspendKeyPressed)
    void flowControlKeyPressed(bool suspendKeyPressed);

    // Emitted when the cursor shape changes.
    // 对应C++: void cursorChanged(KeyboardCursorShape cursorShape, bool blinkingCursorEnabled)
    void cursorChanged(KeyboardCursorShape cursorShape, bool blinkingCursorEnabled);

    // Emitted when a command from the keyboard should be handled by a window.
    // 对应C++: void handleCommandFromKeyboard(KeyboardTranslator::Command command)
    // 注:本仓库 KeyboardTranslatorCommand 是自由枚举,连接伙伴 ScreenWindow 的槽
    // 形参是 QFlags(KeyboardTranslatorCommands),故信号用 QFlags 类型以便 PMF 直连;
    // 发射处传入单个枚举值会隐式转换。
    void handleCommandFromKeyboard(Konsole::KeyboardTranslatorCommands command);

    // Emitted after output produced in response to a key press.
    // 对应C++: void outputFromKeypressEvent(void)
    void outputFromKeypressEvent();

protected:
    // Processes a single unicode character of application input.
    // 对应C++: virtual void receiveChar(wchar_t ch)
    virtual void receiveChar(int ch);

    // Sets the active screen (0 = primary, 1 = alternate).
    // 对应C++: void setScreen(int n)
    void setScreen(int n);

    // Available character encoding codecs.
    // 对应C++: enum EmulationCodec { LocaleCodec = 0, Utf8Codec = 1 };
    enum EmulationCodec {
        LocaleCodec = 0,
        Utf8Codec   = 1
    };

    // Sets the codec used to decode the incoming character stream.
    // 对应C++: void setCodec(EmulationCodec codec) // 0 = locale, 1 = utf8
    void setCodec(EmulationCodec codec);

    // The windows connected to this emulation.
    // 对应C++: QList<ScreenWindow*> _windows
    QList<ScreenWindow *> _windows;

    // The currently active screen.
    // 对应C++: Screen* _currentScreen
    Screen *_currentScreen;

    // The two screens: 0 = primary, 1 = alternate.
    // 对应C++: Screen* _screen[2]
    Screen *_screen[2];

    // The key bindings table.
    // 对应C++: const KeyboardTranslator* _keyTranslator
    const KeyboardTranslator *_keyTranslator;

protected Q_SLOTS:
    // Schedules an update of attached views.
    // 对应C++: void bufferedUpdate()
    void bufferedUpdate();

private Q_SLOTS:
    // Emits outputChanged() and resets the timers.
    // 对应C++: void showBulk()
    void showBulk();
    // 对应C++: void usesMouseChanged(bool usesMouse)
    void usesMouseChanged(bool usesMouse);
    // 对应C++: void bracketedPasteModeChanged(bool bracketedPasteMode)
    void bracketedPasteModeChanged(bool bracketedPasteMode);
    // Forwards the cursor-shape change to the title-changed channel (id 50).
    // 对应 Python _onCursorChanged: re-emits titleChanged(50, "CursorShape=...")
    void _onCursorChanged(Konsole::KeyboardCursorShape cursorShape, bool blinkingEnabled);

private:
    // 对应C++: bool _usesMouse
    bool _usesMouse;
    // 对应C++: bool _bracketedPasteMode
    bool _bracketedPasteMode;

    // Bulk-update timers.
    // 对应C++: QTimer _bulkTimer1, _bulkTimer2
    QTimer _bulkTimer1;
    QTimer _bulkTimer2;

    // Incremental byte->UTF-16 decoder for the incoming stream.
    // 对应C++: QStringDecoder _toUtf16
    QStringDecoder _toUtf16;
};

} // namespace Konsole
