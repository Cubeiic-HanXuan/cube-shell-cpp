#pragma once

// tools.h — C++ port of qtermwidget/tools.py
//
// Keyboard-layout / color-scheme directory management and the qtermwidget
// logging category. Ported from the Python version (converted from tools.cpp).

#include <QLoggingCategory>
#include <QString>
#include <QStringList>

namespace Konsole {

// 对应C++: Q_LOGGING_CATEGORY(qtermwidgetLogger, "qtermwidget", QtWarningMsg)
Q_DECLARE_LOGGING_CATEGORY(qtermwidgetLogger)

// Helper to find the keyboard layout files.
// 对应C++: QString get_kb_layout_dir()
QString getKbLayoutDir();

// Adds a custom location for color schemes.
// 对应C++: void add_custom_color_scheme_dir(const QString& custom_dir)
void addCustomColorSchemeDir(const QString &customDir);

// Returns the list of directories that may contain color schemes.
// 对应C++: const QStringList get_color_schemes_dirs()
QStringList getColorSchemesDirs();

// Python-added conveniences (no upstream C++ counterpart) kept for parity.
void clearCustomColorSchemeDirs();
QStringList getCustomColorSchemeDirs();

} // namespace Konsole
