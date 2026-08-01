#pragma once

// CharacterColor.h — C++ port of qtermwidget/character_color.py
//
// Describes the color of a single terminal character. Ported from the Python
// PySide6 version, which was itself converted from Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QColor>
#include <QtGlobal>

namespace Konsole {

// 对应C++: #define BASE_COLORS   (2+8)
inline constexpr int BASE_COLORS   = 2 + 8;
// 对应C++: #define INTENSITIES   2
inline constexpr int INTENSITIES   = 2;
// 对应C++: #define TABLE_COLORS  (INTENSITIES*BASE_COLORS)
inline constexpr int TABLE_COLORS  = INTENSITIES * BASE_COLORS;

// 对应C++: #define DEFAULT_FORE_COLOR 0
inline constexpr int DEFAULT_FORE_COLOR = 0;
// 对应C++: #define DEFAULT_BACK_COLOR 1
inline constexpr int DEFAULT_BACK_COLOR = 1;

// Color space constants.
// 对应C++: #define COLOR_SPACE_UNDEFINED   0
inline constexpr quint8 COLOR_SPACE_UNDEFINED = 0;
// 对应C++: #define COLOR_SPACE_DEFAULT     1
inline constexpr quint8 COLOR_SPACE_DEFAULT   = 1;
// 对应C++: #define COLOR_SPACE_SYSTEM      2
inline constexpr quint8 COLOR_SPACE_SYSTEM    = 2;
// 对应C++: #define COLOR_SPACE_256         3
inline constexpr quint8 COLOR_SPACE_256       = 3;
// 对应C++: #define COLOR_SPACE_RGB         4
inline constexpr quint8 COLOR_SPACE_RGB       = 4;

// A single entry in the terminal display's color palette.
// 对应C++: class ColorEntry
class ColorEntry {
public:
    // Specifies the weight to use when drawing text with this color.
    // 对应C++: enum FontWeight
    enum FontWeight {
        // Always draw text in this color with a bold weight.
        Bold = 0,
        // Always draw text in this color with a normal weight.
        Normal = 1,
        // Use the current font weight set by the terminal application.
        UseCurrentFormat = 2
    };

    ColorEntry()
        : transparent(false), fontWeight(UseCurrentFormat) {}

    ColorEntry(const QColor &c, bool tr, FontWeight weight = UseCurrentFormat)
        : color(c), transparent(tr), fontWeight(weight) {}

    QColor color;
    bool   transparent;
    FontWeight fontWeight;
};

// 256-color mode helper.
// 对应C++: inline const QColor color256(quint8 u, const ColorEntry* base)
inline QColor color256(quint8 u, const ColorEntry *base)
{
    //   0.. 16: system colors
    if (u < 8)        return base[u + 2].color;
    u -= 8;
    if (u < 8)        return base[u + 2 + BASE_COLORS].color;
    u -= 8;

    //  16..231: 6x6x6 rgb color cube
    if (u < 216)      return QColor(((u / 36) % 6) ? (40 * ((u / 36) % 6) + 55) : 0,
                                    ((u /  6) % 6) ? (40 * ((u /  6) % 6) + 55) : 0,
                                    ((u /  1) % 6) ? (40 * ((u /  1) % 6) + 55) : 0);
    u -= 216;

    // 232..255: gray, leaving out black and white
    int gray = u * 10 + 8;
    return QColor(gray, gray, gray);
}

// Describes the color of a single character in the terminal.
// 对应C++: class CharacterColor
class CharacterColor {
public:
    // 对应C++: CharacterColor()
    CharacterColor()
        : _colorSpace(COLOR_SPACE_UNDEFINED), _u(0), _v(0), _w(0) {}

    // 对应C++: CharacterColor(quint8 colorSpace, int co)
    CharacterColor(quint8 colorSpace, int co)
        : _colorSpace(colorSpace), _u(0), _v(0), _w(0)
    {
        switch (colorSpace) {
        case COLOR_SPACE_DEFAULT:
            _u = co & 1;
            break;
        case COLOR_SPACE_SYSTEM:
            _u = co & 7;
            _v = (co >> 3) & 1;
            break;
        case COLOR_SPACE_256:
            _u = co & 255;
            break;
        case COLOR_SPACE_RGB:
            _u = (co >> 16) & 255;
            _v = (co >> 8) & 255;
            _w = co & 255;
            break;
        default:
            _colorSpace = COLOR_SPACE_UNDEFINED;
        }
    }

    // Returns true if this character color entry is valid.
    bool isValid() const { return _colorSpace != COLOR_SPACE_UNDEFINED; }

    // Set the value of this color from a normal system color to the
    // corresponding intensive system color.
    void setIntensive()
    {
        if (_colorSpace == COLOR_SPACE_SYSTEM || _colorSpace == COLOR_SPACE_DEFAULT)
            _v = 1;
    }

    // Returns the color within the specified color palette.
    // 对应C++: QColor color(const ColorEntry* palette) const
    QColor color(const ColorEntry *palette) const
    {
        switch (_colorSpace) {
        case COLOR_SPACE_DEFAULT:
            return palette[_u + 0 + (_v ? BASE_COLORS : 0)].color;
        case COLOR_SPACE_SYSTEM:
            return palette[_u + 2 + (_v ? BASE_COLORS : 0)].color;
        case COLOR_SPACE_256:
            return color256(_u, palette);
        case COLOR_SPACE_RGB:
            return QColor(_u, _v, _w);
        case COLOR_SPACE_UNDEFINED:
            return QColor();
        }
        Q_ASSERT(false); // invalid color space
        return QColor();
    }

    friend bool operator==(const CharacterColor &a, const CharacterColor &b)
    {
        return a._colorSpace == b._colorSpace &&
               a._u == b._u && a._v == b._v && a._w == b._w;
    }
    friend bool operator!=(const CharacterColor &a, const CharacterColor &b)
    {
        return !(a == b);
    }

private:
    quint8 _colorSpace;
    quint8 _u;
    quint8 _v;
    quint8 _w;

    // Character (character.h) needs raw field access for isTransparent() /
    // fontWeight(), mirroring the Python which reads the private fields.
    friend class Character;
};

// Standard color table. Defined in TerminalDisplay.cpp upstream; here provided
// as an inline function returning the precise Konsole base palette.
// 对应C++: extern const ColorEntry base_color_table[TABLE_COLORS]
const ColorEntry *base_color_table();

} // namespace Konsole
