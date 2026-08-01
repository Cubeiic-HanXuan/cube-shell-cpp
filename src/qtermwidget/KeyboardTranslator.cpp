// KeyboardTranslator.cpp — C++ port of qtermwidget/keyboard_translator.py
//
// Keyboard translation tables. Ported from the Python PySide6 version, which
// was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include "KeyboardTranslator.h"

#include <QFile>
#include <QFileInfo>
#include <QKeySequence>
#include <QRegularExpression>

#include "tools.h"

namespace Konsole {

// Helper: bool -> int.
// 对应C++: inline int oneOrZero(int value) { return value ? 1 : 0; }
static inline int oneOrZero(bool value) { return value ? 1 : 0; }

// ---------------------------------------------------------------------------
// KeyboardTranslatorEntry
// ---------------------------------------------------------------------------

KeyboardTranslatorEntry::KeyboardTranslatorEntry()
    : _keyCode(0)
    , _modifiers(Qt::NoModifier)
    , _modifierMask(Qt::NoModifier)
    , _state(NoState)
    , _stateMask(NoState)
    , _command(NoCommand)
    , _text()
{
}

// 对应C++: bool Entry::isNull() const
bool KeyboardTranslatorEntry::isNull() const
{
    return *this == KeyboardTranslatorEntry();
}

KeyboardTranslatorCommand KeyboardTranslatorEntry::command() const { return _command; }
void KeyboardTranslatorEntry::setCommand(KeyboardTranslatorCommand command) { _command = command; }

// 对应C++: QByteArray Entry::text(bool expandWildCards, Qt::KeyboardModifiers modifiers) const
QByteArray KeyboardTranslatorEntry::text(bool expandWildCards,
                                         Qt::KeyboardModifiers modifiers) const
{
    QByteArray expandedText = _text;

    if (expandWildCards) {
        int modifierValue = 1;
        modifierValue += oneOrZero(modifiers & Qt::ShiftModifier);
        modifierValue += oneOrZero(modifiers & Qt::AltModifier) << 1;
        modifierValue += oneOrZero(modifiers & CTRL_MOD) << 2;

        for (int i = 0; i < expandedText.size(); ++i) {
            if (expandedText[i] == '*')
                expandedText[i] = static_cast<char>('0' + modifierValue);
        }
    }

    return expandedText;
}

// 对应C++: void Entry::setText(const QByteArray& text)
void KeyboardTranslatorEntry::setText(const QByteArray &text)
{
    _text = unescape(text);
}

int KeyboardTranslatorEntry::keyCode() const { return _keyCode; }
void KeyboardTranslatorEntry::setKeyCode(int keyCode) { _keyCode = keyCode; }

Qt::KeyboardModifiers KeyboardTranslatorEntry::modifiers() const { return _modifiers; }
Qt::KeyboardModifiers KeyboardTranslatorEntry::modifierMask() const { return _modifierMask; }
void KeyboardTranslatorEntry::setModifiers(Qt::KeyboardModifiers modifiers) { _modifiers = modifiers; }
void KeyboardTranslatorEntry::setModifierMask(Qt::KeyboardModifiers mask) { _modifierMask = mask; }

KeyboardTranslatorStates KeyboardTranslatorEntry::state() const { return _state; }
KeyboardTranslatorStates KeyboardTranslatorEntry::stateMask() const { return _stateMask; }
void KeyboardTranslatorEntry::setState(KeyboardTranslatorStates state) { _state = state; }
void KeyboardTranslatorEntry::setStateMask(KeyboardTranslatorStates mask) { _stateMask = mask; }

// 对应C++: bool Entry::matches(int keyCode, Qt::KeyboardModifiers modifiers, States testState) const
bool KeyboardTranslatorEntry::matches(int keyCode,
                                      Qt::KeyboardModifiers modifiers,
                                      KeyboardTranslatorStates testState) const
{
#if defined(Q_OS_MAC)
    // On Mac, arrow keys are considered part of the keypad; ignore this.
    modifiers &= ~Qt::KeypadModifier;
#endif

    if (_keyCode != keyCode)
        return false;

    if ((modifiers & _modifierMask) != (_modifiers & _modifierMask))
        return false;

    // If modifiers are non-zero, the "any modifier" state is implied.
    if ((modifiers & ~Qt::KeypadModifier) != Qt::NoModifier)
        testState |= AnyModifierState;

    if ((testState & _stateMask) != (_state & _stateMask))
        return false;

    // Special handling for the "any modifier" state.
    const bool anyModifiersSet =
        (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier);
    const bool wantAnyModifier = (_state & AnyModifierState);

    if (_stateMask & AnyModifierState) {
        if (wantAnyModifier != anyModifiersSet)
            return false;
    }

    return true;
}

// 对应C++: QByteArray Entry::escapedText(bool expandWildCards, Qt::KeyboardModifiers modifiers) const
QByteArray KeyboardTranslatorEntry::escapedText(bool expandWildCards,
                                                Qt::KeyboardModifiers modifiers) const
{
    QByteArray result = text(expandWildCards, modifiers);

    for (int i = 0; i < result.size();) {
        const char ch = result[i];
        QByteArray replacement;

        switch (ch) {
        case 27: replacement = "\\E"; break;
        case 8:  replacement = "\\b"; break;
        case 12: replacement = "\\f"; break;
        case 9:  replacement = "\\t"; break;
        case 13: replacement = "\\r"; break;
        case 10: replacement = "\\n"; break;
        default:
            if (ch <= 0 || !QChar::isPrint(static_cast<uchar>(ch)))
                replacement = QByteArray("\\x") + QByteArray::number(static_cast<uchar>(ch), 16).rightJustified(2, '0');
            break;
        }

        if (!replacement.isEmpty()) {
            result.replace(i, 1, replacement);
            i += replacement.size();
        } else {
            ++i;
        }
    }

    return result;
}

// 对应C++: QByteArray Entry::unescape(const QByteArray& input) const
QByteArray KeyboardTranslatorEntry::unescape(const QByteArray &input) const
{
    QByteArray result = input;

    for (int i = 0; i < result.size() - 1;) {
        if (result[i] != '\\') {
            ++i;
            continue;
        }

        QByteArray replacement;
        int charsToRemove = 2;
        bool escapedChar = true;

        const char next = result[i + 1];
        switch (next) {
        case 'E': replacement = QByteArray(1, 27); break;
        case 'b': replacement = QByteArray(1, 8);  break;
        case 'f': replacement = QByteArray(1, 12); break;
        case 't': replacement = QByteArray(1, 9);  break;
        case 'r': replacement = QByteArray(1, 13); break;
        case 'n': replacement = QByteArray(1, 10); break;
        case 'x': {
            // Hex escape sequence \xhh (1 or 2 hex digits).
            QByteArray hexDigits;
            const auto isHex = [](char c) {
                return (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            };
            if (i + 2 < result.size() && isHex(result[i + 2]))
                hexDigits += result[i + 2];
            if (i + 3 < result.size() && isHex(result[i + 3]))
                hexDigits += result[i + 3];

            if (!hexDigits.isEmpty()) {
                bool ok = false;
                const int value = hexDigits.toInt(&ok, 16);
                if (ok) {
                    replacement = QByteArray(1, static_cast<char>(value));
                    charsToRemove = 2 + hexDigits.size();
                } else {
                    escapedChar = false;
                }
            } else {
                escapedChar = false;
            }
            break;
        }
        default:
            escapedChar = false;
            break;
        }

        if (escapedChar && !replacement.isEmpty()) {
            result.replace(i, charsToRemove, replacement);
            i += replacement.size();
        } else {
            ++i;
        }
    }

    return result;
}

// 对应C++: void Entry::insertModifier(QString& item, int modifier) const
void KeyboardTranslatorEntry::insertModifier(QString &item, int modifier) const
{
    const Qt::KeyboardModifier mod = static_cast<Qt::KeyboardModifier>(modifier);
    if (!(mod & _modifierMask))
        return;

    item += (mod & _modifiers) ? QLatin1Char('+') : QLatin1Char('-');

    if (mod == Qt::ShiftModifier)
        item += QLatin1String("Shift");
    else if (mod == Qt::ControlModifier)
        item += QLatin1String("Ctrl");
    else if (mod == Qt::AltModifier)
        item += QLatin1String("Alt");
    else if (mod == Qt::MetaModifier)
        item += QLatin1String("Meta");
    else if (mod == Qt::KeypadModifier)
        item += QLatin1String("KeyPad");
}

// 对应C++: void Entry::insertState(QString& item, int state) const
void KeyboardTranslatorEntry::insertState(QString &item, int state) const
{
    if (!(state & _stateMask))
        return;

    item += (state & _state) ? QLatin1Char('+') : QLatin1Char('-');

    if (state == AlternateScreenState)
        item += QLatin1String("AppScreen");
    else if (state == NewLineState)
        item += QLatin1String("NewLine");
    else if (state == AnsiState)
        item += QLatin1String("Ansi");
    else if (state == CursorKeysState)
        item += QLatin1String("AppCursorKeys");
    else if (state == AnyModifierState)
        item += QLatin1String("AnyModifier");
    else if (state == ApplicationKeypadState)
        item += QLatin1String("AppKeypad");
}

// 对应C++: QString Entry::conditionToString() const
QString KeyboardTranslatorEntry::conditionToString() const
{
    QString result = QKeySequence(_keyCode).toString();

    insertModifier(result, Qt::ShiftModifier);
    insertModifier(result, Qt::ControlModifier);
    insertModifier(result, Qt::AltModifier);
    insertModifier(result, Qt::MetaModifier);
    insertModifier(result, Qt::KeypadModifier);

    insertState(result, AlternateScreenState);
    insertState(result, NewLineState);
    insertState(result, AnsiState);
    insertState(result, CursorKeysState);
    insertState(result, AnyModifierState);
    insertState(result, ApplicationKeypadState);

    return result;
}

// 对应C++: QString Entry::resultToString(bool expandWildCards, Qt::KeyboardModifiers modifiers) const
QString KeyboardTranslatorEntry::resultToString(bool expandWildCards,
                                                Qt::KeyboardModifiers modifiers) const
{
    if (!_text.isEmpty())
        return QString::fromLatin1(escapedText(expandWildCards, modifiers));

    switch (_command) {
    case EraseCommand:              return QStringLiteral("Erase");
    case ScrollPageUpCommand:       return QStringLiteral("ScrollPageUp");
    case ScrollPageDownCommand:     return QStringLiteral("ScrollPageDown");
    case ScrollLineUpCommand:       return QStringLiteral("ScrollLineUp");
    case ScrollLineDownCommand:     return QStringLiteral("ScrollLineDown");
    case ScrollLockCommand:         return QStringLiteral("ScrollLock");
    case ScrollUpToTopCommand:      return QStringLiteral("ScrollUpToTop");
    case ScrollDownToBottomCommand: return QStringLiteral("ScrollDownToBottom");
    default:                        return QString();
    }
}

// 对应C++: bool Entry::operator==(const Entry& rhs) const
bool KeyboardTranslatorEntry::operator==(const KeyboardTranslatorEntry &rhs) const
{
    return _keyCode == rhs._keyCode &&
           _modifiers == rhs._modifiers &&
           _modifierMask == rhs._modifierMask &&
           _state == rhs._state &&
           _stateMask == rhs._stateMask &&
           _command == rhs._command &&
           _text == rhs._text;
}

// ---------------------------------------------------------------------------
// KeyboardTranslator
// ---------------------------------------------------------------------------

KeyboardTranslator::KeyboardTranslator(const QString &name)
    : _name(name)
{
}

QString KeyboardTranslator::name() const { return _name; }
void KeyboardTranslator::setName(const QString &name) { _name = name; }
QString KeyboardTranslator::description() const { return _description; }
void KeyboardTranslator::setDescription(const QString &description) { _description = description; }

// 对应C++: Entry KeyboardTranslator::findEntry(int keyCode, Qt::KeyboardModifiers modifiers, States state) const
KeyboardTranslatorEntry KeyboardTranslator::findEntry(int keyCode,
                                                      Qt::KeyboardModifiers modifiers,
                                                      KeyboardTranslatorStates state) const
{
    const auto it = _entries.constFind(keyCode);
    if (it != _entries.constEnd()) {
        for (const KeyboardTranslatorEntry &entry : it.value()) {
            if (entry.matches(keyCode, modifiers, state))
                return entry;
        }
    }
    return KeyboardTranslatorEntry();
}

// 对应C++: void KeyboardTranslator::addEntry(const Entry& entry)
void KeyboardTranslator::addEntry(const KeyboardTranslatorEntry &entry)
{
    _entries[entry.keyCode()].append(entry);
}

// 对应C++: void KeyboardTranslator::replaceEntry(const Entry& existing, const Entry& replacement)
void KeyboardTranslator::replaceEntry(const KeyboardTranslatorEntry &existing,
                                      const KeyboardTranslatorEntry &replacement)
{
    if (!existing.isNull())
        removeEntry(existing);
    addEntry(replacement);
}

// 对应C++: void KeyboardTranslator::removeEntry(const Entry& entry)
void KeyboardTranslator::removeEntry(const KeyboardTranslatorEntry &entry)
{
    auto it = _entries.find(entry.keyCode());
    if (it == _entries.end())
        return;

    it.value().removeAll(entry);
    if (it.value().isEmpty())
        _entries.erase(it);
}

// 对应C++: QList<Entry> KeyboardTranslator::entries() const
QList<KeyboardTranslatorEntry> KeyboardTranslator::entries() const
{
    QList<KeyboardTranslatorEntry> result;
    for (const auto &list : _entries)
        result.append(list);
    return result;
}

// ---------------------------------------------------------------------------
// KeyboardTranslatorReader
// ---------------------------------------------------------------------------

KeyboardTranslatorReader::KeyboardTranslatorReader(QIODevice *source)
    : _device(source)
    , _lineIndex(0)
    , _hasNext(false)
{
    // Read description.
    while (_description.isEmpty() && _device && !_device->atEnd()) {
        const QList<Token> tokens = tokenize(QString::fromUtf8(_device->readLine()));
        if (!tokens.isEmpty() && tokens.first().type == Token::TitleKeyword) {
            if (tokens.size() > 1)
                _description = tokens.at(1).text;
        }
    }
    readNext();
}

KeyboardTranslatorReader::KeyboardTranslatorReader(const QString &source)
    : _device(nullptr)
    , _lines(source.split(QLatin1Char('\n')))
    , _lineIndex(0)
    , _hasNext(false)
{
    while (_description.isEmpty() && _lineIndex < _lines.size()) {
        const QList<Token> tokens = tokenize(_lines.at(_lineIndex++));
        if (!tokens.isEmpty() && tokens.first().type == Token::TitleKeyword) {
            if (tokens.size() > 1)
                _description = tokens.at(1).text;
        }
    }
    readNext();
}

QString KeyboardTranslatorReader::description() const { return _description; }
bool KeyboardTranslatorReader::hasNextEntry() const { return _hasNext; }

// 对应C++: Entry KeyboardTranslatorReader::nextEntry()
KeyboardTranslatorEntry KeyboardTranslatorReader::nextEntry()
{
    if (!_hasNext)
        return KeyboardTranslatorEntry();

    const KeyboardTranslatorEntry entry = _nextEntry;
    readNext();
    return entry;
}

bool KeyboardTranslatorReader::parseError() const
{
    // TODO: implement real error detection (matches the Python port).
    return false;
}

// 对应C++: void KeyboardTranslatorReader::readNext()
void KeyboardTranslatorReader::readNext()
{
    auto nextLine = [this](QString &out) -> bool {
        if (_device) {
            if (_device->atEnd())
                return false;
            out = QString::fromUtf8(_device->readLine());
            return true;
        }
        if (_lineIndex < _lines.size()) {
            out = _lines.at(_lineIndex++);
            return true;
        }
        return false;
    };

    QString line;
    while (nextLine(line)) {
        const QList<Token> tokens = tokenize(line);
        if (tokens.isEmpty() || tokens.first().type != Token::KeyKeyword)
            continue;
        if (tokens.size() < 3)
            continue;

        KeyboardTranslatorStates flags = NoState;
        KeyboardTranslatorStates flagMask = NoState;
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        Qt::KeyboardModifiers modifierMask = Qt::NoModifier;
        int keyCode = Qt::Key_unknown;

        if (!decodeSequence(tokens.at(1).text.toLower(), keyCode, modifiers,
                            modifierMask, flags, flagMask))
            continue;

        KeyboardTranslatorCommand command = NoCommand;
        QByteArray text;

        const Token &outputToken = tokens.at(2);
        if (outputToken.type == Token::OutputText)
            text = outputToken.text.toLatin1();
        else if (outputToken.type == Token::Command)
            command = parseAsCommand(outputToken.text);

        KeyboardTranslatorEntry newEntry;
        newEntry.setKeyCode(keyCode);
        newEntry.setState(flags);
        newEntry.setStateMask(flagMask);
        newEntry.setModifiers(modifiers);
        newEntry.setModifierMask(modifierMask);
        newEntry.setText(text);
        newEntry.setCommand(command);

        _nextEntry = newEntry;
        _hasNext = true;
        return;
    }

    _hasNext = false;
}

// 对应C++: QList<Token> KeyboardTranslatorReader::tokenize(const QString& line)
QList<KeyboardTranslatorReader::Token> KeyboardTranslatorReader::tokenize(const QString &line) const
{
    QString text = line.simplified();

    // Remove comments. '#' starts a comment unless inside quotes.
    bool inQuotes = false;
    int commentPos = -1;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('"'))
            inQuotes = !inQuotes;
        else if (ch == QLatin1Char('#') && !inQuotes) {
            commentPos = i;
            break;
        }
    }
    if (commentPos != -1)
        text = text.left(commentPos);
    text = text.simplified();

    QList<Token> tokens;
    if (text.isEmpty())
        return tokens;

    static const QRegularExpression titlePattern(
        QStringLiteral(R"KEYTAB(keyboard\s+"([^"]*)")KEYTAB"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression keyPattern(
        QStringLiteral(R"KEYTAB(key\s+([\w\+\s\-\*\.]+)\s*:\s*("([^"]*)"|(\w+)))KEYTAB"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch titleMatch = titlePattern.match(text);
    const QRegularExpressionMatch keyMatch = keyPattern.match(text);

    if (titleMatch.hasMatch()) {
        tokens.append({Token::TitleKeyword, QString()});
        tokens.append({Token::TitleText, titleMatch.captured(1)});
    } else if (keyMatch.hasMatch()) {
        tokens.append({Token::KeyKeyword, QString()});
        QString seq = keyMatch.captured(1);
        seq.remove(QLatin1Char(' '));
        tokens.append({Token::KeySequence, seq});

        if (!keyMatch.captured(3).isNull() && keyMatch.lastCapturedIndex() >= 3 &&
            !keyMatch.captured(2).isEmpty() && keyMatch.captured(2).startsWith(QLatin1Char('"')))
            tokens.append({Token::OutputText, keyMatch.captured(3)});
        else if (!keyMatch.captured(4).isEmpty())
            tokens.append({Token::Command, keyMatch.captured(4)});
    }

    return tokens;
}

// 对应C++: bool KeyboardTranslatorReader::decodeSequence(...)
bool KeyboardTranslatorReader::decodeSequence(const QString &text,
                                              int &keyCode,
                                              Qt::KeyboardModifiers &modifiers,
                                              Qt::KeyboardModifiers &modifierMask,
                                              KeyboardTranslatorStates &flags,
                                              KeyboardTranslatorStates &flagMask)
{
    bool isWanted = true;
    QString buffer;

    Qt::KeyboardModifiers tempModifiers = modifiers;
    Qt::KeyboardModifiers tempModifierMask = modifierMask;
    KeyboardTranslatorStates tempFlags = flags;
    KeyboardTranslatorStates tempFlagMask = flagMask;
    int tempKeyCode = keyCode;

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const bool isFirstLetter = (i == 0);
        const bool isLastLetter = (i == text.size() - 1);
        bool endOfItem = true;

        if (ch.isLetterOrNumber()) {
            endOfItem = false;
            buffer += ch;
        } else if (isFirstLetter) {
            buffer += ch;
        }

        if ((endOfItem || isLastLetter) && !buffer.isEmpty()) {
            Qt::KeyboardModifier itemModifier = Qt::NoModifier;
            int itemKeyCode = 0;
            KeyboardTranslatorStates itemFlag = NoState;

            if (parseAsModifier(buffer, itemModifier)) {
                tempModifierMask |= itemModifier;
                if (isWanted)
                    tempModifiers |= itemModifier;
            } else if (parseAsStateFlag(buffer, itemFlag)) {
                tempFlagMask |= itemFlag;
                if (isWanted)
                    tempFlags |= itemFlag;
            } else if (parseAsKeyCode(buffer, itemKeyCode)) {
                tempKeyCode = itemKeyCode;
            } else {
                qCDebug(qtermwidgetLogger) << "Unable to parse key binding item:" << buffer;
            }

            buffer.clear();
        }

        if (ch == QLatin1Char('+'))
            isWanted = true;
        else if (ch == QLatin1Char('-'))
            isWanted = false;
    }

    keyCode = tempKeyCode;
    modifiers = tempModifiers;
    modifierMask = tempModifierMask;
    flags = tempFlags;
    flagMask = tempFlagMask;
    return true;
}

// 对应C++: bool KeyboardTranslatorReader::parseAsModifier(...)
bool KeyboardTranslatorReader::parseAsModifier(const QString &item,
                                               Qt::KeyboardModifier &modifier)
{
    const QString lower = item.toLower();
    if (lower == QLatin1String("shift")) {
        modifier = Qt::ShiftModifier;
        return true;
    }
    if (lower == QLatin1String("ctrl") || lower == QLatin1String("control")) {
        modifier = Qt::ControlModifier;
        return true;
    }
    if (lower == QLatin1String("alt")) {
        modifier = Qt::AltModifier;
        return true;
    }
    if (lower == QLatin1String("meta")) {
        modifier = Qt::MetaModifier;
        return true;
    }
    if (lower == QLatin1String("keypad")) {
        modifier = Qt::KeypadModifier;
        return true;
    }
    return false;
}

// 对应C++: bool KeyboardTranslatorReader::parseAsStateFlag(...)
bool KeyboardTranslatorReader::parseAsStateFlag(const QString &item,
                                                KeyboardTranslatorStates &flag)
{
    const QString lower = item.toLower();
    if (lower == QLatin1String("appcukeys") || lower == QLatin1String("appcursorkeys")) {
        flag = CursorKeysState;
        return true;
    }
    if (lower == QLatin1String("ansi")) {
        flag = AnsiState;
        return true;
    }
    if (lower == QLatin1String("newline")) {
        flag = NewLineState;
        return true;
    }
    if (lower == QLatin1String("appscreen")) {
        flag = AlternateScreenState;
        return true;
    }
    if (lower == QLatin1String("anymod") || lower == QLatin1String("anymodifier")) {
        flag = AnyModifierState;
        return true;
    }
    if (lower == QLatin1String("appkeypad")) {
        flag = ApplicationKeypadState;
        return true;
    }
    return false;
}

// 对应C++: bool KeyboardTranslatorReader::parseAsKeyCode(...)
bool KeyboardTranslatorReader::parseAsKeyCode(const QString &item, int &keyCode)
{
    const QString lower = item.toLower();

    // Backwards-compatibility / special key names first.
    static const QHash<QString, int> specialKeys = {
        {QStringLiteral("prior"), Qt::Key_PageUp},
        {QStringLiteral("next"), Qt::Key_PageDown},
        {QStringLiteral("tab"), Qt::Key_Tab},
        {QStringLiteral("return"), Qt::Key_Return},
        {QStringLiteral("enter"), Qt::Key_Enter},
        {QStringLiteral("escape"), Qt::Key_Escape},
        {QStringLiteral("space"), Qt::Key_Space},
        {QStringLiteral("up"), Qt::Key_Up},
        {QStringLiteral("down"), Qt::Key_Down},
        {QStringLiteral("left"), Qt::Key_Left},
        {QStringLiteral("right"), Qt::Key_Right},
        {QStringLiteral("insert"), Qt::Key_Insert},
        {QStringLiteral("delete"), Qt::Key_Delete},
        {QStringLiteral("home"), Qt::Key_Home},
        {QStringLiteral("end"), Qt::Key_End},
        {QStringLiteral("pageup"), Qt::Key_PageUp},
        {QStringLiteral("pagedown"), Qt::Key_PageDown},
        {QStringLiteral("backspace"), Qt::Key_Backspace},
        {QStringLiteral("backtab"), Qt::Key_Backtab},
    };
    const auto specialIt = specialKeys.constFind(lower);
    if (specialIt != specialKeys.constEnd()) {
        keyCode = specialIt.value();
        return true;
    }

    // Function keys F1-F35.
    if (lower.startsWith(QLatin1Char('f')) && lower.size() > 1) {
        bool ok = false;
        const int funcNum = lower.mid(1).toInt(&ok);
        if (ok && funcNum >= 1 && funcNum <= 35) {
            keyCode = static_cast<int>(Qt::Key_F1) + (funcNum - 1);
            return true;
        }
    }

    // Digit keys 0-9.
    if (lower.size() == 1 && lower.at(0).isDigit()) {
        keyCode = static_cast<int>(Qt::Key_0) + lower.at(0).digitValue();
        return true;
    }

    // Letter keys a-z.
    if (lower.size() == 1 && lower.at(0).isLetter()) {
        keyCode = static_cast<int>(Qt::Key_A) + (lower.at(0).toLower().unicode() - 'a');
        return true;
    }

    // Fall back to QKeySequence parsing.
    auto tryKeySequence = [&keyCode](const QString &candidate) -> bool {
        const QKeySequence sequence = QKeySequence::fromString(candidate);
        if (!sequence.isEmpty() && sequence.count() > 0) {
            const int combined = sequence[0].toCombined();
            keyCode = combined & ~static_cast<int>(Qt::KeyboardModifierMask);
            return true;
        }
        return false;
    };

    if (tryKeySequence(item))
        return true;

    // Try a capitalized version ("Home", "Pageup", ...).
    if (!item.isEmpty()) {
        QString titled = item.toLower();
        titled[0] = titled[0].toUpper();
        if (tryKeySequence(titled))
            return true;
    }

    keyCode = Qt::Key_unknown;
    return false;
}

// 对应C++: bool KeyboardTranslatorReader::parseAsCommand(...)
KeyboardTranslatorCommand KeyboardTranslatorReader::parseAsCommand(const QString &text)
{
    const QString lower = text.toLower();
    if (lower == QLatin1String("erase"))                return EraseCommand;
    if (lower == QLatin1String("scrollpageup"))         return ScrollPageUpCommand;
    if (lower == QLatin1String("scrollpagedown"))       return ScrollPageDownCommand;
    if (lower == QLatin1String("scrolllineup"))         return ScrollLineUpCommand;
    if (lower == QLatin1String("scrolllinedown"))       return ScrollLineDownCommand;
    if (lower == QLatin1String("scrolllock"))           return ScrollLockCommand;
    if (lower == QLatin1String("scrolluptotop"))        return ScrollUpToTopCommand;
    if (lower == QLatin1String("scrolldowntobottom"))   return ScrollDownToBottomCommand;
    return NoCommand;
}

// 对应C++: Entry KeyboardTranslatorReader::createEntry(const QString& condition, const QString& result)
KeyboardTranslatorEntry KeyboardTranslatorReader::createEntry(const QString &condition,
                                                              const QString &result)
{
    QString entryString = QStringLiteral("keyboard \"temporary\"\nkey ") + condition + QStringLiteral(" : ");

    const KeyboardTranslatorCommand command = parseAsCommand(result);
    if (command != NoCommand)
        entryString += result;
    else
        entryString += QLatin1Char('"') + result + QLatin1Char('"');

    KeyboardTranslatorReader reader(entryString);
    if (reader.hasNextEntry())
        return reader.nextEntry();
    return KeyboardTranslatorEntry();
}

// ---------------------------------------------------------------------------
// KeyboardTranslatorWriter
// ---------------------------------------------------------------------------

KeyboardTranslatorWriter::KeyboardTranslatorWriter(QIODevice *destination)
    : _destination(destination)
{
}

// 对应C++: void KeyboardTranslatorWriter::writeHeader(const QString& description)
void KeyboardTranslatorWriter::writeHeader(const QString &description)
{
    if (!_destination || !_destination->isWritable())
        return;
    QTextStream stream(_destination);
    stream << "keyboard \"" << description << "\"\n";
}

// 对应C++: void KeyboardTranslatorWriter::writeEntry(const Entry& entry)
void KeyboardTranslatorWriter::writeEntry(const KeyboardTranslatorEntry &entry)
{
    if (!_destination || !_destination->isWritable())
        return;

    QString result;
    if (entry.command() != NoCommand)
        result = entry.resultToString();
    else
        result = QLatin1Char('"') + entry.resultToString() + QLatin1Char('"');

    QTextStream stream(_destination);
    stream << "key " << entry.conditionToString() << " : " << result << '\n';
}

// ---------------------------------------------------------------------------
// KeyboardTranslatorManager
// ---------------------------------------------------------------------------

// 对应C++: const char* KeyboardTranslatorManager::defaultTranslatorText
const char *KeyboardTranslatorManager::defaultTranslatorText =
    "keyboard \"Fallback Key Translator\"\n"
    "key Tab : \"\\t\"";

KeyboardTranslatorManager *KeyboardTranslatorManager::_instance = nullptr;

// 对应C++: KeyboardTranslatorManager* KeyboardTranslatorManager::instance()
KeyboardTranslatorManager *KeyboardTranslatorManager::instance()
{
    if (!_instance)
        _instance = new KeyboardTranslatorManager();
    return _instance;
}

KeyboardTranslatorManager::KeyboardTranslatorManager()
    : _haveLoadedAll(false)
{
}

KeyboardTranslatorManager::~KeyboardTranslatorManager()
{
    qDeleteAll(_translators);
    _translators.clear();
}

// 对应C++: QString KeyboardTranslatorManager::findTranslatorPath(const QString& name)
QString KeyboardTranslatorManager::findTranslatorPath(const QString &name)
{
    return getKbLayoutDir() + QLatin1Char('/') + name + QStringLiteral(".keytab");
}

// 对应C++: void KeyboardTranslatorManager::findTranslators()
void KeyboardTranslatorManager::findTranslators()
{
    const QString kbDir = getKbLayoutDir();
    const QDir dir(kbDir);
    if (!dir.exists()) {
        _haveLoadedAll = true;
        return;
    }

    const QStringList files = dir.entryList({QStringLiteral("*.keytab")}, QDir::Files);
    for (const QString &filename : files) {
        QString name = filename;
        name.chop(7); // remove ".keytab"
        if (!_translators.contains(name))
            _translators.insert(name, nullptr);
    }

    _haveLoadedAll = true;
}

// 对应C++: const KeyboardTranslator* KeyboardTranslatorManager::findTranslator(const QString& name)
const KeyboardTranslator *KeyboardTranslatorManager::findTranslator(const QString &name)
{
    if (name.isEmpty())
        return defaultTranslator();

    if (_translators.contains(name) && _translators.value(name) != nullptr)
        return _translators.value(name);

    KeyboardTranslator *translator = loadTranslator(name);

    if (translator != nullptr)
        _translators.insert(name, translator);
    else if (!name.isEmpty())
        qCDebug(qtermwidgetLogger) << "Unable to load translator" << name;

    return translator;
}

// 对应C++: KeyboardTranslator* KeyboardTranslatorManager::loadTranslator(const QString& name)
KeyboardTranslator *KeyboardTranslatorManager::loadTranslator(const QString &name)
{
    const QString path = findTranslatorPath(name);

    if (name.isEmpty() || !QFile::exists(path))
        return nullptr;

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return nullptr;

    return loadTranslator(&source, name);
}

// 对应C++: KeyboardTranslator* KeyboardTranslatorManager::loadTranslator(QIODevice* source, const QString& name)
KeyboardTranslator *KeyboardTranslatorManager::loadTranslator(QIODevice *source,
                                                              const QString &name)
{
    auto *translator = new KeyboardTranslator(name);
    KeyboardTranslatorReader reader(source);

    translator->setDescription(reader.description());

    while (reader.hasNextEntry())
        translator->addEntry(reader.nextEntry());

    if (!reader.parseError())
        return translator;

    delete translator;
    return nullptr;
}

// 对应C++: const KeyboardTranslator* KeyboardTranslatorManager::defaultTranslator()
const KeyboardTranslator *KeyboardTranslatorManager::defaultTranslator()
{
    const KeyboardTranslator *translator = findTranslator(QStringLiteral("default"));
    if (translator == nullptr) {
        QByteArray data(defaultTranslatorText);
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        translator = loadTranslator(&buffer, QStringLiteral("fallback"));
    }
    return translator;
}

// 对应C++: bool KeyboardTranslatorManager::saveTranslator(const KeyboardTranslator* translator)
bool KeyboardTranslatorManager::saveTranslator(const KeyboardTranslator *translator)
{
    Q_UNUSED(translator);
    qCDebug(qtermwidgetLogger) << "KeyboardTranslatorManager::saveTranslator unimplemented";
    return true; // Simplified implementation, matching the Python port.
}

// 对应C++: void KeyboardTranslatorManager::addTranslator(KeyboardTranslator* translator)
void KeyboardTranslatorManager::addTranslator(KeyboardTranslator *translator)
{
    _translators.insert(translator->name(), translator);

    if (!saveTranslator(translator))
        qCDebug(qtermwidgetLogger) << "Unable to save translator" << translator->name() << "to disk.";
}

// 对应C++: bool KeyboardTranslatorManager::deleteTranslator(const QString& name)
bool KeyboardTranslatorManager::deleteTranslator(const QString &name)
{
    if (!_translators.contains(name))
        return false;

    const QString path = findTranslatorPath(name);
    if (QFile::exists(path) && !QFile::remove(path)) {
        qCDebug(qtermwidgetLogger) << "Failed to remove translator -" << path;
        return false;
    }

    delete _translators.take(name);
    return true;
}

// 对应C++: QList<QString> KeyboardTranslatorManager::allTranslators()
QList<QString> KeyboardTranslatorManager::allTranslators()
{
    if (!_haveLoadedAll)
        findTranslators();

    return _translators.keys();
}

} // namespace Konsole
