// Character.cpp — implementation of ExtendedCharTable and the VT100 graphics
// table. Ported from qtermwidget/character.py.

#include "Character.h"

namespace Konsole {

ExtendedCharTable ExtendedCharTable::instance;

unsigned short vt100_graphics[32] = {
    // 0/0     1/1    2/2    3/3    4/4    5/5    6/6    7/7
    0x0020,    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1,    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0xF800,    0xF801, 0x2500, 0xF803, 0xF804, 0x251C, 0x2524, 0x2534,
    0x252C,    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7
};

uint ExtendedCharTable::createExtendedChar(const uint *unicodePoints, ushort length)
{
    if (length == 0)
        return 0;

    uint hash = extendedCharHash(unicodePoints, length);

    // If a matching sequence already exists, return its hash.
    if (extendedCharTable.contains(hash) && extendedCharMatch(hash, unicodePoints, length))
        return hash;

    // Store as [length, point1, point2, ...]
    QVector<uint> entry;
    entry.reserve(length + 1);
    entry.append(length);
    for (ushort i = 0; i < length; ++i)
        entry.append(unicodePoints[i]);
    extendedCharTable.insert(hash, entry);

    return hash;
}

uint *ExtendedCharTable::lookupExtendedChar(uint hash, ushort &length) const
{
    auto it = extendedCharTable.constFind(hash);
    if (it == extendedCharTable.constEnd() || it->isEmpty())
        return nullptr;

    const QVector<uint> &entry = it.value();
    length = static_cast<ushort>(entry[0]);
    // Return a pointer to the start of the unicode points (after the length).
    return const_cast<uint *>(entry.constData() + 1);
}

uint ExtendedCharTable::extendedCharHash(const uint *unicodePoints, ushort length) const
{
    uint hash = 0;
    for (ushort i = 0; i < length; ++i)
        hash = hash * 31 + unicodePoints[i];
    return hash;
}

bool ExtendedCharTable::extendedCharMatch(uint hash, const uint *unicodePoints, ushort length) const
{
    auto it = extendedCharTable.constFind(hash);
    if (it == extendedCharTable.constEnd() || it->isEmpty())
        return false;

    const QVector<uint> &entry = it.value();
    if (static_cast<ushort>(entry[0]) != length)
        return false;

    for (ushort i = 0; i < length; ++i) {
        if (entry[1 + i] != unicodePoints[i])
            return false;
    }
    return true;
}

} // namespace Konsole
