#pragma once

// TerminalCharacterDecoder.h — C++ port of qtermwidget/terminal_character_decoder.py
//
// Decodes lines of terminal characters (unicode value + colors + rendition)
// into text, either as plain text or as HTML markup. Used for selection,
// clipboard copy and search. Ported from the Python PySide6 version
// (converted from Konsole / QTermWidget).
//
// Original copyright:
//   Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

#include <QList>
#include <QString>
#include <QTextStream>
#include <QtGlobal>

#include "Character.h"
#include "CharacterColor.h"

namespace Konsole {

// Base class for terminal character decoders.
//
// A decoder converts a line of terminal characters — each consisting of a
// unicode character, foreground/background colors and appearance-related
// rendition flags — into a text string. Derived classes may produce plain
// text without any color/appearance info, or markup that carries those
// extra attributes (HTML).
//
// 对应C++: class TerminalCharacterDecoder
class TerminalCharacterDecoder {
public:
    // 对应C++: TerminalCharacterDecoder()
    TerminalCharacterDecoder() = default;
    // 对应C++: virtual ~TerminalCharacterDecoder()
    virtual ~TerminalCharacterDecoder() = default;

    // Begin decoding characters. The resulting text is appended to @p output.
    // 对应C++: virtual void begin(QTextStream* output) = 0
    virtual void begin(QTextStream *output) = 0;

    // End decoding.
    // 对应C++: virtual void end() = 0
    virtual void end() = 0;

    // Converts a line of terminal characters with associated @p properties
    // into a text string and writes the string into the output QTextStream.
    // 对应C++: virtual void decodeLine(const Character* const characters,
    //                                 int count, LineProperty properties) = 0
    virtual void decodeLine(const Character *const characters,
                            int count, LineProperty properties) = 0;
};

// A terminal character decoder which produces plain text, ignoring colors
// and other appearance-related attributes of the original characters.
//
// 对应C++: class PlainTextDecoder : public TerminalCharacterDecoder
class PlainTextDecoder : public TerminalCharacterDecoder {
public:
    // 对应C++: PlainTextDecoder::PlainTextDecoder()
    PlainTextDecoder();

    // Set whether trailing whitespace at the end of lines is included in the
    // output. Defaults to true.
    // 对应C++: void setTrailingWhitespace(bool enable)
    void setTrailingWhitespace(bool enable);
    // Returns whether trailing whitespace at the end of lines is included in
    // the output.
    // 对应C++: bool trailingWhitespace() const
    bool trailingWhitespace() const;

    // Returns the character positions in the output stream at which new lines
    // were added. Returns an empty list if setRecordLinePositions() is false
    // or if the output device is not a string.
    // 对应C++: QList<int> linePositions() const
    QList<int> linePositions() const;
    // Enables recording of the character positions at which new lines are
    // added. See linePositions().
    // 对应C++: void setRecordLinePositions(bool record)
    void setRecordLinePositions(bool record);

    // 对应C++: void begin(QTextStream* output) override
    void begin(QTextStream *output) override;
    // 对应C++: void end() override
    void end() override;
    // 对应C++: void decodeLine(const Character* const characters,
    //                          int count, LineProperty /*properties*/) override
    void decodeLine(const Character *const characters,
                    int count, LineProperty properties) override;

private:
    QTextStream *_output;             // 对应C++: QTextStream* _output
    bool _includeTrailingWhitespace;  // 对应C++: bool _includeTrailingWhitespace
    bool _recordLinePositions;        // 对应C++: bool _recordLinePositions
    QList<int> _linePositions;        // 对应C++: QList<int> _linePositions
};

// A terminal character decoder which produces pretty HTML markup.
//
// 对应C++: class HTMLDecoder : public TerminalCharacterDecoder
class HTMLDecoder : public TerminalCharacterDecoder {
public:
    // Constructs an HTML decoder using a default black-on-white color scheme.
    // 对应C++: HTMLDecoder::HTMLDecoder()
    HTMLDecoder();

    // Sets the color table which the decoder uses to produce the HTML color
    // codes in its output.
    // 对应C++: void setColorTable(const ColorEntry* table)
    void setColorTable(const ColorEntry *table);

    // 对应C++: void begin(QTextStream* output) override
    void begin(QTextStream *output) override;
    // 对应C++: void end() override
    void end() override;
    // 对应C++: void decodeLine(const Character* const characters,
    //                          int count, LineProperty /*properties*/) override
    void decodeLine(const Character *const characters,
                    int count, LineProperty properties) override;

private:
    // 对应C++: void openSpan(QString& text, const QString& style)
    void openSpan(QString &text, const QString &style);
    // 对应C++: void closeSpan(QString& text)
    void closeSpan(QString &text);

    QTextStream *_output;                 // 对应C++: QTextStream* _output
    const ColorEntry *_colorTable;        // 对应C++: const ColorEntry* _colorTable
    bool _innerSpanOpen;                  // 对应C++: bool _innerSpanOpen
    quint16 _lastRendition;               // 对应C++: quint8 _lastRendition (Python注释;实际需16位)
    CharacterColor _lastForeColor;        // 对应C++: CharacterColor _lastForeColor
    CharacterColor _lastBackColor;        // 对应C++: CharacterColor _lastBackColor
};

} // namespace Konsole
