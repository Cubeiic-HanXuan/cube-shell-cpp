// CharacterColor.cpp — base color table for the terminal palette.
//
// The Python version used a *simplified* palette (see character_color.py's
// create_base_color_table). This C++ port uses the precise Konsole base color
// table, matching upstream TerminalDisplay.cpp.

#include "CharacterColor.h"

namespace Konsole {

// 对应C++: const ColorEntry base_color_table[TABLE_COLORS]
// Layout: [ default fg, default bg, 8 system colors ] then the same block
// again with the 8 system colors intensified (bright variants).
static const ColorEntry s_base_color_table[TABLE_COLORS] = {
    // Normal intensity
    ColorEntry(QColor(0x00, 0x00, 0x00), false), // default foreground (black)
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), true),  // default background (white)

    ColorEntry(QColor(0x00, 0x00, 0x00), false), // black
    ColorEntry(QColor(0xB2, 0x18, 0x18), false), // red
    ColorEntry(QColor(0x18, 0xB2, 0x18), false), // green
    ColorEntry(QColor(0xB2, 0x68, 0x18), false), // yellow
    ColorEntry(QColor(0x18, 0x18, 0xB2), false), // blue
    ColorEntry(QColor(0xB2, 0x18, 0xB2), false), // magenta
    ColorEntry(QColor(0x18, 0xB2, 0xB2), false), // cyan
    ColorEntry(QColor(0xB2, 0xB2, 0xB2), false), // white

    // Intensive (bright) — default fg/bg repeated, then the 8 bright colors
    ColorEntry(QColor(0x00, 0x00, 0x00), false),
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), true),

    ColorEntry(QColor(0x68, 0x68, 0x68), false), // bright black
    ColorEntry(QColor(0xFF, 0x54, 0x54), false), // bright red
    ColorEntry(QColor(0x54, 0xFF, 0x54), false), // bright green
    ColorEntry(QColor(0xFF, 0xFF, 0x54), false), // bright yellow
    ColorEntry(QColor(0x54, 0x54, 0xFF), false), // bright blue
    ColorEntry(QColor(0xFF, 0x54, 0xFF), false), // bright magenta
    ColorEntry(QColor(0x54, 0xFF, 0xFF), false), // bright cyan
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), false)  // bright white
};

const ColorEntry *base_color_table()
{
    return s_base_color_table;
}

} // namespace Konsole
