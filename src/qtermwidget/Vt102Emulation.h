#pragma once

// Vt102Emulation.h — C++ port of qtermwidget/vt102_emulation.py
//
// Concrete VT102 / xterm terminal emulation: the escape-sequence parser and
// state machine. Subclasses Emulation, parses tokens out of the incoming byte
// stream and drives Screen operations. Ported from the Python PySide6 version,
// which was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robert.knight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QHash>
#include <QSize>
#include <QString>
#include <QStringEncoder>
#include <QTimer>
#include <QtGlobal>

#include "Character.h"        // vt100_graphics
#include "Screen.h"           // MODES_SCREEN, MODE_* (parallel port, contract name)
#include "Emulation.h"        // Emulation, KeyboardCursorShape (parallel port, contract name)
#include "KeyboardTranslator.h" // CTRL_MOD

class QKeyEvent;

namespace Konsole {

// ---------------------------------------------------------------------------
// VT102 mode constants (in addition to the screen modes in Screen.h).
// 对应C++: #define MODE_AppScreen (MODES_SCREEN+0) 等
// ---------------------------------------------------------------------------
// MODES_SCREEN / MODE_NewLine / MODE_Insert / MODE_Cursor come from Screen.h.
inline constexpr int MODE_AppScreen        = MODES_SCREEN + 0;  // Mode #1
inline constexpr int MODE_AppCuKeys        = MODES_SCREEN + 1;  // Application cursor keys (DECCKM)
inline constexpr int MODE_AppKeyPad        = MODES_SCREEN + 2;  // Application keypad
inline constexpr int MODE_Mouse1000        = MODES_SCREEN + 3;  // Send mouse X,Y on press and release
inline constexpr int MODE_Mouse1001        = MODES_SCREEN + 4;  // Highlight mouse tracking
inline constexpr int MODE_Mouse1002        = MODES_SCREEN + 5;  // Cell motion mouse tracking
inline constexpr int MODE_Mouse1003        = MODES_SCREEN + 6;  // All motion mouse tracking
inline constexpr int MODE_Mouse1005        = MODES_SCREEN + 7;  // Xterm extended coordinates
inline constexpr int MODE_Mouse1006        = MODES_SCREEN + 8;  // 2nd Xterm extended coordinates
inline constexpr int MODE_Mouse1015        = MODES_SCREEN + 9;  // Urxvt extended coordinates
inline constexpr int MODE_Ansi             = MODES_SCREEN + 10; // US ASCII for G0-G3 (DECANM)
inline constexpr int MODE_132Columns       = MODES_SCREEN + 11; // 80 <-> 132 column switch (DECCOLM)
inline constexpr int MODE_Allow132Columns  = MODES_SCREEN + 12; // Allow DECCOLM mode
inline constexpr int MODE_BracketedPaste   = MODES_SCREEN + 13; // Xterm bracketed paste mode
inline constexpr int MODE_total            = MODES_SCREEN + 14;

// ---------------------------------------------------------------------------
// Token type constructors — 32-bit packed int.
// 对应C++: #define TY_CONSTRUCT(T,A,N) ...
//   Bits 0-7  : Type (T)
//   Bits 8-15 : Character code (A)
//   Bits 16-31: Parameter / value (N)
// ---------------------------------------------------------------------------
inline constexpr int TY_CONSTRUCT(int T, int A, int N)
{
    return (((N & 0xffff) << 16) | ((A & 0xff) << 8) | (T & 0xff));
}
inline constexpr int TY_CHR()            { return TY_CONSTRUCT(0, 0, 0); }
inline constexpr int TY_CTL(int A)       { return TY_CONSTRUCT(1, A, 0); }
inline constexpr int TY_ESC(int A)       { return TY_CONSTRUCT(2, A, 0); }
inline constexpr int TY_ESC_CS(int A, int B) { return TY_CONSTRUCT(3, A, B); }
inline constexpr int TY_ESC_DE(int A)    { return TY_CONSTRUCT(4, A, 0); }
inline constexpr int TY_CSI_PS(int A, int N) { return TY_CONSTRUCT(5, A, N); }
inline constexpr int TY_CSI_PN(int A)    { return TY_CONSTRUCT(6, A, 0); }
inline constexpr int TY_CSI_PR(int A, int N) { return TY_CONSTRUCT(7, A, N); }
inline constexpr int TY_VT52(int A)      { return TY_CONSTRUCT(8, A, 0); }
inline constexpr int TY_CSI_PG(int A)    { return TY_CONSTRUCT(9, A, 0); }
inline constexpr int TY_CSI_PE(int A)    { return TY_CONSTRUCT(10, A, 0); }
inline constexpr int TY_CSI_PS_SP(int A, int N) { return TY_CONSTRUCT(11, A, N); }

// ---------------------------------------------------------------------------
// Constants — 对应C++: MAXARGS / MAX_TOKEN_LENGTH / ESC / DEL
// ---------------------------------------------------------------------------
inline constexpr int MAX_ARGUMENT     = 4096;
inline constexpr int MAX_TOKEN_LENGTH = 256;
inline constexpr int MAXARGS          = 15;
inline constexpr int ESC              = 27;
inline constexpr int DEL              = 127;

// Character class flags — 对应C++: #define CTL 1 等
inline constexpr int CTL = 1;   // Control character
inline constexpr int CHR = 2;   // Printable character
inline constexpr int CPN = 4;   // Parameter ending character
inline constexpr int DIG = 8;   // Digit
inline constexpr int SCS = 16;  // Character set selection
inline constexpr int GRP = 32;  // Group characters
inline constexpr int CPS = 64;  // End of window resize sequence

// 对应C++: #define CNTL(c) ((c)-'@')
inline constexpr int CNTL(char c) { return c - '@'; }

// ---------------------------------------------------------------------------
// Character set encoding info for one screen.
// 对应C++: struct CharCodes
// ---------------------------------------------------------------------------
struct CharCodes {
    // 对应C++: char charset[4]
    char charset[4] = {'B', 'B', 'B', 'B'};
    // 对应C++: int cu_cs
    int  cu_cs      = 0;
    // 对应C++: bool graphic
    bool graphic    = false;
    // 对应C++: bool pound
    bool pound      = false;
    // 对应C++: bool sa_graphic
    bool sa_graphic = false;
    // 对应C++: bool sa_pound
    bool sa_pound   = false;
};

// ---------------------------------------------------------------------------
// VT102 mode state.
// 对应C++: class TerminalState
// ---------------------------------------------------------------------------
class TerminalState {
public:
    // 对应C++: bool mode[MODE_total]
    bool mode[MODE_total] = {};
};

// ---------------------------------------------------------------------------
// Vt102Emulation — the concrete VT102/xterm emulation.
// 对应C++: class Vt102Emulation : public Emulation
// ---------------------------------------------------------------------------
class Vt102Emulation : public Emulation {
    Q_OBJECT
public:
    // 对应C++: Vt102Emulation()
    Vt102Emulation();
    // 对应C++: ~Vt102Emulation()
    ~Vt102Emulation() override;

    // --- reimplemented from Emulation ------------------------------------
    // 对应C++: void clearEntireScreen() override
    void clearEntireScreen() override;
    // 对应C++: void reset() override
    void reset() override;
    // 对应C++: char eraseChar() const override
    char eraseChar() const override;
    // 对应C++: void sendString(const char*, int length = -1) override
    void sendString(const char *s, int length = -1) override;
    // 对应C++: void sendText(const QString& text) override
    void sendText(const QString &text) override;
    // 对应C++: void sendKeyEvent(QKeyEvent*, bool fromPaste) override
    void sendKeyEvent(QKeyEvent *event, bool fromPaste = false) override;
    // 对应C++: void sendMouseEvent(int buttons, int column, int line, int eventType) override
    void sendMouseEvent(int cb, int cx, int cy, int eventType) override;

    // 对应C++: virtual void focusLost()
    virtual void focusLost();
    // 对应C++: virtual void focusGained()
    virtual void focusGained();

    // 是否处于 alternate screen（vim/less/top 等全屏 TUI）。
    // 对应Python: cube-shell.py::_should_disable_command_suggestions 中的
    // emu.getMode(MODE_AppScreen)（getMode 本身保持 private）
    bool isAppScreenMode() const { return getMode(MODE_AppScreen); }

public Q_SLOTS:
    // 对应C++: void updateTitle()
    void updateTitle();

protected:
    // 对应C++: void setMode(int mode)   // 注:Emulation.h 中 setMode/resetMode 非 virtual
    void setMode(int mode);
    // 对应C++: void resetMode(int mode)
    void resetMode(int mode);
    // 对应C++: void receiveChar(wchar_t cc)
    void receiveChar(int cc) override;

private:
    // --- token processing -------------------------------------------------
    void processToken(int token, int p, int q);
    void processWindowAttributeChange();

    // --- character sets ---------------------------------------------------
    int  applyCharset(int c);
    void setCharset(int n, char cs);
    void useCharset(int n);
    void setAndUseCharset(int n, char cs);
    void saveCursor();
    void restoreCursor();
    void resetCharset(int scrno);

    // --- margins / modes --------------------------------------------------
    void setMargins(int top, int bottom);
    void setDefaultMargins();
    bool getMode(int mode) const;
    void saveMode(int mode);
    void restoreMode(int mode);
    void resetModes();

    // --- tokenizer ---------------------------------------------------------
    void resetTokenizer();
    void addToCurrentToken(int cc);
    void addDigit(int digit);
    void addArgument();
    void initTokenizer();

    // --- reports -----------------------------------------------------------
    void reportDecodingError();
    void reportCursorPosition();
    void reportTerminalType();
    void reportSecondaryAttributes();
    void reportTerminalParms(int p);
    void reportStatus();
    void reportAnswerBack();

    // --- token dispatch helpers -------------------------------------------
    void processControlChar(int ch);
    void processEscapeSequence(int ch);
    void processCharsetSelection(int ch, int param);
    void processDecSequence(int ch);
    void processCsiPs(int ch, int param, int p, int q);
    void processSgr(int param, int p, int q);
    void processCsiPn(int ch, int p, int q);
    void processCsiPr(int ch, int param);
    void processPrivateMode(int modeNum, bool enable);
    void processCsiPsSp(int ch, int param);
    void processVt52(int ch, int p, int q);

    // --- tokenizer state predicates (mirror the C++ macros) ----------------
    bool lec(int P, int L, int C) const;
    bool lun() const;
    bool les(int P, int L, int C) const;
    bool eec(int C) const;
    bool ees(int C) const;
    bool eps(int C) const;
    bool epp() const;
    bool epe() const;
    bool egt() const;
    bool esp() const;
    bool Xpe() const;
    bool Xte(int cc) const;
    bool ces(int c, int cc) const;

    void clearScreenAndSetColumns(int columns);

private:
    // 对应C++: int prevCC
    int prevCC = 0;
    // 对应C++: QTimer* _titleUpdateTimer
    QTimer *_titleUpdateTimer;
    // 对应C++: bool _reportFocusEvents
    bool _reportFocusEvents = false;

    // 对应C++: CharCodes _charset[2]
    CharCodes _charset[2];

    // 对应C++: TerminalState _currentModes, _savedModes
    TerminalState _currentModes;
    TerminalState _savedModes;

    // 对应C++: wchar_t tokenBuffer[MAX_TOKEN_LENGTH]
    uint tokenBuffer[MAX_TOKEN_LENGTH] = {};
    // 对应C++: int tokenBufferPos
    int tokenBufferPos = 0;
    // 对应C++: int argc, argv[MAXARGS]
    int argc = 0;
    int argv[MAXARGS] = {};

    // 对应C++: int charClass[256]
    int charClass[256] = {};

    // 对应C++: QHash<int,QString> _pendingTitleUpdates
    QHash<int, QString> _pendingTitleUpdates;
};

} // namespace Konsole
