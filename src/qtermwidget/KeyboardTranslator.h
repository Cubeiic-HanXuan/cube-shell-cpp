#pragma once

// KeyboardTranslator.h — C++ port of qtermwidget/keyboard_translator.py
//
// Keyboard translation tables. Parses ".keytab" files and maps key events to
// byte sequences/commands. Ported from the Python PySide6 version, which was
// itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include <QByteArray>
#include <QBuffer>
#include <QDir>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <Qt>

class QKeyEvent;

namespace Konsole {

// Control modifier alias. On macOS, Qt::ControlModifier is Cmd and the
// terminal Ctrl is Qt::MetaModifier.
// 对应C++: #ifdef Q_OS_MAC  const Qt::KeyboardModifier CTRL_MOD = Qt::MetaModifier; #else ...
#if defined(Q_OS_MAC)
inline constexpr Qt::KeyboardModifier CTRL_MOD = Qt::MetaModifier;
#else
inline constexpr Qt::KeyboardModifier CTRL_MOD = Qt::ControlModifier;
#endif

// Keyboard translator state flags.
// 对应C++: KeyboardTranslator::State enum
enum KeyboardTranslatorState {
    NoState                  = 0,
    NewLineState             = 1,
    AnsiState                = 2,
    CursorKeysState          = 4,
    AlternateScreenState     = 8,
    AnyModifierState         = 16,
    ApplicationKeypadState   = 32
};
Q_DECLARE_FLAGS(KeyboardTranslatorStates, KeyboardTranslatorState)
Q_DECLARE_OPERATORS_FOR_FLAGS(KeyboardTranslatorStates)

// Keyboard translator commands.
// 对应C++: KeyboardTranslator::Command enum
enum KeyboardTranslatorCommand {
    NoCommand                = 0,
    SendCommand              = 1,
    ScrollPageUpCommand      = 2,
    ScrollPageDownCommand    = 4,
    ScrollLineUpCommand      = 8,
    ScrollLineDownCommand    = 16,
    ScrollLockCommand        = 32,
    ScrollUpToTopCommand     = 64,
    ScrollDownToBottomCommand= 128,
    EraseCommand             = 256
};
Q_DECLARE_FLAGS(KeyboardTranslatorCommands, KeyboardTranslatorCommand)
Q_DECLARE_OPERATORS_FOR_FLAGS(KeyboardTranslatorCommands)

// A single entry in a keyboard translation table.
// 对应C++: KeyboardTranslator::Entry class
class KeyboardTranslatorEntry {
public:
    // 对应C++: Entry::Entry()
    KeyboardTranslatorEntry();

    // Returns true if this entry is null.
    bool isNull() const;

    KeyboardTranslatorCommand command() const;
    void setCommand(KeyboardTranslatorCommand command);

    // Returns the character sequence associated with this entry.
    // 对应C++: QByteArray Entry::text(bool expandWildCards = false, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const
    QByteArray text(bool expandWildCards = false,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier) const;

    // Sets the character sequence (unescaping any backslash sequences).
    void setText(const QByteArray &text);

    int keyCode() const;
    void setKeyCode(int keyCode);

    Qt::KeyboardModifiers modifiers() const;
    Qt::KeyboardModifiers modifierMask() const;
    void setModifiers(Qt::KeyboardModifiers modifiers);
    void setModifierMask(Qt::KeyboardModifiers mask);

    KeyboardTranslatorStates state() const;
    KeyboardTranslatorStates stateMask() const;
    void setState(KeyboardTranslatorStates state);
    void setStateMask(KeyboardTranslatorStates mask);

    // Returns true if this entry matches the given key code, modifiers and state.
    // 对应C++: bool Entry::matches(int keyCode, Qt::KeyboardModifiers modifiers, States testState) const
    bool matches(int keyCode,
                 Qt::KeyboardModifiers modifiers,
                 KeyboardTranslatorStates testState) const;

    // Returns the character sequence with non-printable characters escaped.
    // 对应C++: QByteArray Entry::escapedText(bool expandWildCards = false, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const
    QByteArray escapedText(bool expandWildCards = false,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) const;

    // Returns the condition portion of this entry as a string.
    QString conditionToString() const;

    // Returns the result portion of this entry as a string.
    QString resultToString(bool expandWildCards = false,
                           Qt::KeyboardModifiers modifiers = Qt::NoModifier) const;

    bool operator==(const KeyboardTranslatorEntry &rhs) const;

private:
    void insertModifier(QString &item, int modifier) const;
    void insertState(QString &item, int state) const;
    QByteArray unescape(const QByteArray &input) const;

    int _keyCode;
    Qt::KeyboardModifiers _modifiers;
    Qt::KeyboardModifiers _modifierMask;
    KeyboardTranslatorStates _state;
    KeyboardTranslatorStates _stateMask;
    KeyboardTranslatorCommand _command;
    QByteArray _text;
};

// A keyboard translation table.
// 对应C++: class KeyboardTranslator
class KeyboardTranslator {
public:
    explicit KeyboardTranslator(const QString &name);

    QString name() const;
    void setName(const QString &name);
    QString description() const;
    void setDescription(const QString &description);

    // Looks up an entry by key code, modifiers and state flags.
    // 对应C++: Entry KeyboardTranslator::findEntry(int keyCode, Qt::KeyboardModifiers modifiers, States state = NoState) const
    KeyboardTranslatorEntry findEntry(int keyCode,
                                      Qt::KeyboardModifiers modifiers,
                                      KeyboardTranslatorStates state = KeyboardTranslatorStates()) const;

    void addEntry(const KeyboardTranslatorEntry &entry);
    void replaceEntry(const KeyboardTranslatorEntry &existing,
                      const KeyboardTranslatorEntry &replacement);
    void removeEntry(const KeyboardTranslatorEntry &entry);
    QList<KeyboardTranslatorEntry> entries() const;

private:
    QString _name;
    QString _description;
    QHash<int, QList<KeyboardTranslatorEntry>> _entries;
};

// Parses a .keytab file or string into KeyboardTranslatorEntry objects.
// 对应C++: class KeyboardTranslatorReader
class KeyboardTranslatorReader {
public:
    explicit KeyboardTranslatorReader(QIODevice *source);
    explicit KeyboardTranslatorReader(const QString &source);

    QString description() const;
    bool hasNextEntry() const;
    KeyboardTranslatorEntry nextEntry();
    bool parseError() const;

    // Creates a new Entry from a binding description.
    // 对应C++: static Entry KeyboardTranslatorReader::createEntry(const QString& condition, const QString& result)
    static KeyboardTranslatorEntry createEntry(const QString &condition,
                                               const QString &result);

private:
    struct Token {
        enum Type {
            TitleKeyword = 0,
            TitleText,
            KeyKeyword,
            KeySequence,
            Command,
            OutputText
        };
        Type type;
        QString text;
    };

    void readNext();
    QList<Token> tokenize(const QString &line) const;
    bool decodeSequence(const QString &text,
                        int &keyCode,
                        Qt::KeyboardModifiers &modifiers,
                        Qt::KeyboardModifiers &modifierMask,
                        KeyboardTranslatorStates &flags,
                        KeyboardTranslatorStates &flagMask);

    static bool parseAsModifier(const QString &item, Qt::KeyboardModifier &modifier);
    static bool parseAsStateFlag(const QString &item, KeyboardTranslatorStates &flag);
    static bool parseAsKeyCode(const QString &item, int &keyCode);
    static KeyboardTranslatorCommand parseAsCommand(const QString &text);

    QIODevice *_device;      // nullptr when parsing from a string
    QStringList _lines;      // used when parsing from a string
    int _lineIndex;
    QString _description;
    bool _hasNext;
    KeyboardTranslatorEntry _nextEntry;
};

// Writes keyboard translation tables to disk.
// 对应C++: class KeyboardTranslatorWriter
class KeyboardTranslatorWriter {
public:
    explicit KeyboardTranslatorWriter(QIODevice *destination);

    void writeHeader(const QString &description);
    void writeEntry(const KeyboardTranslatorEntry &entry);

private:
    QIODevice *_destination;
};

// Manages and supplies the keyboard translation tables.
// 对应C++: class KeyboardTranslatorManager
class KeyboardTranslatorManager {
public:
    // Returns the global KeyboardTranslatorManager instance.
    // 对应C++: static KeyboardTranslatorManager* KeyboardTranslatorManager::instance()
    static KeyboardTranslatorManager *instance();

    void addTranslator(KeyboardTranslator *translator);
    bool deleteTranslator(const QString &name);

    // Returns the keyboard translator with the given name, or a null pointer.
    // 对应C++: const KeyboardTranslator* KeyboardTranslatorManager::findTranslator(const QString& name)
    const KeyboardTranslator *findTranslator(const QString &name);

    // Returns the default fallback translator (never null).
    const KeyboardTranslator *defaultTranslator();

    // Saves a translator to disk (currently a stub, matching the Python port).
    bool saveTranslator(const KeyboardTranslator *translator);

    // Returns the names of all available translators.
    QList<QString> allTranslators();

    // Finds the path of a translator file.
    QString findTranslatorPath(const QString &name);

private:
    KeyboardTranslatorManager();
    ~KeyboardTranslatorManager();
    Q_DISABLE_COPY(KeyboardTranslatorManager)

    void findTranslators();
    KeyboardTranslator *loadTranslator(const QString &name);
    KeyboardTranslator *loadTranslator(QIODevice *source, const QString &name);

    QHash<QString, KeyboardTranslator *> _translators;
    bool _haveLoadedAll;

    static KeyboardTranslatorManager *_instance;
    static const char *defaultTranslatorText;
};

} // namespace Konsole
