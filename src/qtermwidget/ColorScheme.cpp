// ColorScheme.cpp — C++ port of qtermwidget/color_scheme.py
//
// Represents a color scheme for the terminal display, plus the
// ColorSchemeManager singleton which loads / caches .colorscheme files.
// Ported from the Python PySide6 version, which was itself converted from
// Konsole / QTermWidget.
//
// Original copyright:
//   Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

#include "ColorScheme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>

#include "tools.h"

namespace Konsole {

// 对应C++: const ColorEntry ColorScheme::defaultTable[TABLE_COLORS]
const ColorEntry ColorScheme::defaultTable[TABLE_COLORS] = {
    // The following are almost IBM standard color codes, with some slight
    // gamma correction for the dim colors to compensate for bright X screens.
    // It contains the 8 ansiterm/xterm colors in 2 intensities.

    // Fixme: could add faint colors here, also.

    // normal
    ColorEntry(QColor(0x00, 0x00, 0x00), false), // Dfore
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), true),  // Dback
    ColorEntry(QColor(0x00, 0x00, 0x00), false), // Black
    ColorEntry(QColor(0xB2, 0x18, 0x18), false), // Red
    ColorEntry(QColor(0x18, 0xB2, 0x18), false), // Green
    ColorEntry(QColor(0xB2, 0x68, 0x18), false), // Yellow
    ColorEntry(QColor(0x18, 0x18, 0xB2), false), // Blue
    ColorEntry(QColor(0xB2, 0x18, 0xB2), false), // Magenta
    ColorEntry(QColor(0x18, 0xB2, 0xB2), false), // Cyan
    ColorEntry(QColor(0xB2, 0xB2, 0xB2), false), // White
    // intensiv
    ColorEntry(QColor(0x00, 0x00, 0x00), false), // Dfore
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), true),  // Dback
    ColorEntry(QColor(0x68, 0x68, 0x68), false), // Black
    ColorEntry(QColor(0xFF, 0x54, 0x54), false), // Red
    ColorEntry(QColor(0x54, 0xFF, 0x54), false), // Green
    ColorEntry(QColor(0xFF, 0xFF, 0x54), false), // Yellow
    ColorEntry(QColor(0x54, 0x54, 0xFF), false), // Blue
    ColorEntry(QColor(0xFF, 0x54, 0xFF), false), // Magenta
    ColorEntry(QColor(0x54, 0xFF, 0xFF), false), // Cyan
    ColorEntry(QColor(0xFF, 0xFF, 0xFF), false)  // White
};

// 对应C++: const char* const ColorScheme::colorNames[TABLE_COLORS]
const char *const ColorScheme::colorNames[TABLE_COLORS] = {
    "Foreground",
    "Background",
    "Color0",
    "Color1",
    "Color2",
    "Color3",
    "Color4",
    "Color5",
    "Color6",
    "Color7",
    "ForegroundIntense",
    "BackgroundIntense",
    "Color0Intense",
    "Color1Intense",
    "Color2Intense",
    "Color3Intense",
    "Color4Intense",
    "Color5Intense",
    "Color6Intense",
    "Color7Intense"
};

// 对应C++: const char* const ColorScheme::translatedColorNames[TABLE_COLORS]
// (Python 版翻译为中文显示字符串;此处直接用 UTF-8 中文,无 QObject 上下文)
const char *const ColorScheme::translatedColorNames[TABLE_COLORS] = {
    "前景色",
    "背景色",
    "颜色 1",
    "颜色 2",
    "颜色 3",
    "颜色 4",
    "颜色 5",
    "颜色 6",
    "颜色 7",
    "颜色 8",
    "前景色 (高亮)",
    "背景色 (高亮)",
    "颜色 1 (高亮)",
    "颜色 2 (高亮)",
    "颜色 3 (高亮)",
    "颜色 4 (高亮)",
    "颜色 5 (高亮)",
    "颜色 6 (高亮)",
    "颜色 7 (高亮)",
    "颜色 8 (高亮)"
};

// 对应C++: ColorScheme::ColorScheme()
ColorScheme::ColorScheme()
    : _opacity(1.0)
    , _table(nullptr)
    , _randomTable(nullptr)
{
}

// 对应C++: ColorScheme::ColorScheme(const ColorScheme& other)
ColorScheme::ColorScheme(const ColorScheme &other)
    : _description(other._description)
    , _name(other._name)
    , _opacity(other._opacity)
    , _table(nullptr)
    , _randomTable(nullptr)
{
    if (other._table != nullptr) {
        for (int i = 0; i < TABLE_COLORS; i++)
            setColorTableEntry(i, other._table->at(i));
    }

    if (other._randomTable != nullptr) {
        for (int i = 0; i < TABLE_COLORS; i++) {
            const RandomizationRange &range = other._randomTable->at(i);
            setRandomizationRange(i, range.hue, range.saturation, range.value);
        }
    }
}

ColorScheme &ColorScheme::operator=(const ColorScheme &other)
{
    if (this == &other)
        return *this;

    _description = other._description;
    _name = other._name;
    _opacity = other._opacity;

    delete _table;
    delete _randomTable;
    _table = nullptr;
    _randomTable = nullptr;

    if (other._table != nullptr) {
        for (int i = 0; i < TABLE_COLORS; i++)
            setColorTableEntry(i, other._table->at(i));
    }

    if (other._randomTable != nullptr) {
        for (int i = 0; i < TABLE_COLORS; i++) {
            const RandomizationRange &range = other._randomTable->at(i);
            setRandomizationRange(i, range.hue, range.saturation, range.value);
        }
    }

    return *this;
}

ColorScheme::~ColorScheme()
{
    delete _table;
    delete _randomTable;
}

void ColorScheme::setDescription(const QString &description) { _description = description; }
QString ColorScheme::description() const { return _description; }

void ColorScheme::setName(const QString &name) { _name = name; }
QString ColorScheme::name() const { return _name; }

void ColorScheme::setColorTableEntry(int index, const ColorEntry &entry)
{
    Q_ASSERT(index >= 0 && index < TABLE_COLORS);

    if (_table == nullptr) {
        _table = new QVector<ColorEntry>();
        for (int i = 0; i < TABLE_COLORS; i++)
            _table->append(defaultTable[i]);
    }

    (*_table)[index] = entry;
}

ColorEntry ColorScheme::colorEntry(int index) const
{
    Q_ASSERT(index >= 0 && index < TABLE_COLORS);

    ColorEntry entry = colorTable()[index];

    if ((_randomTable != nullptr) && !_randomTable->at(index).isNull()) {
        const RandomizationRange &range = _randomTable->at(index);

        int hueDifference = range.hue == 0
                ? 0
                : QRandomGenerator::global()->bounded(range.hue) - range.hue / 2;
        int saturationDifference = range.saturation == 0
                ? 0
                : QRandomGenerator::global()->bounded(range.saturation) - range.saturation / 2;
        int valueDifference = range.value == 0
                ? 0
                : QRandomGenerator::global()->bounded(range.value) - range.value / 2;

        QColor color = entry.color;

        int newHue = qAbs((color.hue() + hueDifference) % MAX_HUE);
        int newValue = qMin(qAbs(color.value() + valueDifference), 255);
        int newSaturation = qMin(qAbs(color.saturation() + saturationDifference), 255);

        color.setHsv(newHue, newSaturation, newValue);

        entry.color = color;
    }

    return entry;
}

void ColorScheme::getColorTable(ColorEntry *table) const
{
    for (int i = 0; i < TABLE_COLORS; i++)
        table[i] = colorEntry(i);
}

bool ColorScheme::randomizedBackgroundColor() const
{
    return _randomTable != nullptr && !_randomTable->at(1).isNull();
}

void ColorScheme::setRandomizedBackgroundColor(bool randomize)
{
    if (randomize) {
        // The hue of the background color is allowed to be randomly
        // adjusted as much as possible.
        //
        // The value and saturation are left alone to maintain readability.
        setRandomizationRange(1 /* background color index */, MAX_HUE, 255, 0);
    } else {
        if (_randomTable != nullptr)
            setRandomizationRange(1 /* background color index */, 0, 0, 0);
    }
}

void ColorScheme::setRandomizationRange(int index, quint16 hue, quint8 saturation, quint8 value)
{
    Q_ASSERT(hue <= MAX_HUE);
    Q_ASSERT(index >= 0 && index < TABLE_COLORS);

    if (_randomTable == nullptr) {
        _randomTable = new QVector<RandomizationRange>();
        for (int i = 0; i < TABLE_COLORS; i++)
            _randomTable->append(RandomizationRange());
    }

    (*_randomTable)[index].hue = hue;
    (*_randomTable)[index].value = value;
    (*_randomTable)[index].saturation = saturation;
}

const ColorEntry *ColorScheme::colorTable() const
{
    if (_table != nullptr)
        return _table->constData();
    else
        return defaultTable;
}

QColor ColorScheme::foregroundColor() const
{
    return colorTable()[0].color;
}

QColor ColorScheme::backgroundColor() const
{
    return colorTable()[1].color;
}

bool ColorScheme::hasDarkBackground() const
{
    // value can be in the range 0-255
    return backgroundColor().value() < 127;
}

void ColorScheme::setOpacity(qreal opacity) { _opacity = opacity; }
qreal ColorScheme::opacity() const { return _opacity; }

void ColorScheme::read(const QString &fileName)
{
    QSettings settings(fileName, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("General"));

    _description = settings.value(QStringLiteral("Description"),
                                  QStringLiteral("Un-named Color Scheme")).toString();
    _opacity = settings.value(QStringLiteral("Opacity"), qreal(1.0)).toReal();

    settings.endGroup();

    for (int i = 0; i < TABLE_COLORS; i++)
        readColorEntry(&settings, i);
}

void ColorScheme::readColorEntry(QSettings *s, int index)
{
    QString colorName = colorNameForIndex(index);

    s->beginGroup(colorName);

    ColorEntry entry;

    QVariant colorValue = s->value(QStringLiteral("Color"));
    QString colorStr;
    int r, g, b;
    bool ok = false;
    // XXX: Undocumented(?) QSettings behavior: values with commas are parsed
    // as QStringList and others QString
    if (colorValue.userType() == QMetaType::QStringList) {
        QStringList rgbList = colorValue.toStringList();
        colorStr = rgbList.join(QLatin1Char(','));
        if (rgbList.count() == 3) {
            bool parse_ok;

            ok = true;
            r = rgbList[0].toInt(&parse_ok);
            ok = ok && parse_ok;
            g = rgbList[1].toInt(&parse_ok);
            ok = ok && parse_ok;
            b = rgbList[2].toInt(&parse_ok);
            ok = ok && parse_ok;

            // check if the color is within a valid range
            ok = ok && qBound(0, r, 255) == r && qBound(0, g, 255) == g
                 && qBound(0, b, 255) == b;
        }
    } else {
        colorStr = colorValue.toString();
        r = 0, g = 0, b = 0;
        QRegularExpression hexColorPattern(QStringLiteral("^#[0-9a-f]{6}$"),
                                           QRegularExpression::CaseInsensitiveOption);
        if (hexColorPattern.match(colorStr).hasMatch()) {
            // Parsing is always ok as already matched by the regexp
            r = colorStr.mid(1, 2).toInt(nullptr, 16);
            g = colorStr.mid(3, 2).toInt(nullptr, 16);
            b = colorStr.mid(5, 2).toInt(nullptr, 16);
            ok = true;
        }
    }

    if (!ok) {
        r = g = b = 0;
        qCWarning(qtermwidgetLogger) << "Invalid color value " << colorStr
                                     << " for " << colorName << ". Fallback to black.";
    }

    entry.color = QColor(r, g, b);
    entry.transparent = s->value(QStringLiteral("Transparent"), false).toBool();

    // Deprecated key for backwards compatibility
    if (s->contains(QStringLiteral("Bold"))) {
        const bool isBold = s->value(QStringLiteral("Bold")).toBool();
        entry.fontWeight = isBold ? ColorEntry::Bold : ColorEntry::UseCurrentFormat;
    }

    quint16 hue = quint16(s->value(QStringLiteral("MaxRandomHue"), 0).toInt());
    quint8 value = quint8(s->value(QStringLiteral("MaxRandomValue"), 0).toInt());
    quint8 saturation = quint8(s->value(QStringLiteral("MaxRandomSaturation"), 0).toInt());

    setColorTableEntry(index, entry);

    if (hue != 0 || value != 0 || saturation != 0)
        setRandomizationRange(index, hue, saturation, value);

    s->endGroup();
}

QString ColorScheme::colorNameForIndex(int index)
{
    Q_ASSERT(index >= 0 && index < TABLE_COLORS);
    return QString::fromLatin1(colorNames[index]);
}

QString ColorScheme::translatedColorNameForIndex(int index)
{
    Q_ASSERT(index >= 0 && index < TABLE_COLORS);
    return QString::fromUtf8(translatedColorNames[index]);
}

// 对应C++: AccessibleColorScheme::AccessibleColorScheme()
AccessibleColorScheme::AccessibleColorScheme()
    : ColorScheme()
{
    // basic attributes
    setName(QStringLiteral("accessible"));
    setDescription(QString::fromUtf8("可访问颜色方案"));

    // It's not finished in Konsole, so why bother
#if 0
    // set up defaults
    const ColorEntry *table = KColorScheme(QPalette::Active, KColorScheme::View).colors();

    setColorTableEntry(...)
#endif
}

// 对应C++: const ColorScheme ColorSchemeManager::_defaultColorScheme;
const ColorScheme ColorSchemeManager::_defaultColorScheme;

// 对应C++: ColorSchemeManager::ColorSchemeManager()
ColorSchemeManager::ColorSchemeManager()
    : _haveLoadedAll(false)
{
}

// 对应C++: ColorSchemeManager::~ColorSchemeManager()
ColorSchemeManager::~ColorSchemeManager()
{
    qDeleteAll(_colorSchemes);
    _colorSchemes.clear();
}

// 对应C++: void ColorSchemeManager::loadAllColorSchemes()
void ColorSchemeManager::loadAllColorSchemes()
{
    int failed = 0;

    const QStringList nativeColorSchemes = listColorSchemes();
    for (const QString &schemePath : nativeColorSchemes) {
        if (!loadColorScheme(schemePath))
            failed++;
    }

    if (failed > 0)
        qCWarning(qtermwidgetLogger) << "failed to load " << failed << " color schemes.";

    _haveLoadedAll = true;
}

// 对应C++: bool ColorSchemeManager::loadColorScheme(const QString& filePath)
bool ColorSchemeManager::loadColorScheme(const QString &filePath)
{
    if (!filePath.endsWith(QLatin1String(".colorscheme")) || !QFile::exists(filePath))
        return false;

    QFileInfo info(filePath);

    const QString schemeName = info.baseName();

    ColorScheme *scheme = new ColorScheme();
    scheme->setName(schemeName);
    scheme->read(filePath);

    if (scheme->name().isEmpty()) {
        qCWarning(qtermwidgetLogger) << "Color scheme in" << filePath
                                     << "does not have a valid name and was not loaded.";
        delete scheme;
        return false;
    }

    if (!_colorSchemes.contains(schemeName)) {
        _colorSchemes.insert(schemeName, scheme);
    } else {
        qCWarning(qtermwidgetLogger) << "color scheme with name" << schemeName
                                     << "has already been found, ignoring.";
        delete scheme;
    }

    return true;
}

// 对应C++: QList<QString> ColorSchemeManager::listColorSchemes()
QStringList ColorSchemeManager::listColorSchemes()
{
    QStringList ret;
    const QStringList dirs = getColorSchemesDirs();
    for (const QString &schemeDir : dirs) {
        const QStringList fileNames = QDir(schemeDir).entryList(QStringList(QStringLiteral("*.colorscheme")));
        for (const QString &fileName : fileNames)
            ret.append(schemeDir + QLatin1Char('/') + fileName);
    }
    return ret;
}

// 对应C++: const ColorScheme* ColorSchemeManager::defaultColorScheme() const
const ColorScheme *ColorSchemeManager::defaultColorScheme() const
{
    return &_defaultColorScheme;
}

// 对应C++: bool ColorSchemeManager::deleteColorScheme(const QString& name)
bool ColorSchemeManager::deleteColorScheme(const QString &name)
{
    Q_ASSERT(_colorSchemes.contains(name));

    // lookup the path and delete
    QString path = findColorSchemePath(name);
    if (QFile::remove(path)) {
        _colorSchemes.remove(name);
        return true;
    } else {
        qCWarning(qtermwidgetLogger) << "Failed to remove color scheme -" << path;
        return false;
    }
}

// 对应C++: QString ColorSchemeManager::findColorSchemePath(const QString& name) const
QString ColorSchemeManager::findColorSchemePath(const QString &name) const
{
    const QStringList dirs = getColorSchemesDirs();

    if (dirs.isEmpty())
        return QString();

    const QStringList suffixes = {QStringLiteral(".colorscheme"), QStringLiteral(".schema")};
    for (const QString &dir : dirs) {
        for (const QString &suffix : suffixes) {
            QString path(dir + name + suffix);
            if (QFile::exists(path))
                return path;
        }
    }

    return QString();
}

// 对应C++: const ColorScheme* ColorSchemeManager::findColorScheme(const QString& name)
const ColorScheme *ColorSchemeManager::findColorScheme(const QString &name)
{
    if (name.isEmpty())
        return defaultColorScheme();

    if (_colorSchemes.contains(name))
        return _colorSchemes.value(name);
    else {
        // look for this color scheme
        QString path = findColorSchemePath(name);
        if (!path.isEmpty() && loadColorScheme(path))
            return findColorScheme(name);

        qCWarning(qtermwidgetLogger) << "Could not find color scheme - " << name;

        return nullptr;
    }
}

// 对应C++: QList<const ColorScheme*> ColorSchemeManager::allColorSchemes()
QList<const ColorScheme *> ColorSchemeManager::allColorSchemes()
{
    if (!_haveLoadedAll)
        loadAllColorSchemes();

    QList<const ColorScheme *> result;
    result.reserve(_colorSchemes.size());
    for (const ColorScheme *scheme : std::as_const(_colorSchemes))
        result.append(scheme);
    return result;
}

// 对应C++: bool ColorSchemeManager::loadCustomColorScheme(const QString& path)
bool ColorSchemeManager::loadCustomColorScheme(const QString &path)
{
    if (path.endsWith(QLatin1String(".colorscheme")))
        return loadColorScheme(path);

    return false;
}

// 对应C++: void ColorSchemeManager::addCustomColorSchemeDir(const QString& custom_dir)
void ColorSchemeManager::addCustomColorSchemeDir(const QString &customDir)
{
    // 调用 tools.h 中的同名自由函数(命名空间作用域限定以避免递归到本成员)
    Konsole::addCustomColorSchemeDir(customDir);
}

// 对应C++: Q_GLOBAL_STATIC(ColorSchemeManager, theColorSchemeManager)
// 对应C++: ColorSchemeManager* ColorSchemeManager::instance()
ColorSchemeManager *ColorSchemeManager::instance()
{
    static ColorSchemeManager theColorSchemeManager;
    return &theColorSchemeManager;
}

} // namespace Konsole
