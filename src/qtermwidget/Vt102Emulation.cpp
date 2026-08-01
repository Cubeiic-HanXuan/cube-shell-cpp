// Vt102Emulation.cpp — C++ port of qtermwidget/vt102_emulation.py
//
// Concrete VT102 / xterm terminal emulation: escape-sequence parser / state
// machine. See Vt102Emulation.h for the class documentation.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robert.knight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include "Vt102Emulation.h"

#include <QKeyEvent>
#include <QEvent>

#include "Screen.h"           // Screen, MODES_SCREEN, MODE_* (parallel port, contract name)
#include "Character.h"        // RE_* rendition flags

// IS_WINDOWS — 对应 Python: IS_WINDOWS = sys.platform == 'win32'
#if defined(Q_OS_WIN)
static constexpr bool IS_WINDOWS = true;
#else
static constexpr bool IS_WINDOWS = false;
#endif

namespace Konsole {

// ===========================================================================
// Construction / destruction
// 对应C++: Vt102Emulation::Vt102Emulation() / ~Vt102Emulation()
// ===========================================================================

Vt102Emulation::Vt102Emulation()
    : Emulation()
    , _titleUpdateTimer(new QTimer(this))
{
    // 对应C++: _titleUpdateTimer->setSingleShot(true); connect(...)
    _titleUpdateTimer->setSingleShot(true);
    connect(_titleUpdateTimer, &QTimer::timeout, this, &Vt102Emulation::updateTitle);

    initTokenizer();
    reset();
}

Vt102Emulation::~Vt102Emulation() = default;

// ===========================================================================
// Public interface
// ===========================================================================

// 对应C++: void Vt102Emulation::clearEntireScreen()
void Vt102Emulation::clearEntireScreen()
{
    _currentScreen->clearEntireScreen();
    // 对应 Python: 清屏使用更高效的更新策略 —— 立即发射 outputChanged,不走批量定时器。
    // 注:Emulation._bulkTimer1/2 为 private,这里直接发射信号即可(上游 Konsole 亦如此)。
    Q_EMIT outputChanged();
}

// 对应C++: void Vt102Emulation::reset()
void Vt102Emulation::reset()
{
    resetTokenizer();
    resetModes();
    resetCharset(0);
    _screen[0]->reset();
    resetCharset(1);
    _screen[1]->reset();
    bufferedUpdate();
}

// 对应C++: char Vt102Emulation::eraseChar() const
char Vt102Emulation::eraseChar() const
{
    // Windows ConPTY 把 0x08(^H) 当作 Ctrl+Backspace(按词删除)，0x7f(DEL) 才是
    // 单字符退格，所以 Windows 上必须返回 0x7f。
    if (IS_WINDOWS)
        return '\x7f';
    return '\b';
}

// ===========================================================================
// Public slots
// ===========================================================================

// 对应C++: void Vt102Emulation::sendString(const char* s, int length)
void Vt102Emulation::sendString(const char *s, int length)
{
    Q_ASSERT(s != nullptr);
    QByteArray data = (length >= 0) ? QByteArray(s, length) : QByteArray(s);
    Q_EMIT sendData(data.constData(), data.size());
}

// 对应C++: void Vt102Emulation::sendText(const QString& text)
void Vt102Emulation::sendText(const QString &text)
{
    if (!text.isEmpty()) {
        // 对应C++: 创建 QKeyEvent 并走 sendKeyEvent 路径
        QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, text);
        sendKeyEvent(&event, false);
    }
}

// 对应C++: void Vt102Emulation::sendKeyEvent(QKeyEvent* event, bool fromPaste)
void Vt102Emulation::sendKeyEvent(QKeyEvent *event, bool fromPaste)
{
    Qt::KeyboardModifiers modifiers = event->modifiers();

    // 组装当前终端状态标志 —— 对应C++的 states 获取逻辑
    KeyboardTranslatorStates states = NoState;
    if (getMode(MODE_NewLine))
        states |= NewLineState;
    if (getMode(MODE_Ansi))
        states |= AnsiState;
    if (getMode(MODE_AppCuKeys))
        states |= CursorKeysState;
    if (getMode(MODE_AppScreen))
        states |= AlternateScreenState;
    if (getMode(MODE_AppKeyPad) && (modifiers & Qt::KeypadModifier))
        states |= ApplicationKeypadState;

    // 流控制按键
    if (modifiers & CTRL_MOD) {
        if (event->key() == Qt::Key_S)
            Q_EMIT flowControlKeyPressed(true);
        else if (event->key() == Qt::Key_Q || event->key() == Qt::Key_C)
            Q_EMIT flowControlKeyPressed(false);
    }

    if (_keyTranslator) {
        // 查找键映射条目
        KeyboardTranslatorEntry entry = _keyTranslator->findEntry(event->key(), modifiers, states);

        QByteArray textToSend;

        // Alt / Meta 修饰符特殊处理
        const bool wantsAltModifier  = (entry.modifiers() & entry.modifierMask() & Qt::AltModifier);
        const bool wantsMetaModifier = (entry.modifiers() & entry.modifierMask() & Qt::MetaModifier);
        const bool wantsAnyModifier  = (entry.state() & entry.stateMask() & AnyModifierState);

        if ((modifiers & Qt::AltModifier) && !(wantsAltModifier || wantsAnyModifier)
            && !event->text().isEmpty())
            textToSend.prepend("\033");

        if ((modifiers & Qt::MetaModifier) && !(wantsMetaModifier || wantsAnyModifier)
            && !event->text().isEmpty())
            textToSend.prepend("\030@s");

        // 处理命令或文本
        if (entry.command() != NoCommand) {
            if (entry.command() & EraseCommand) {
                textToSend += eraseChar();
            } else {
                Q_EMIT handleCommandFromKeyboard(entry.command());
            }
        } else if (!entry.text().isEmpty()) {
            // 应用通配符扩展 —— 对应C++: entry.text(true, modifiers)
            textToSend += entry.text(true, modifiers);
        } else if ((modifiers & CTRL_MOD) && event->key() >= 0x40 && event->key() < 0x5f) {
            // Ctrl+字母组合
            textToSend += static_cast<char>(event->key() & 0x1f);
        } else if (event->key() == Qt::Key_Tab) {
            textToSend += '\x09';
        } else if (event->key() == Qt::Key_PageUp) {
            textToSend += "\033[5~";
        } else if (event->key() == Qt::Key_PageDown) {
            textToSend += "\033[6~";
        } else if (!event->text().isEmpty()) {
            textToSend += event->text().toUtf8();
        }

        // Windows ConPTY 退格修正:普通 Backspace -> 0x7f,Ctrl+Backspace -> 0x08
        if (IS_WINDOWS && event->key() == Qt::Key_Backspace && !textToSend.isEmpty()) {
            if (modifiers & CTRL_MOD)
                textToSend.replace('\x7f', '\b');
            else
                textToSend.replace('\b', '\x7f');
        }

        if (!fromPaste && !textToSend.isEmpty())
            Q_EMIT outputFromKeypressEvent();

        if (!textToSend.isEmpty())
            Q_EMIT sendData(textToSend.constData(), textToSend.size());
    } else {
        // 没有键盘翻译器的错误处理
        static const char errorMsg[] =
            "No keyboard translator available. The information needed to convert "
            "key presses into characters to send to the terminal is missing.";
        reset();
        receiveData(errorMsg, static_cast<int>(sizeof(errorMsg) - 1));
    }
}

// 对应C++: void Vt102Emulation::sendMouseEvent(int cb, int cx, int cy, int eventType)
void Vt102Emulation::sendMouseEvent(int cb, int cx, int cy, int eventType)
{
    if (cx < 1 || cy < 1)
        return;

    // 按钮释放编码(除了 1006 模式)
    if (eventType == 2 && !getMode(MODE_Mouse1006))
        cb = 3;

    // 普通按钮编码为 0x20 + button;滚轮(buttons 4,5)编码为 0x5c + button
    if (cb >= 4)
        cb += 0x3c;

    // 鼠标移动处理
    if ((getMode(MODE_Mouse1002) || getMode(MODE_Mouse1003)) && eventType == 1)
        cb += 0x20; // 添加 32 表示移动事件

    char command[32];
    command[0] = '\0';

    // 检查扩展(按偏好递减顺序)
    if (getMode(MODE_Mouse1006)) {
        snprintf(command, sizeof(command), "\033[<%d;%d;%d%c",
                 cb, cx, cy, (eventType == 2 ? 'm' : 'M'));
    } else if (getMode(MODE_Mouse1015)) {
        snprintf(command, sizeof(command), "\033[%d;%d;%dM", cb + 0x20, cx, cy);
    } else if (getMode(MODE_Mouse1005)) {
        if (cx <= 2015 && cy <= 2015) {
            // UTF-8 编码坐标
            QString coords;
            coords += QChar(cx + 0x20);
            coords += QChar(cy + 0x20);
            QByteArray utf8 = coords.toUtf8();
            snprintf(command, sizeof(command), "\033[M%c%s",
                     static_cast<char>(cb + 0x20), utf8.constData());
        }
    } else if (cx <= 223 && cy <= 223) {
        snprintf(command, sizeof(command), "\033[M%c%c%c",
                 static_cast<char>(cb + 0x20),
                 static_cast<char>(cx + 0x20),
                 static_cast<char>(cy + 0x20));
    }

    if (command[0] != '\0')
        sendString(command);
}

// 对应C++: void Vt102Emulation::focusLost()
void Vt102Emulation::focusLost()
{
    if (_reportFocusEvents)
        sendString("\033[O");
}

// 对应C++: void Vt102Emulation::focusGained()
void Vt102Emulation::focusGained()
{
    if (_reportFocusEvents)
        sendString("\033[I");
}

// ===========================================================================
// Protected — mode set/reset
// ===========================================================================

// 对应C++: void Vt102Emulation::setMode(int mode)
void Vt102Emulation::setMode(int mode)
{
    _currentModes.mode[mode] = true;

    if (mode == MODE_132Columns) {
        if (getMode(MODE_Allow132Columns))
            clearScreenAndSetColumns(132);
        else
            _currentModes.mode[mode] = false;
    } else if (mode == MODE_Mouse1000 || mode == MODE_Mouse1001 ||
               mode == MODE_Mouse1002 || mode == MODE_Mouse1003) {
        Q_EMIT programUsesMouseChanged(false);
    } else if (mode == MODE_BracketedPaste) {
        Q_EMIT programBracketedPasteModeChanged(true);
    } else if (mode == MODE_AppScreen) {
        _screen[1]->clearSelection();
        setScreen(1);
        // 进入备用屏(全屏 TUI),通知显示层关闭语法高亮
        Q_EMIT primaryScreenInUse(false);
    }

    if (mode < MODES_SCREEN) {
        _screen[0]->setMode(mode);
        _screen[1]->setMode(mode);
    }
}

// 对应C++: void Vt102Emulation::resetMode(int mode)
void Vt102Emulation::resetMode(int mode)
{
    _currentModes.mode[mode] = false;

    if (mode == MODE_132Columns) {
        if (getMode(MODE_Allow132Columns))
            clearScreenAndSetColumns(80);
    } else if (mode == MODE_Mouse1000 || mode == MODE_Mouse1001 ||
               mode == MODE_Mouse1002 || mode == MODE_Mouse1003) {
        Q_EMIT programUsesMouseChanged(true);
    } else if (mode == MODE_BracketedPaste) {
        Q_EMIT programBracketedPasteModeChanged(false);
    } else if (mode == MODE_AppScreen) {
        _screen[0]->clearSelection();
        setScreen(0);
        // 退回主屏(shell),通知显示层恢复语法高亮
        Q_EMIT primaryScreenInUse(true);
    }

    if (mode < MODES_SCREEN) {
        _screen[0]->resetMode(mode);
        _screen[1]->resetMode(mode);
    }
}

// 对应C++: void Vt102Emulation::clearScreenAndSetColumns(int columns)
// (Python 版调用此方法但从未定义 —— 移植 bug,这里按上游 Konsole 补全)
void Vt102Emulation::clearScreenAndSetColumns(int columns)
{
    clearEntireScreen();
    setImageSize(_currentScreen->getLines(), columns);
    Q_EMIT imageResizeRequest(QSize(columns, _currentScreen->getLines()));
}

// ===========================================================================
// Core receive state machine
// 对应C++: void Vt102Emulation::receiveChar(wchar_t cc)
// ===========================================================================

void Vt102Emulation::receiveChar(int cc)
{
    if (cc == DEL)
        return; // VT100: ignore.

    if (ces(CTL, cc)) {
        // 忽略 OSC "ESC]" 文本部分的控制字符
        if (Xpe()) {
            prevCC = cc;
            return;
        }

        // DEC HACK: 控制字符允许出现在 ESC 序列内部
        if (cc == CNTL('X') || cc == CNTL('Z') || cc == ESC)
            resetTokenizer(); // VT100: CAN or SUB
        if (cc != ESC) {
            processToken(TY_CTL(cc + '@'), 0, 0);
            return;
        }
    }

    // 推进状态
    addToCurrentToken(cc);

    uint *s = tokenBuffer;
    const int p = tokenBufferPos;

    if (getMode(MODE_Ansi)) {
        if (lec(1, 0, ESC))
            return;
        if (lec(1, 0, ESC + 128)) {
            s[0] = ESC;
            receiveChar('[');
            return;
        }
        if (les(2, 1, GRP))
            return;
        if (Xte(cc)) {
            processWindowAttributeChange();
            resetTokenizer();
            return;
        }
        if (Xpe()) {
            prevCC = cc;
            return;
        }
        if (lec(3, 2, '?'))
            return;
        if (lec(3, 2, '>'))
            return;
        if (lec(3, 2, '!'))
            return;
        if (lun()) {
            processToken(TY_CHR(), applyCharset(cc), 0);
            resetTokenizer();
            return;
        }
        if (lec(2, 0, ESC)) {
            processToken(TY_ESC(s[1]), 0, 0);
            resetTokenizer();
            return;
        }
        if (les(3, 1, SCS)) {
            processToken(TY_ESC_CS(s[1], s[2]), 0, 0);
            resetTokenizer();
            return;
        }
        if (lec(3, 1, '#')) {
            processToken(TY_ESC_DE(s[2]), 0, 0);
            resetTokenizer();
            return;
        }
        if (eps(CPN)) {
            processToken(TY_CSI_PN(cc), argv[0], argv[1]);
            resetTokenizer();
            return;
        }
        if (esp())
            return;
        if (lec(5, 4, 'q') && s[3] == ' ') {
            processToken(TY_CSI_PS_SP(cc, argv[0]), argv[0], 0);
            resetTokenizer();
            return;
        }

        // resize = \e[8;<row>;<col>t
        if (eps(CPS)) {
            processToken(TY_CSI_PS(cc, argv[0]), argv[1], argv[2]);
            resetTokenizer();
            return;
        }

        if (epe()) {
            processToken(TY_CSI_PE(cc), 0, 0);
            resetTokenizer();
            return;
        }
        if (ees(DIG)) {
            addDigit(cc - '0');
            return;
        }
        if (eec(';') || eec(':')) {
            addArgument();
            return;
        }

        for (int i = 0; i <= argc; i++) {
            if (epp()) {
                processToken(TY_CSI_PR(cc, argv[i]), 0, 0);
            } else if (egt()) {
                processToken(TY_CSI_PG(cc), 0, 0); // spec. case for ESC]>0c or ESC]>c
            } else if (cc == 'm' && argc - i >= 4 &&
                       (argv[i] == 38 || argv[i] == 48) && argv[i + 1] == 2) {
                // ESC[ ... 48;2;<r>;<g>;<b> ... m  -or-  ESC[ ... 38;2;<r>;<g>;<b> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i - 2]), COLOR_SPACE_RGB,
                             (argv[i] << 16) | (argv[i + 1] << 8) | argv[i + 2]);
                i += 2;
            } else if (cc == 'm' && argc - i >= 2 &&
                       (argv[i] == 38 || argv[i] == 48) && argv[i + 1] == 5) {
                // ESC[ ... 48;5;<index> ... m  -or-  ESC[ ... 38;5;<index> ... m
                i += 2;
                processToken(TY_CSI_PS(cc, argv[i - 2]), COLOR_SPACE_256, argv[i]);
                i += 2;
            } else {
                processToken(TY_CSI_PS(cc, argv[i]), 0, 0);
            }
        }
        resetTokenizer();
    } else {
        // VT52 Mode
        if (lec(1, 0, ESC))
            return;
        if (les(1, 0, CHR)) {
            processToken(TY_CHR(), s[0], 0);
            resetTokenizer();
            return;
        }
        if (lec(2, 1, 'Y'))
            return;
        if (lec(3, 1, 'Y'))
            return;
        if (p < 4) {
            processToken(TY_VT52(s[1]), 0, 0);
            resetTokenizer();
            return;
        }
        processToken(TY_VT52(s[1]), s[2], s[3]);
        resetTokenizer();
        return;
    }
}

// 对应C++: void Vt102Emulation::processWindowAttributeChange()
void Vt102Emulation::processWindowAttributeChange()
{
    int attributeToChange = 0;
    int i = 2;

    // 解析属性编号
    while (i < tokenBufferPos && tokenBuffer[i] >= '0' && tokenBuffer[i] <= '9') {
        attributeToChange = 10 * attributeToChange + (tokenBuffer[i] - '0');
        ++i;
    }

    if (i >= tokenBufferPos || tokenBuffer[i] != ';') {
        reportDecodingError();
        return;
    }

    // 从分号后的第一个字符开始;减 2 跳过结束标记
    const int startPos = i + 1;
    int length = tokenBufferPos - i - 2;
    if (length < 0)
        length = 0;

    QString newValue;
    newValue.reserve(length);
    for (int j = startPos; j < startPos + length; ++j) {
        if (j < tokenBufferPos)
            newValue += QChar(tokenBuffer[j]);
    }

    _pendingTitleUpdates[attributeToChange] = newValue;
    _titleUpdateTimer->start(20);
}

// ===========================================================================
// Private slots
// ===========================================================================

// 对应C++: void Vt102Emulation::updateTitle()
void Vt102Emulation::updateTitle()
{
    const auto keys = _pendingTitleUpdates.keys();
    for (int arg : keys)
        Q_EMIT titleChanged(arg, _pendingTitleUpdates[arg]);
    _pendingTitleUpdates.clear();
}

// ===========================================================================
// Character sets
// ===========================================================================

// 对应C++: int Vt102Emulation::applyCharset(int c)
int Vt102Emulation::applyCharset(int c)
{
    CharCodes &charset = _charset[_currentScreen == _screen[1] ? 1 : 0];

    if (charset.graphic && c >= 0x5f && c <= 0x7e)
        return vt100_graphics[c - 0x5f];
    if (charset.pound && c == '#')
        return 0xa3; // 英镑符号
    return c;
}

// 对应C++: void Vt102Emulation::setCharset(int n, char cs)
void Vt102Emulation::setCharset(int n, char cs)
{
    _charset[0].charset[n & 3] = cs;
    useCharset(_charset[0].cu_cs);
    _charset[1].charset[n & 3] = cs;
    useCharset(_charset[1].cu_cs);
}

// 对应C++: void Vt102Emulation::useCharset(int n)
void Vt102Emulation::useCharset(int n)
{
    CharCodes &charset = _charset[_currentScreen == _screen[1] ? 1 : 0];
    charset.cu_cs   = n & 3;
    charset.graphic = (charset.charset[n & 3] == '0');
    charset.pound   = (charset.charset[n & 3] == 'A');
}

// 对应C++: void Vt102Emulation::setAndUseCharset(int n, char cs)
void Vt102Emulation::setAndUseCharset(int n, char cs)
{
    CharCodes &charset = _charset[_currentScreen == _screen[1] ? 1 : 0];
    charset.charset[n & 3] = cs;
    useCharset(n & 3);
}

// 对应C++: void Vt102Emulation::saveCursor()
void Vt102Emulation::saveCursor()
{
    CharCodes &charset = _charset[_currentScreen == _screen[1] ? 1 : 0];
    charset.sa_graphic = charset.graphic;
    charset.sa_pound   = charset.pound;
    _currentScreen->saveCursor();
}

// 对应C++: void Vt102Emulation::restoreCursor()
void Vt102Emulation::restoreCursor()
{
    CharCodes &charset = _charset[_currentScreen == _screen[1] ? 1 : 0];
    charset.graphic = charset.sa_graphic;
    charset.pound   = charset.sa_pound;
    _currentScreen->restoreCursor();
}

// 对应C++: void Vt102Emulation::resetCharset(int scrno)
void Vt102Emulation::resetCharset(int scrno)
{
    _charset[scrno].cu_cs = 0;
    _charset[scrno].charset[0] = 'B';
    _charset[scrno].charset[1] = 'B';
    _charset[scrno].charset[2] = 'B';
    _charset[scrno].charset[3] = 'B';
    _charset[scrno].sa_graphic = false;
    _charset[scrno].sa_pound   = false;
    _charset[scrno].graphic    = false;
    _charset[scrno].pound      = false;
}

// ===========================================================================
// Margins / modes
// ===========================================================================

// 对应C++: void Vt102Emulation::setMargins(int top, int bottom)
void Vt102Emulation::setMargins(int top, int bottom)
{
    _screen[0]->setMargins(top, bottom);
    _screen[1]->setMargins(top, bottom);
}

// 对应C++: void Vt102Emulation::setDefaultMargins()
void Vt102Emulation::setDefaultMargins()
{
    _screen[0]->setDefaultMargins();
    _screen[1]->setDefaultMargins();
}

// 对应C++: bool Vt102Emulation::getMode(int mode)
bool Vt102Emulation::getMode(int mode) const
{
    return _currentModes.mode[mode];
}

// 对应C++: void Vt102Emulation::saveMode(int mode)
void Vt102Emulation::saveMode(int mode)
{
    _savedModes.mode[mode] = _currentModes.mode[mode];
}

// 对应C++: void Vt102Emulation::restoreMode(int mode)
void Vt102Emulation::restoreMode(int mode)
{
    if (_savedModes.mode[mode])
        setMode(mode);
    else
        resetMode(mode);
}

// 对应C++: void Vt102Emulation::resetModes()
void Vt102Emulation::resetModes()
{
    static const int modesToReset[] = {
        MODE_132Columns, MODE_Mouse1000, MODE_Mouse1001, MODE_Mouse1002,
        MODE_Mouse1003, MODE_Mouse1005, MODE_Mouse1006, MODE_Mouse1015,
        MODE_BracketedPaste, MODE_AppScreen, MODE_AppCuKeys, MODE_AppKeyPad
    };

    for (int mode : modesToReset) {
        resetMode(mode);
        saveMode(mode);
    }

    resetMode(MODE_NewLine);
    setMode(MODE_Ansi);
}

// ===========================================================================
// Tokenizer
// ===========================================================================

// 对应C++: void Vt102Emulation::resetTokenizer()
void Vt102Emulation::resetTokenizer()
{
    tokenBufferPos = 0;
    argc = 0;
    argv[0] = 0;
    argv[1] = 0;
    prevCC = 0;
}

// 对应C++: void Vt102Emulation::addToCurrentToken(int cc)
void Vt102Emulation::addToCurrentToken(int cc)
{
    tokenBuffer[tokenBufferPos] = cc;
    tokenBufferPos = qMin(tokenBufferPos + 1, MAX_TOKEN_LENGTH - 1);
}

// 对应C++: void Vt102Emulation::addDigit(int digit)
void Vt102Emulation::addDigit(int digit)
{
    if (argv[argc] < MAX_ARGUMENT)
        argv[argc] = 10 * argv[argc] + digit;
}

// 对应C++: void Vt102Emulation::addArgument()
void Vt102Emulation::addArgument()
{
    argc = qMin(argc + 1, MAXARGS - 1);
    argv[argc] = 0;
}

// 对应C++: void Vt102Emulation::initTokenizer()
void Vt102Emulation::initTokenizer()
{
    for (int i = 0; i < 256; ++i)
        charClass[i] = 0;
    // 控制字符 (0-31)
    for (int i = 0; i < 32; ++i)
        charClass[i] |= CTL;
    // 可打印字符 (32-255)
    for (int i = 32; i < 256; ++i)
        charClass[i] |= CHR;

    // 参数结束字符
    for (const char ch : QByteArrayLiteral("@ABCDEFGHILMPSTXZbcdfry"))
        charClass[static_cast<uchar>(ch)] |= CPN;
    // 窗口尺寸改变序列结束字符
    for (const char ch : QByteArrayLiteral("t"))
        charClass[static_cast<uchar>(ch)] |= CPS;
    // 数字字符
    for (const char ch : QByteArrayLiteral("0123456789"))
        charClass[static_cast<uchar>(ch)] |= DIG;
    // 字符集选择字符
    for (const char ch : QByteArrayLiteral("()+*%"))
        charClass[static_cast<uchar>(ch)] |= SCS;
    // 组字符
    for (const char ch : QByteArrayLiteral("()+*#[]%"))
        charClass[static_cast<uchar>(ch)] |= GRP;

    resetTokenizer();
}

// ===========================================================================
// Tokenizer state predicates — mirror the C++ macros to keep logic identical
// ===========================================================================

bool Vt102Emulation::lec(int P, int L, int C) const
{
    return tokenBufferPos == P && L < tokenBufferPos && tokenBuffer[L] == static_cast<uint>(C);
}

bool Vt102Emulation::lun() const
{
    return tokenBufferPos == 1 && tokenBuffer[0] >= 32;
}

bool Vt102Emulation::les(int P, int L, int C) const
{
    return tokenBufferPos == P && L < tokenBufferPos &&
           tokenBuffer[L] < 256 && (charClass[tokenBuffer[L]] & C) == C;
}

bool Vt102Emulation::eec(int C) const
{
    return tokenBufferPos >= 3 && tokenBuffer[tokenBufferPos - 1] == static_cast<uint>(C);
}

bool Vt102Emulation::ees(int C) const
{
    const uint cc = tokenBufferPos > 0 ? tokenBuffer[tokenBufferPos - 1] : 0;
    return tokenBufferPos >= 3 && cc < 256 && (charClass[cc] & C) == C;
}

bool Vt102Emulation::eps(int C) const
{
    if (tokenBufferPos < 3 ||
        tokenBuffer[2] == '?' || tokenBuffer[2] == '!' || tokenBuffer[2] == '>')
        return false;
    const uint cc = tokenBufferPos > 0 ? tokenBuffer[tokenBufferPos - 1] : 0;
    return cc < 256 && (charClass[cc] & C) == C;
}

bool Vt102Emulation::epp() const
{
    return tokenBufferPos >= 3 && tokenBuffer[2] == '?';
}

bool Vt102Emulation::epe() const
{
    return tokenBufferPos >= 3 && tokenBuffer[2] == '!';
}

bool Vt102Emulation::egt() const
{
    return tokenBufferPos >= 3 && tokenBuffer[2] == '>';
}

bool Vt102Emulation::esp() const
{
    return tokenBufferPos == 4 && tokenBuffer[3] == ' ';
}

bool Vt102Emulation::Xpe() const
{
    return tokenBufferPos >= 2 && tokenBuffer[1] == ']';
}

bool Vt102Emulation::Xte(int cc) const
{
    return Xpe() && (cc == 7 || (prevCC == 27 && cc == 92));
}

bool Vt102Emulation::ces(int c, int cc) const
{
    return cc < 256 && (charClass[cc] & c) == c && !Xte(cc);
}

// ===========================================================================
// Reports
// ===========================================================================

// 对应C++: void Vt102Emulation::reportDecodingError()
void Vt102Emulation::reportDecodingError()
{
    if (tokenBufferPos == 0 ||
        (tokenBufferPos == 1 && (tokenBuffer[0] & 0xff) >= 32))
        return;
    QString tokenStr;
    tokenStr.reserve(tokenBufferPos);
    for (int i = 0; i < tokenBufferPos; ++i)
        tokenStr += QChar(tokenBuffer[i]);
    qWarning("Undecodable sequence: %s", qPrintable(tokenStr));
}

// 对应C++: void Vt102Emulation::reportCursorPosition()
void Vt102Emulation::reportCursorPosition()
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "\033[%d;%dR",
             _currentScreen->getCursorY() + 1, _currentScreen->getCursorX() + 1);
    sendString(tmp);
}

// 对应C++: void Vt102Emulation::reportTerminalType()
void Vt102Emulation::reportTerminalType()
{
    if (getMode(MODE_Ansi))
        sendString("\033[?1;2c"); // VT100
    else
        sendString("\033/Z");     // VT52
}

// 对应C++: void Vt102Emulation::reportSecondaryAttributes()
void Vt102Emulation::reportSecondaryAttributes()
{
    if (getMode(MODE_Ansi))
        sendString("\033[>0;115;0c");
    else
        sendString("\033/Z");
}

// 对应C++: void Vt102Emulation::reportTerminalParms(int p)
void Vt102Emulation::reportTerminalParms(int p)
{
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "\033[%d;1;1;112;112;1;0x", p);
    sendString(tmp);
}

// 对应C++: void Vt102Emulation::reportStatus()
void Vt102Emulation::reportStatus()
{
    sendString("\033[0n"); // 0 = Ready
}

// 对应C++: void Vt102Emulation::reportAnswerBack()
void Vt102Emulation::reportAnswerBack()
{
    sendString(""); // 默认为空
}

// ===========================================================================
// Token dispatch
// 对应C++: void Vt102Emulation::processToken(int token, wchar_t p, int q)
// ===========================================================================

void Vt102Emulation::processToken(int token, int p, int q)
{
    const int tokenType = token & 0xff;
    const int ch        = (token >> 8) & 0xff;
    const int param     = (token >> 16) & 0xffff;

    switch (tokenType) {
    case 0: // TY_CHR — 显示字符
        _currentScreen->displayCharacter(p <= 0x10FFFF ? static_cast<uint>(p) : static_cast<uint>(' '));
        break;
    case 1: // TY_CTL
        processControlChar(ch);
        break;
    case 2: // TY_ESC
        processEscapeSequence(ch);
        break;
    case 3: // TY_ESC_CS
        processCharsetSelection(ch, param);
        break;
    case 4: // TY_ESC_DE
        processDecSequence(ch);
        break;
    case 5: // TY_CSI_PS
        processCsiPs(ch, param, p, q);
        break;
    case 6: // TY_CSI_PN
        processCsiPn(ch, p, q);
        break;
    case 7: // TY_CSI_PR
        processCsiPr(ch, param);
        break;
    case 8: // TY_VT52
        processVt52(ch, p, q);
        break;
    case 9: // TY_CSI_PG
        if (ch == 'c')
            reportSecondaryAttributes();
        break;
    case 10: // TY_CSI_PE
        break; // 暂不处理
    case 11: // TY_CSI_PS_SP
        processCsiPsSp(ch, param);
        break;
    default:
        reportDecodingError();
        break;
    }
}

// 对应 Python: _process_control_char
void Vt102Emulation::processControlChar(int ch)
{
    switch (ch) {
    case 'H': _currentScreen->backspace(); break;             // BS
    case 'I': _currentScreen->tab(); break;                   // HT
    case 'J':
    case 'K':
    case 'L': _currentScreen->newLine(); break;               // LF, VT, FF
    case 'M': _currentScreen->toStartOfLine(); break;         // CR
    case 'G': Q_EMIT stateSet(1); break;                      // BEL (NOTIFYBELL)
    case 'E': reportAnswerBack(); break;                      // ENQ
    case 'N': useCharset(1); break;                           // SO (Use G1)
    case 'O': useCharset(0); break;                           // SI (Use G0)
    case 'X':
    case 'Z': _currentScreen->displayCharacter(0x2592); break; // CAN, SUB
    default: break; // 忽略其他字符
    }
}

// 对应 Python: _process_escape_sequence
void Vt102Emulation::processEscapeSequence(int ch)
{
    switch (ch) {
    case 'D': _currentScreen->index(); break;                 // IND
    case 'E': _currentScreen->nextLine(); break;              // NEL
    case 'H': _currentScreen->changeTabStop(true); break;     // HTS
    case 'M': _currentScreen->reverseIndex(); break;          // RI
    case 'Z': reportTerminalType(); break;                    // DECID
    case 'c': reset(); break;                                 // RIS
    case 'n': useCharset(2); break;                           // LS2
    case 'o': useCharset(3); break;                           // LS3
    case '7': saveCursor(); break;                            // DECSC
    case '8': restoreCursor(); break;                         // DECRC
    case '=': setMode(MODE_AppKeyPad); break;                 // DECKPAM
    case '>': resetMode(MODE_AppKeyPad); break;               // DECKPNM
    case '<': setMode(MODE_Ansi); break;                      // VT52 -> ANSI
    default: break;
    }
}

// 对应 Python: _process_charset_selection
void Vt102Emulation::processCharsetSelection(int ch, int param)
{
    // ch 是 designate 字符 ((, ), *, +),param 是字符集名 (B in TY_CONSTRUCT(3,A,B))
    const char cs = static_cast<char>(param & 0xff);
    switch (ch) {
    case '(': setCharset(0, cs); break; // G0
    case ')': setCharset(1, cs); break; // G1
    case '*': setCharset(2, cs); break; // G2
    case '+': setCharset(3, cs); break; // G3
    default: break;
    }
}

// 对应 Python: _process_dec_sequence
void Vt102Emulation::processDecSequence(int ch)
{
    if (ch == '8') {
        _currentScreen->helpAlign(); // DECALN
    } else if (ch == '3' || ch == '4' || ch == '5' || ch == '6') {
        // double-height / double-width lines
        const bool doubleWidth  = (ch != '5');
        const bool doubleHeight = (ch == '3' || ch == '4');
        _currentScreen->setLineProperty(LINE_DOUBLEWIDTH, doubleWidth);
        _currentScreen->setLineProperty(LINE_DOUBLEHEIGHT, doubleHeight);
    }
}

// 对应 Python: _process_csi_ps
void Vt102Emulation::processCsiPs(int ch, int param, int p, int q)
{
    switch (ch) {
    case 'm': // SGR
        processSgr(param, p, q);
        break;
    case 't': // Window manipulation
        if (param == 8) {
            setImageSize(p, q);
            Q_EMIT imageResizeRequest(QSize(q, p));
        } else if (param == 28) {
            Q_EMIT changeTabTextColorRequest(p);
        }
        break;
    case 'K': // EL
        if (param == 0)      _currentScreen->clearToEndOfLine();
        else if (param == 1) _currentScreen->clearToBeginOfLine();
        else if (param == 2) _currentScreen->clearEntireLine();
        break;
    case 'J': // ED
        if (param == 0)      _currentScreen->clearToEndOfScreen();
        else if (param == 1) _currentScreen->clearToBeginOfScreen();
        else if (param == 2) _currentScreen->clearEntireScreen();
        else if (param == 3) clearHistory();
        break;
    case 'g': // TBC
        if (param == 0)      _currentScreen->changeTabStop(false);
        else if (param == 3) _currentScreen->clearTabStops();
        break;
    case 'h': // SM
        if (param == 4)      _currentScreen->setMode(MODE_Insert);
        else if (param == 20) setMode(MODE_NewLine);
        break;
    case 'l': // RM
        if (param == 4)      _currentScreen->resetMode(MODE_Insert);
        else if (param == 20) resetMode(MODE_NewLine);
        break;
    case 's': saveCursor(); break;    // SCP
    case 'u': restoreCursor(); break; // RCP
    case 'n': // DSR
        if (param == 5)      reportStatus();
        else if (param == 6) reportCursorPosition();
        break;
    case 'x': // DECREQTPARM
        if (param == 0 || param == 1)
            reportTerminalParms(param + 2);
        break;
    default: break;
    }
}

// 对应 Python: _process_sgr
void Vt102Emulation::processSgr(int param, int p, int q)
{
    if (param == 0) {
        _currentScreen->setDefaultRendition();
    } else if (param == 1) {
        _currentScreen->setRendition(RE_BOLD);
    } else if (param == 2) {
        _currentScreen->setRendition(RE_FAINT);
    } else if (param == 3) {
        _currentScreen->setRendition(RE_ITALIC);
    } else if (param == 4) {
        _currentScreen->setRendition(RE_UNDERLINE);
    } else if (param == 5) {
        _currentScreen->setRendition(RE_BLINK);
    } else if (param == 6) {
        _currentScreen->setRendition(RE_BLINK);
    } else if (param == 7) {
        _currentScreen->setRendition(RE_REVERSE);
    } else if (param == 8) {
        _currentScreen->setRendition(RE_CONCEAL);
    } else if (param == 9) {
        _currentScreen->setRendition(RE_STRIKEOUT);
    } else if (param == 53) {
        _currentScreen->setRendition(RE_OVERLINE);
    // Reset
    } else if (param == 21) {
        _currentScreen->resetRendition(RE_BOLD);
    } else if (param == 22) {
        _currentScreen->resetRendition(RE_BOLD);
        _currentScreen->resetRendition(RE_FAINT);
    } else if (param == 23) {
        _currentScreen->resetRendition(RE_ITALIC);
    } else if (param == 24) {
        _currentScreen->resetRendition(RE_UNDERLINE);
    } else if (param == 25) {
        _currentScreen->resetRendition(RE_BLINK);
    } else if (param == 27) {
        _currentScreen->resetRendition(RE_REVERSE);
    } else if (param == 28) {
        _currentScreen->resetRendition(RE_CONCEAL);
    } else if (param == 29) {
        _currentScreen->resetRendition(RE_STRIKEOUT);
    } else if (param == 55) {
        _currentScreen->resetRendition(RE_OVERLINE);
    // Colors
    } else if (param >= 30 && param <= 37) {
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, param - 30);
    } else if (param == 38) {
        _currentScreen->setForeColor(p, q); // Extended foreground
    } else if (param == 39) {
        _currentScreen->setForeColor(COLOR_SPACE_DEFAULT, 0);
    } else if (param >= 40 && param <= 47) {
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, param - 40);
    } else if (param == 48) {
        _currentScreen->setBackColor(p, q); // Extended background
    } else if (param == 49) {
        _currentScreen->setBackColor(COLOR_SPACE_DEFAULT, 1);
    // Bright colors
    } else if (param >= 90 && param <= 97) {
        _currentScreen->setForeColor(COLOR_SPACE_SYSTEM, param - 90 + 8);
    } else if (param >= 100 && param <= 107) {
        _currentScreen->setBackColor(COLOR_SPACE_SYSTEM, param - 100 + 8);
    }
}

// 对应 Python: _process_csi_pn
void Vt102Emulation::processCsiPn(int ch, int p, int q)
{
    switch (ch) {
    case '@': _currentScreen->insertChars(p); break;
    case 'A': _currentScreen->cursorUp(p); break;
    case 'B': _currentScreen->cursorDown(p); break;
    case 'C': _currentScreen->cursorRight(p); break;
    case 'D': _currentScreen->cursorLeft(p); break;
    case 'E': _currentScreen->cursorNextLine(p); break;
    case 'F': _currentScreen->cursorPreviousLine(p); break;
    case 'G': _currentScreen->setCursorX(p); break;
    case 'H':
    case 'f': _currentScreen->setCursorYX(p, q); break;
    case 'I': _currentScreen->tab(p); break;
    case 'L': _currentScreen->insertLines(p); break;
    case 'M': _currentScreen->deleteLines(p); break;
    case 'P': _currentScreen->deleteChars(p); break;
    case 'S': _currentScreen->scrollUpRegion(p); break;
    case 'T': _currentScreen->scrollDownRegion(p); break;
    case 'X': _currentScreen->eraseChars(p); break;
    case 'Z': _currentScreen->backtab(p); break;
    case 'b': _currentScreen->repeatChars(p); break;
    case 'c': reportTerminalType(); break;
    case 'd': _currentScreen->setCursorY(p); break;
    case 'r': setMargins(p, q); break;
    default: break;
    }
}

// 对应 Python: _process_csi_pr
void Vt102Emulation::processCsiPr(int ch, int param)
{
    if (ch == 'h')      processPrivateMode(param, true);  // DECSET
    else if (ch == 'l') processPrivateMode(param, false); // DECRST
}

// 对应 Python: _process_private_mode
void Vt102Emulation::processPrivateMode(int modeNum, bool enable)
{
    int targetMode = -1;

    switch (modeNum) {
    case 1:    targetMode = MODE_AppCuKeys; break;
    case 25:   targetMode = MODE_Cursor; break;
    case 47:   targetMode = MODE_AppScreen; break;
    case 1000: targetMode = MODE_Mouse1000; break;
    case 1002: targetMode = MODE_Mouse1002; break;
    case 1005: targetMode = MODE_Mouse1005; break;
    case 1006: targetMode = MODE_Mouse1006; break;
    case 1015: targetMode = MODE_Mouse1015; break;
    case 1047: targetMode = MODE_AppScreen; break;
    case 2004: targetMode = MODE_BracketedPaste; break;
    default: break;
    }

    // 焦点上报 (?1004):不影响渲染状态机,仅通知显示层主屏内有交互式 TUI 在运行
    if (modeNum == 1004) {
        Q_EMIT programReportFocusChanged(enable);
        return;
    }

    // 特殊处理
    if (modeNum == 1048) {
        if (enable) saveCursor();
        else        restoreCursor();
        return;
    }
    if (modeNum == 1049) {
        if (enable) {
            saveCursor();
            setMode(MODE_AppScreen);
            clearEntireScreen();
        } else {
            resetMode(MODE_AppScreen);
            restoreCursor();
        }
        return;
    }

    if (targetMode != -1) {
        if (enable) setMode(targetMode);
        else        resetMode(targetMode);
    }
}

// 对应 Python: _process_csi_ps_sp
void Vt102Emulation::processCsiPsSp(int ch, int param)
{
    if (ch != 'q')
        return;

    // DECSCUSR (Set Cursor Style)
    // 0,1: Blinking Block  2: Steady Block  3: Blinking Underline
    // 4: Steady Underline  5: Blinking Bar   6: Steady Bar
    int shape = 0; // Block
    bool blinking = true;

    switch (param) {
    case 0:
    case 1: break;
    case 2: blinking = false; break;
    case 3: shape = 1; break;
    case 4: shape = 1; blinking = false; break;
    case 5: shape = 2; break;
    case 6: shape = 2; blinking = false; break;
    default: break;
    }

    Q_EMIT cursorChanged(static_cast<KeyboardCursorShape>(shape), blinking);
}

// 对应 Python: _process_vt52
void Vt102Emulation::processVt52(int ch, int p, int q)
{
    switch (ch) {
    case 'A': _currentScreen->cursorUp(1); break;
    case 'B': _currentScreen->cursorDown(1); break;
    case 'C': _currentScreen->cursorRight(1); break;
    case 'D': _currentScreen->cursorLeft(1); break;
    case 'F': setAndUseCharset(0, '0'); break; // Graphics
    case 'G': setAndUseCharset(0, 'B'); break; // ASCII
    case 'H': _currentScreen->setCursorYX(1, 1); break;
    case 'I': _currentScreen->reverseIndex(); break;
    case 'J': _currentScreen->clearToEndOfScreen(); break;
    case 'K': _currentScreen->clearToEndOfLine(); break;
    case 'Y': _currentScreen->setCursorYX(p - 31, q - 31); break;
    case 'Z': reportTerminalType(); break;
    case '<': setMode(MODE_Ansi); break;
    case '=': setMode(MODE_AppKeyPad); break;
    case '>': resetMode(MODE_AppKeyPad); break;
    default: break;
    }
}

} // namespace Konsole
