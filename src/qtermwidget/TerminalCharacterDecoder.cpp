// TerminalCharacterDecoder.cpp — C++ port of qtermwidget/terminal_character_decoder.py
//
// Decodes lines of terminal characters into plain text or HTML markup.
// See TerminalCharacterDecoder.h for the class documentation.
//
// Original copyright:
//   Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

#include "TerminalCharacterDecoder.h"

#include <QColor>

#include "konsole_wcwidth.h"

namespace Konsole {

// ---------------------------------------------------------------------------
// PlainTextDecoder
// ---------------------------------------------------------------------------

// 对应C++: PlainTextDecoder::PlainTextDecoder()
PlainTextDecoder::PlainTextDecoder()
    : _output(nullptr), _includeTrailingWhitespace(true), _recordLinePositions(false)
{
}

// 对应C++: void PlainTextDecoder::setTrailingWhitespace(bool enable)
void PlainTextDecoder::setTrailingWhitespace(bool enable)
{
    _includeTrailingWhitespace = enable;
}

// 对应C++: bool PlainTextDecoder::trailingWhitespace() const
bool PlainTextDecoder::trailingWhitespace() const
{
    return _includeTrailingWhitespace;
}

// 对应C++: QList<int> PlainTextDecoder::linePositions() const
QList<int> PlainTextDecoder::linePositions() const
{
    return _linePositions;
}

// 对应C++: void PlainTextDecoder::setRecordLinePositions(bool record)
void PlainTextDecoder::setRecordLinePositions(bool record)
{
    _recordLinePositions = record;
}

// 对应C++: void PlainTextDecoder::begin(QTextStream* output)
void PlainTextDecoder::begin(QTextStream *output)
{
    _output = output;
    if (!_linePositions.isEmpty())
        _linePositions.clear();
}

// 对应C++: void PlainTextDecoder::end()
void PlainTextDecoder::end()
{
    _output = nullptr;
}

// 对应C++: void PlainTextDecoder::decodeLine(const Character* const characters,
//                                            int count, LineProperty /*properties*/)
void PlainTextDecoder::decodeLine(const Character *const characters,
                                  int count, LineProperty /*properties*/)
{
    Q_ASSERT(_output);

    // If we should record new line positions, we need to find out
    // the current position in the output string.
    if (_recordLinePositions && _output->string()) {
        // Note: QString::size() returns int; keep semantics identical to the
        // Python `len(self._output.string())`.
        const int pos = _output->string()->size();
        _linePositions.append(pos);
    }

    if (!characters)
        return;

    QString plainText;
    plainText.reserve(count);

    int outputCount = count;

    // determine the number of characters which will be appended,
    // truncating trailing whitespace if it is not wanted.
    if (!_includeTrailingWhitespace) {
        for (int i = count - 1; i >= 0; i--) {
            if (!characters[i].isSpace())
                break;
            else
                outputCount--;
        }
    }

    for (int i = 0; i < outputCount;) {
        if (characters[i].rendition & RE_EXTENDED_CHAR) {
            ushort extendedCharLength = 0;
            const uint *chars = ExtendedCharTable::instance.lookupExtendedChar(
                characters[i].character, extendedCharLength);
            if (chars) {
                // Build the QString from the sequence of unicode points.
                // 对应C++: QString s = QString::fromUcs4(chars, extendedCharLength);
                QString charStr = QString::fromUcs4(
                    reinterpret_cast<const char32_t *>(chars), extendedCharLength);
                plainText.append(charStr);
                // Wide / extended chars occupy string_width() cells; skip the
                // continuation cell(s) so they are not emitted twice.
                i += qMax(1, string_width(charStr));
            } else {
                i += 1;
            }
        } else {
            plainText.append(QChar(characters[i].character));
            // Skip continuation cell of wide (double-width) chars.
            i += qMax(1, konsole_wcwidth(characters[i].character));
        }
    }

    *_output << plainText;
}

// ---------------------------------------------------------------------------
// HTMLDecoder
// ---------------------------------------------------------------------------

// 对应C++: HTMLDecoder::HTMLDecoder()
HTMLDecoder::HTMLDecoder()
    : _output(nullptr),
      _colorTable(base_color_table()),
      _innerSpanOpen(false),
      _lastRendition(DEFAULT_RENDITION)
{
}

// 对应C++: void HTMLDecoder::setColorTable(const ColorEntry* table)
void HTMLDecoder::setColorTable(const ColorEntry *table)
{
    _colorTable = table;
}

// 对应C++: void HTMLDecoder::begin(QTextStream* output)
void HTMLDecoder::begin(QTextStream *output)
{
    _output = output;

    QString text;
    openSpan(text, QStringLiteral("font-family:monospace"));

    *output << text;
}

// 对应C++: void HTMLDecoder::end()
void HTMLDecoder::end()
{
    Q_ASSERT(_output);

    QString text;
    closeSpan(text);

    *_output << text;

    _output = nullptr;
}

// 对应C++: void HTMLDecoder::decodeLine(const Character* const characters,
//                                        int count, LineProperty /*properties*/)
void HTMLDecoder::decodeLine(const Character *const characters,
                             int count, LineProperty /*properties*/)
{
    Q_ASSERT(_output);

    QString text;

    int spaceCount = 0;

    for (int i = 0; i < count; i++) {
        // check if appearance of character is different from previous char
        if (characters[i].rendition != _lastRendition ||
            characters[i].foregroundColor != _lastForeColor ||
            characters[i].backgroundColor != _lastBackColor) {

            // if there is a current span open, close it first
            if (_innerSpanOpen)
                closeSpan(text);

            // keep a record of current character state
            _lastRendition = characters[i].rendition;
            _lastForeColor = characters[i].foregroundColor;
            _lastBackColor = characters[i].backgroundColor;

            QString style;

            // weight == normal or bold?
            bool useBold;
            ColorEntry::FontWeight weight = characters[i].fontWeight(_colorTable);
            if (weight == ColorEntry::UseCurrentFormat)
                useBold = (_lastRendition & RE_BOLD) != 0;
            else
                useBold = (weight == ColorEntry::Bold);

            if (useBold)
                style.append(QStringLiteral("font-weight:bold;"));

            if (_lastRendition & RE_UNDERLINE)
                style.append(QStringLiteral("font-decoration:underline;"));

            // colours - a colour table must have been defined first
            if (_colorTable) {
                style.append(QStringLiteral("color:%1;")
                                 .arg(_lastForeColor.color(_colorTable).name()));

                if (!characters[i].isTransparent(_colorTable)) {
                    style.append(QStringLiteral("background-color:%1;")
                                     .arg(_lastBackColor.color(_colorTable).name()));
                }
            }

            // open the span with the current style
            openSpan(text, style);
            _innerSpanOpen = true;
        }

        // count the number of consecutive spaces to decide between
        // a normal space and a non-breaking space (&#160;).
        if (characters[i].isSpace()) {
            spaceCount++;
        } else {
            spaceCount = 0;
        }

        if (spaceCount < 2) {
            // handle extended characters (hash into the extended char table)
            if (characters[i].rendition & RE_EXTENDED_CHAR) {
                ushort extendedCharLength = 0;
                const uint *chars = ExtendedCharTable::instance.lookupExtendedChar(
                    characters[i].character, extendedCharLength);
                if (chars) {
                    // 对应C++: text.append(QString::fromUcs4(chars, extendedCharLength));
                    text.append(QString::fromUcs4(
                        reinterpret_cast<const char32_t *>(chars), extendedCharLength));
                }
            } else {
                // escape HTML special characters
                const QChar ch(characters[i].character);
                if (ch == QLatin1Char('<'))
                    text.append(QLatin1String("&lt;"));
                else if (ch == QLatin1Char('>'))
                    text.append(QLatin1String("&gt;"));
                else if (ch == QLatin1Char('&'))
                    text.append(QLatin1String("&amp;"));
                else
                    text.append(ch);
            }
        } else {
            // second (and subsequent) consecutive spaces are emitted as
            // non-breaking spaces so that runs of blanks survive HTML collapsing.
            text.append(QLatin1String("&#160;"));
        }
    }

    // close any remaining open span
    if (_innerSpanOpen)
        closeSpan(text);

    // start a new line
    text.append(QLatin1String("<br>"));

    *_output << text;
}

// 对应C++: void HTMLDecoder::openSpan(QString& text, const QString& style)
void HTMLDecoder::openSpan(QString &text, const QString &style)
{
    text.append(QStringLiteral("<span style=\"%1\">").arg(style));
}

// 对应C++: void HTMLDecoder::closeSpan(QString& text)
void HTMLDecoder::closeSpan(QString &text)
{
    text.append(QLatin1String("</span>"));
}

} // namespace Konsole
