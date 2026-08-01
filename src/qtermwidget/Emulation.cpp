// Emulation.cpp — C++ port of qtermwidget/emulation.py
//
// Abstract base class for terminal emulation back-ends. Ported from the
// Python PySide6 version (itself converted from Konsole / QTermWidget,
// upstream Emulation.cpp).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
//   Copyright 1996 by Matthias Ettrich <ettrich@kde.org>

#include "Emulation.h"

#include <QKeyEvent>

// 并行移植的兄弟模块,按契约名包含头文件。
#include "Screen.h"                 // Screen
#include "ScreenWindow.h"           // ScreenWindow
#include "TerminalCharacterDecoder.h"

namespace Konsole {

// Bulk-update timeouts (ms).
// 对应C++: #define BULK_TIMEOUT1 10 / #define BULK_TIMEOUT2 40
inline constexpr int BULK_TIMEOUT1 = 10;
inline constexpr int BULK_TIMEOUT2 = 40;

Emulation::Emulation(QObject *parent)
    : QObject(parent)
    , _currentScreen(nullptr)
    , _keyTranslator(nullptr)
    , _usesMouse(false)
    , _bracketedPasteMode(false)
    , _bulkTimer1(this)
    , _bulkTimer2(this)
    , _toUtf16(QStringDecoder::Utf8)
{
    // Create screens with a default size.
    // 对应C++: _screen[0] = new Screen(40,80); _screen[1] = new Screen(40,80);
    _screen[0] = new Screen(40, 80);
    _screen[1] = new Screen(40, 80);
    _currentScreen = _screen[0];

    // 对应C++: connect(&_bulkTimer1, &QTimer::timeout, this, &Emulation::showBulk);
    QObject::connect(&_bulkTimer1, &QTimer::timeout, this, &Emulation::showBulk);
    QObject::connect(&_bulkTimer2, &QTimer::timeout, this, &Emulation::showBulk);

    // 对应C++: connect(this, &Emulation::programUsesMouseChanged, this, &Emulation::usesMouseChanged);
    QObject::connect(this, &Emulation::programUsesMouseChanged,
                     this, &Emulation::usesMouseChanged);
    QObject::connect(this, &Emulation::programBracketedPasteModeChanged,
                     this, &Emulation::bracketedPasteModeChanged);

    // cube-shell: 把光标形状变化通过 titleChanged(id=50) 通道转发给显示层。
    QObject::connect(this, &Emulation::cursorChanged,
                     this, &Emulation::_onCursorChanged);
}

Emulation::~Emulation()
{
    // 从全局扩展字符表中移除本仿真的所有窗口。
    // 对应C++: ExtendedCharTable::instance.windows.remove(...)/delete windows
    for (ScreenWindow *window : std::as_const(_windows)) {
        ExtendedCharTable::instance.windows.remove(window);
        delete window;
    }

    // 对应C++: delete _screen[0]; delete _screen[1];
    delete _screen[0];
    delete _screen[1];
}

ScreenWindow *Emulation::createWindow()
{
    // 对应C++: ScreenWindow* window = new ScreenWindow();
    auto *window = new ScreenWindow();
    window->setScreen(_currentScreen);
    _windows << window;
    ExtendedCharTable::instance.windows.insert(window);

    // 对应C++: connect(window, &ScreenWindow::selectionChanged, this, &Emulation::bufferedUpdate);
    QObject::connect(window, &ScreenWindow::selectionChanged,
                     this, &Emulation::bufferedUpdate);
    QObject::connect(this, &Emulation::outputChanged,
                     window, &ScreenWindow::notifyOutputChanged);
    QObject::connect(this, &Emulation::handleCommandFromKeyboard,
                     window, &ScreenWindow::handleCommandFromKeyboard);
    QObject::connect(this, &Emulation::outputFromKeypressEvent,
                     window, &ScreenWindow::scrollToEnd);

    return window;
}

bool Emulation::programUsesMouse() const
{
    // 对应C++: bool programUsesMouse() const { return _usesMouse; }
    return _usesMouse;
}

void Emulation::usesMouseChanged(bool usesMouse)
{
    // 对应C++: void usesMouseChanged(bool usesMouse) { _usesMouse = usesMouse; }
    _usesMouse = usesMouse;
}

bool Emulation::programBracketedPasteMode() const
{
    // 对应C++: bool programBracketedPasteMode() const { return _bracketedPasteMode; }
    return _bracketedPasteMode;
}

void Emulation::bracketedPasteModeChanged(bool bracketedPasteMode)
{
    // 对应C++: void bracketedPasteModeChanged(bool bracketedPasteMode)
    _bracketedPasteMode = bracketedPasteMode;
}

void Emulation::setScreen(int n)
{
    // 对应C++: void setScreen(int n)
    Screen *old = _currentScreen;
    _currentScreen = _screen[n & 1];
    if (_currentScreen != old) {
        // Tell all windows to switch to the newly active screen.
        for (ScreenWindow *window : std::as_const(_windows))
            window->setScreen(_currentScreen);
    }
}

void Emulation::clearHistory()
{
    // 对应C++: _screen[0]->setScroll(_screen[0]->getScroll(), false);
    _screen[0]->setScroll(_screen[0]->getScroll(), false);
}

void Emulation::setHistory(const HistoryType &t)
{
    // 对应C++: _screen[0]->setScroll(t); showBulk();
    _screen[0]->setScroll(t);
    showBulk();
}

const HistoryType &Emulation::history() const
{
    // 对应C++: const HistoryType& history() const { return _screen[0]->getScroll(); }
    return _screen[0]->getScroll();
}

void Emulation::setKeyBindings(const QString &name)
{
    // 对应C++: _keyTranslator = KeyboardTranslatorManager::instance()->findTranslator(name);
    KeyboardTranslatorManager *manager = KeyboardTranslatorManager::instance();
    _keyTranslator = manager->findTranslator(name);
    if (_keyTranslator == nullptr)
        _keyTranslator = manager->defaultTranslator();
}

QString Emulation::keyBindings() const
{
    // 对应C++: return _keyTranslator->name();
    return _keyTranslator != nullptr ? _keyTranslator->name() : QString();
}

void Emulation::receiveChar(int ch)
{
    // 对应C++: void receiveChar(wchar_t c)
    ch &= 0xff;

    switch (ch) {
    case '\b':
        _currentScreen->backspace();
        break;
    case '\t':
        _currentScreen->tab();
        break;
    case '\n':
        _currentScreen->newLine();
        break;
    case '\r':
        _currentScreen->toStartOfLine();
        break;
    case 0x07:
        Q_EMIT stateSet(NOTIFYBELL);
        break;
    default:
        _currentScreen->displayCharacter(static_cast<ushort>(ch));
        break;
    }
}

void Emulation::sendKeyEvent(QKeyEvent *ev, bool fromPaste)
{
    // 对应C++: void sendKeyEvent(QKeyEvent* ev, bool fromPaste)
    Q_UNUSED(fromPaste);
    Q_EMIT stateSet(NOTIFYNORMAL);

    if (ev->text().isEmpty())
        return;

    QByteArray text = ev->text().toUtf8();
    Q_EMIT sendData(text.constData(), static_cast<int>(text.size()));
}

void Emulation::sendMouseEvent(int /*buttons*/, int /*column*/, int /*row*/, int /*eventType*/)
{
    // Default implementation does nothing; subclasses override.
    // 对应C++: virtual void sendMouseEvent(...) {}
}

void Emulation::receiveData(const char *text, int length)
{
    // 对应C++: void receiveData(const char* text, int length)
    Q_EMIT stateSet(NOTIFYACTIVITY);

    bufferedUpdate();

    // Decode the bytes to UTF-16 using the current codec, then feed each
    // character to receiveChar(). 对应 Python 的增量 UTF-8 解码循环。
    const QByteArray ba(text, length);
    const QString unicode = _toUtf16(ba);
    for (const QChar c : unicode)
        receiveChar(c.unicode());

    // Look for z-modem indicator: \030 followed by "B00".
    // 对应C++: for (int i=0;i<len;i++) if (text[i] == '\030') ...
    for (int i = 0; i < length; ++i) {
        if (static_cast<uchar>(text[i]) == 0x18) { // '\030'
            if (length - i - 1 > 3 && strncmp(text + i + 1, "B00", 3) == 0)
                Q_EMIT zmodemDetected();
        }
    }
}

void Emulation::writeToStream(TerminalCharacterDecoder *decoder, int startLine, int endLine)
{
    // 对应C++: _currentScreen->writeLinesToStream(decoder, startLine, endLine);
    _currentScreen->writeLinesToStream(decoder, startLine, endLine);
}

int Emulation::lineCount() const
{
    // 对应C++: return _currentScreen->getLines() + _currentScreen->getHistLines();
    return _currentScreen->getLines() + _currentScreen->getHistLines();
}

void Emulation::showBulk()
{
    // 对应C++: _bulkTimer1.stop(); _bulkTimer2.stop(); emit outputChanged(); ...
    _bulkTimer1.stop();
    _bulkTimer2.stop();

    Q_EMIT outputChanged();

    _currentScreen->resetScrolledLines();
    _currentScreen->resetDroppedLines();
}

void Emulation::bufferedUpdate()
{
    // 对应C++: _bulkTimer1.setSingleShot(true); _bulkTimer1.start(BULK_TIMEOUT1); ...
    _bulkTimer1.setSingleShot(true);
    _bulkTimer1.start(BULK_TIMEOUT1);

    if (!_bulkTimer2.isActive()) {
        _bulkTimer2.setSingleShot(true);
        _bulkTimer2.start(BULK_TIMEOUT2);
    }
}

char Emulation::eraseChar() const
{
    // 对应C++: virtual char eraseChar() const { return '\b'; }
    return '\b';
}

void Emulation::setCodec(EmulationCodec codec)
{
    // 对应C++: void setCodec(EmulationCodec codec)
    // Python 版把这个丢掉了(硬编码 UTF-8)。此处恢复上游语义:
    // LocaleCodec 用系统本地编码,Utf8Codec 用 UTF-8,均带替换错误处理。
    if (codec == Utf8Codec)
        _toUtf16 = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::ConvertInvalidToNull);
    else
        _toUtf16 = QStringDecoder(QStringDecoder::System, QStringDecoder::Flag::ConvertInvalidToNull);
}

void Emulation::setImageSize(int lines, int columns)
{
    // 对应C++: void setImageSize(int lines, int columns)
    if (lines < 1 || columns < 1)
        return;

    const QSize screenSize[2] = {
        QSize(_screen[0]->getColumns(), _screen[0]->getLines()),
        QSize(_screen[1]->getColumns(), _screen[1]->getLines())
    };
    const QSize newSize(columns, lines);

    if (newSize == screenSize[0] && newSize == screenSize[1])
        return;

    // 主屏启用 reflow（窗口变宽/变窄时重排文本，不丢内容）；
    // 备屏不重排（vim 等全屏应用收到 SIGWINCH 后自行重绘）。
    _screen[0]->resizeImage(lines, columns, true);
    _screen[1]->resizeImage(lines, columns, false);

    Q_EMIT imageSizeChanged(lines, columns);

    bufferedUpdate();
}

QSize Emulation::imageSize() const
{
    // 对应C++: return QSize(_currentScreen->getColumns(), _currentScreen->getLines());
    return QSize(_currentScreen->getColumns(), _currentScreen->getLines());
}

void Emulation::_onCursorChanged(KeyboardCursorShape cursorShape, bool blinkingEnabled)
{
    // cube-shell 扩展: 把光标形状/闪烁信息打包成 "CursorShape=N;BlinkingCursorEnabled=B"
    // 通过 titleChanged(id=50) 通道发出,Session 再转成 profileChangeCommandReceived。
    const int shapeValue = static_cast<int>(cursorShape);
    const QString titleText =
        QStringLiteral("CursorShape=%1;BlinkingCursorEnabled=%2")
            .arg(shapeValue)
            .arg(blinkingEnabled);
    Q_EMIT titleChanged(50, titleText);
}

} // namespace Konsole
