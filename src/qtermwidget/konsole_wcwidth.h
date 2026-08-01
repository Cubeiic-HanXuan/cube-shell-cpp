#pragma once

// konsole_wcwidth.h — C++ port of qtermwidget/wcwidth.py
//
// Width of a Unicode character in terminal cells.
//   0: combining / invisible
//   1: normal
//   2: wide (CJK etc.)
//  -1: control character
//
// Original: Markus Kuhn -- 2001-01-12 -- public domain
// KDE adaptations by Waldo Bastian; Qt4 rewrite by e_k.

#include <QString>
#include <QtGlobal>

namespace Konsole {

// 对应C++: int konsole_wcwidth(wchar_t ucs)
int konsole_wcwidth(quint32 ucs);

// 对应C++: int string_width(const std::wstring & wstr)
int string_width(const QString &text);

// Helpers used across the terminal code.
inline bool isWideChar(QChar c)  { return konsole_wcwidth(c.unicode()) == 2; }
inline bool isPrintableChar(QChar c) { return konsole_wcwidth(c.unicode()) > 0; }

} // namespace Konsole
