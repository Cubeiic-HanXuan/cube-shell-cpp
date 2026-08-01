#pragma once

// ColorScheme.h — C++ port of qtermwidget/color_scheme.py
//
// Represents a color scheme for the terminal display, plus the
// ColorSchemeManager singleton which loads / caches .colorscheme files.
// Ported from the Python PySide6 version, which was itself converted from
// Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include <QColor>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "CharacterColor.h"

class QSettings;

namespace Konsole {

// 对应C++: class ColorScheme
class ColorScheme {
public:
    // 对应C++: class RandomizationRange
    class RandomizationRange {
    public:
        // 对应C++: RandomizationRange() : hue(0) , saturation(0) , value(0) {}
        RandomizationRange()
            : hue(0), saturation(0), value(0) {}

        // 对应C++: bool isNull() const
        bool isNull() const { return hue == 0 && saturation == 0 && value == 0; }

        quint16 hue;
        quint8  saturation;
        quint8  value;
    };

    // 对应C++: ColorScheme::ColorScheme()
    ColorScheme();
    // 对应C++: ColorScheme::ColorScheme(const ColorScheme& other)
    ColorScheme(const ColorScheme &other);
    ColorScheme &operator=(const ColorScheme &other);
    ~ColorScheme();

    // 对应C++: void setDescription(const QString& description)
    void setDescription(const QString &description);
    // 对应C++: QString description() const
    QString description() const;

    // 对应C++: void setName(const QString& name)
    void setName(const QString &name);
    // 对应C++: QString name() const
    QString name() const;

    // 对应C++: void setColorTableEntry(int index , const ColorEntry& entry)
    void setColorTableEntry(int index, const ColorEntry &entry);

    // 对应C++: ColorEntry colorEntry(int index) const
    ColorEntry colorEntry(int index) const;

    // 对应C++: void getColorTable(ColorEntry* table) const
    void getColorTable(ColorEntry *table) const;

    // 对应C++: bool randomizedBackgroundColor() const
    bool randomizedBackgroundColor() const;
    // 对应C++: void setRandomizedBackgroundColor(bool randomize)
    void setRandomizedBackgroundColor(bool randomize);

    // 对应C++: void setRandomizationRange( int index , quint16 hue , quint8 saturation , quint8 value )
    void setRandomizationRange(int index, quint16 hue, quint8 saturation, quint8 value);

    // 对应C++: const ColorEntry* colorTable() const
    const ColorEntry *colorTable() const;

    // 对应C++: QColor foregroundColor() const
    QColor foregroundColor() const;
    // 对应C++: QColor backgroundColor() const
    QColor backgroundColor() const;
    // 对应C++: bool hasDarkBackground() const
    bool hasDarkBackground() const;

    // 对应C++: void setOpacity(qreal opacity)
    void setOpacity(qreal opacity);
    // 对应C++: qreal opacity() const
    qreal opacity() const;

    // 对应C++: void read(const QString & fileName)
    void read(const QString &fileName);

    // 对应C++: static QString colorNameForIndex(int index)
    static QString colorNameForIndex(int index);
    // 对应C++: static QString translatedColorNameForIndex(int index)
    static QString translatedColorNameForIndex(int index);

    // 对应C++: static const quint16 MAX_HUE = 340;
    static constexpr quint16 MAX_HUE = 340;

private:
    void readColorEntry(QSettings *s, int index);

    QString _description;
    QString _name;
    qreal   _opacity;

    // Lazily-allocated tables (nullptr until a custom entry / range is set).
    QVector<ColorEntry>        *_table;
    QVector<RandomizationRange> *_randomTable;

    // 对应C++: static const ColorEntry defaultTable[TABLE_COLORS]
    static const ColorEntry defaultTable[TABLE_COLORS];
    // 对应C++: static const char* const colorNames[TABLE_COLORS]
    static const char *const colorNames[TABLE_COLORS];
    // 对应C++: static const char* const translatedColorNames[TABLE_COLORS]
    static const char *const translatedColorNames[TABLE_COLORS];
};

// 对应C++: class AccessibleColorScheme : public ColorScheme
class AccessibleColorScheme : public ColorScheme {
public:
    // 对应C++: AccessibleColorScheme::AccessibleColorScheme()
    AccessibleColorScheme();
};

// 对应C++: class ColorSchemeManager
class ColorSchemeManager {
public:
    // 对应C++: ColorSchemeManager::ColorSchemeManager()
    ColorSchemeManager();
    // 对应C++: ColorSchemeManager::~ColorSchemeManager()
    ~ColorSchemeManager();

    // 对应C++: const ColorScheme* defaultColorScheme() const
    const ColorScheme *defaultColorScheme() const;

    // 对应C++: const ColorScheme* findColorScheme(const QString& name)
    const ColorScheme *findColorScheme(const QString &name);

    // 对应C++: bool deleteColorScheme(const QString& name)
    bool deleteColorScheme(const QString &name);

    // 对应C++: QList<const ColorScheme*> allColorSchemes()
    QList<const ColorScheme *> allColorSchemes();

    // 对应C++: bool loadCustomColorScheme(const QString& path)
    bool loadCustomColorScheme(const QString &path);

    // 对应C++: void addCustomColorSchemeDir(const QString& custom_dir)
    void addCustomColorSchemeDir(const QString &customDir);

    // 对应C++: static ColorSchemeManager* instance()
    static ColorSchemeManager *instance();

private:
    void loadAllColorSchemes();
    bool loadColorScheme(const QString &filePath);
    QStringList listColorSchemes();
    QString findColorSchemePath(const QString &name) const;

    QHash<QString, ColorScheme *> _colorSchemes;
    bool _haveLoadedAll;

    // 对应C++: static const ColorScheme _defaultColorScheme;
    static const ColorScheme _defaultColorScheme;
};

} // namespace Konsole
