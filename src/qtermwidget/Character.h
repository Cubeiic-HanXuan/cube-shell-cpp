#pragma once

// Character.h — C++ port of qtermwidget/character.py
//
// A single character in the terminal: unicode value, foreground/background
// colors, and a set of rendition flags controlling how it is drawn. Ported
// from the Python PySide6 version (converted from Konsole / QTermWidget).
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
//   Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QHash>
#include <QSet>
#include <QVector>
#include <QtGlobal>

#include "CharacterColor.h"

namespace Konsole {

class ScreenWindow; // fwd — windows tracked by ExtendedCharTable

// 对应C++: typedef unsigned char LineProperty;
typedef unsigned char LineProperty;

// 对应C++: static const int LINE_DEFAULT = 0; 等
inline constexpr int LINE_DEFAULT       = 0;
inline constexpr int LINE_WRAPPED       = (1 << 0);
inline constexpr int LINE_DOUBLEWIDTH   = (1 << 1);
inline constexpr int LINE_DOUBLEHEIGHT  = (1 << 2);

// Rendition flags.
// 对应C++: #define DEFAULT_RENDITION 0 等
inline constexpr quint16 DEFAULT_RENDITION = 0;
inline constexpr quint16 RE_BOLD           = (1 << 0);
inline constexpr quint16 RE_BLINK          = (1 << 1);
inline constexpr quint16 RE_UNDERLINE      = (1 << 2);
inline constexpr quint16 RE_REVERSE        = (1 << 3); // Screen only
inline constexpr quint16 RE_INTENSIVE      = (1 << 3); // Widget only
inline constexpr quint16 RE_ITALIC         = (1 << 4);
inline constexpr quint16 RE_CURSOR         = (1 << 5);
inline constexpr quint16 RE_EXTENDED_CHAR  = (1 << 6);
inline constexpr quint16 RE_FAINT          = (1 << 7);
inline constexpr quint16 RE_STRIKEOUT      = (1 << 8);
inline constexpr quint16 RE_CONCEAL        = (1 << 9);
inline constexpr quint16 RE_OVERLINE       = (1 << 10);

// A single character in the terminal.
// 对应C++: class Character
class Character {
public:
    // 对应C++: inline Character(quint16 _c=' ', CharacterColor _f=..., _b=..., quint8 _r=DEFAULT_RENDITION)
    inline Character(quint16 _c = ' ',
                     CharacterColor _f = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR),
                     CharacterColor _b = CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_BACK_COLOR),
                     quint16 _r = DEFAULT_RENDITION)
        : character(_c), rendition(_r), foregroundColor(_f), backgroundColor(_b) {}

    union {
        // The unicode character value for this character.
        // 对应C++: wchar_t character
        quint16 character;
        // Used when RE_EXTENDED_CHAR is set: a hash into the extended char table.
        quint16 extendedCharHash;
    };

    // A combination of RENDITION flags which specify options for drawing the character.
    // 对应C++: quint8 rendition
    quint16 rendition;

    // The foreground color used to draw this character.
    CharacterColor foregroundColor;
    // The background color used to draw this character.
    CharacterColor backgroundColor;

    // Returns true if this character has a transparent background when drawn
    // with the specified palette.
    // 对应C++: bool isTransparent(const ColorEntry* palette) const
    inline bool isTransparent(const ColorEntry *palette) const
    {
        return ((backgroundColor._colorSpace == COLOR_SPACE_DEFAULT &&
                 palette[backgroundColor._u + 0 + (backgroundColor._v ? BASE_COLORS : 0)].transparent)
                ||
                (backgroundColor._colorSpace == COLOR_SPACE_SYSTEM &&
                 palette[backgroundColor._u + 2 + (backgroundColor._v ? BASE_COLORS : 0)].transparent));
    }

    // Returns the font weight to use when drawing this character.
    // 对应C++: ColorEntry::FontWeight fontWeight(const ColorEntry* base) const
    inline ColorEntry::FontWeight fontWeight(const ColorEntry *base) const
    {
        if (backgroundColor._colorSpace == COLOR_SPACE_DEFAULT)
            return base[backgroundColor._u + 0 + (backgroundColor._v ? BASE_COLORS : 0)].fontWeight;
        else if (backgroundColor._colorSpace == COLOR_SPACE_SYSTEM)
            return base[backgroundColor._u + 2 + (backgroundColor._v ? BASE_COLORS : 0)].fontWeight;
        else
            return ColorEntry::UseCurrentFormat;
    }

    // Returns true if this character's format (colors and rendition flags) equals another's.
    inline bool equalsFormat(const Character &other) const
    {
        return backgroundColor == other.backgroundColor &&
               foregroundColor == other.foregroundColor &&
               rendition == other.rendition;
    }

    // 对应C++: inline bool isLineChar() const
    inline bool isLineChar() const
    {
        if (rendition & RE_EXTENDED_CHAR) return false;
        return (character & 0xFF80) == 0x2500;
    }

    // 对应C++: inline bool isSpace() const
    inline bool isSpace() const
    {
        if (rendition & RE_EXTENDED_CHAR) return false;
        return QChar(character).isSpace();
    }

    friend bool operator==(const Character &a, const Character &b)
    {
        return a.character == b.character &&
               a.rendition == b.rendition &&
               a.foregroundColor == b.foregroundColor &&
               a.backgroundColor == b.backgroundColor;
    }
    friend bool operator!=(const Character &a, const Character &b)
    {
        return !(a == b);
    }
};

// Stores a table of unicode character sequences referenced by hash keys.
// 对应C++: class ExtendedCharTable
class ExtendedCharTable {
public:
    // 对应C++: ExtendedCharTable()
    ExtendedCharTable() = default;

    // Adds a unicode character sequence to the table and returns a hash code
    // which can be used later to look the sequence up with lookupExtendedChar().
    // 对应C++: uint createExtendedChar(uint* unicodePoints, ushort length)
    uint createExtendedChar(const uint *unicodePoints, ushort length);

    // Looks up and returns a pointer to a sequence of unicode characters added
    // with createExtendedChar().
    // 对应C++: uint* lookupExtendedChar(uint hash, ushort& length) const
    uint *lookupExtendedChar(uint hash, ushort &length) const;

    // The global single instance.
    // 对应C++: static ExtendedCharTable instance
    static ExtendedCharTable instance;

    // Keeps track of all ScreenWindows; used by createExtendedChar() when
    // scavenging unused extended characters across every screen.
    // 对应C++: QSet<ScreenWindow*> windows (public member)
    QSet<ScreenWindow *> windows;

private:
    // Computes the hash key of a sequence of unicode points.
    uint extendedCharHash(const uint *unicodePoints, ushort length) const;
    // Tests whether the entry specified by hash matches the given sequence.
    bool extendedCharMatch(uint hash, const uint *unicodePoints, ushort length) const;

    QHash<uint, QVector<uint>> extendedCharTable;
};

// VT100 graphics character table.
// 对应C++: extern unsigned short vt100_graphics[32]
extern unsigned short vt100_graphics[32];

} // namespace Konsole
