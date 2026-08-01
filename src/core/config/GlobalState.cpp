// GlobalState.cpp — global constants and app state. See GlobalState.h.

#include "GlobalState.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace cubeshell {

// Resolve the per-user base dir matching Python appdirs semantics.
//   macOS:   both config and data map to ~/Library/Application Support
//   Windows: both map to %LOCALAPPDATA%
//   Linux:   config -> $XDG_CONFIG_HOME (~/.config), data -> ~/.local/share
// QStandardPaths::GenericConfigLocation on macOS is ~/Library/Preferences,
// which does NOT match appdirs — hence the platform switch below.
static QString appdirsBase(bool wantData)
{
#if defined(Q_OS_MACOS)
    Q_UNUSED(wantData);
    // appdirs: ~/Library/Application Support (config == data on macOS)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#elif defined(Q_OS_WIN)
    Q_UNUSED(wantData);
    // appdirs with appauthor=False, roaming=False: %LOCALAPPDATA%
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#else
    // Linux/XDG: ~/.config vs ~/.local/share
    return QStandardPaths::writableLocation(
        wantData ? QStandardPaths::GenericDataLocation
                 : QStandardPaths::GenericConfigLocation);
#endif
}

GlobalState &GlobalState::instance()
{
    static GlobalState s;
    return s;
}

// 对应Python: cube-shell.py::get_config_directory (appdirs.user_config_dir + makedirs)
QString GlobalState::configDir()
{
    const QString dir = appdirsBase(false) + QLatin1Char('/')
                        + QLatin1String(vars::APP_NAME);
    QDir().mkpath(dir); // os.makedirs(config_dir, exist_ok=True)
    return dir;
}

// 对应Python: appdirs.user_data_dir("cube-shell", appauthor=False) 用法
// (core/ai/audit.py::AuditLogger.__init__, core/update/platform_match.py 等)
QString GlobalState::dataDir()
{
    const QString dir = appdirsBase(true) + QLatin1Char('/')
                        + QLatin1String(vars::APP_NAME);
    QDir().mkpath(dir);
    return dir;
}

// 对应Python: cube-shell.py::get_config_path
QString GlobalState::configFilePath(const QString &fileName)
{
    return configDir() + QLatin1Char('/') + fileName;
}

QString GlobalState::tunnelConfigPath()
{
    return configFilePath(QLatin1String(vars::CONF_FILE));
}

// 对应Python: core/group_manager.py::_get_groups_file_path
QString GlobalState::groupsConfigPath()
{
    return configFilePath(QStringLiteral("groups.json"));
}

// 对应Python: function/theme.py::MainWindow._load_current_settings (util.read_json 部分)
bool GlobalState::loadTheme(const QString &themeJsonPath, QString *errorOut)
{
    // Always remember the target path so saveTheme() can create the file later
    // even if the file does not yet exist (first-run scenario).
    m_themePath = themeJsonPath;

    QFile f(themeJsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        // File does not exist yet — initialize with sensible defaults
        // (matches Python conf/theme.json structure).
        m_theme = QJsonObject{
            {QStringLiteral("appearance"), QStringLiteral("dark")},
            {QStringLiteral("font"), QStringLiteral("Monaco")},
            {QStringLiteral("font_size"), 14},
            {QStringLiteral("language"), QStringLiteral("zh_CN")},
            {QStringLiteral("terminal_theme"), QStringLiteral("Ubuntu")},
        };
        return true;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) *errorOut = QStringLiteral("invalid JSON in %1: %2")
                                      .arg(themeJsonPath, perr.errorString());
        return false;
    }
    m_theme = doc.object();
    return true;
}

// 对应Python: function/theme.py::MainWindow._set_appearance (util.write_json 部分, indent=4)
bool GlobalState::saveTheme(QString *errorOut) const
{
    if (m_themePath.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("no theme file loaded");
        return false;
    }
    QSaveFile f(m_themePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(m_themePath);
        return false;
    }
    // QJsonDocument::Indented ≈ Python json.dump(indent=4); both remain
    // mutually parseable regardless of indentation width.
    f.write(QJsonDocument(m_theme).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(m_themePath);
        return false;
    }
    return true;
}

// 对应Python: function/theme.py — str(data.get("appearance") or "dark").lower()
QString GlobalState::appearance() const
{
    const QString a = m_theme.value(QStringLiteral("appearance")).toString().toLower();
    return a.isEmpty() ? QStringLiteral("dark") : a;
}

void GlobalState::setAppearance(const QString &appearance)
{
    m_theme.insert(QStringLiteral("appearance"), appearance.toLower());
}

// 对应Python: conf/theme.json 的 "language" 字段 + language_manager 默认 zh_CN
QString GlobalState::language() const
{
    const QString l = m_theme.value(QStringLiteral("language")).toString();
    return l.isEmpty() ? QStringLiteral("zh_CN") : l;
}

void GlobalState::setLanguage(const QString &langCode)
{
    m_theme.insert(QStringLiteral("language"), langCode);
}

QString GlobalState::fontFamily() const
{
    return m_theme.value(QStringLiteral("font")).toString();
}

// 对应Python: function/theme.py — data.get('font_size', 14)
int GlobalState::fontSize() const
{
    return m_theme.value(QStringLiteral("font_size")).toInt(14);
}

// 对应Python: function/theme.py::MainWindow.apply_font_settings (保存部分)
void GlobalState::setFont(const QString &family, int pointSize)
{
    m_theme.insert(QStringLiteral("font"), family);
    m_theme.insert(QStringLiteral("font_size"), pointSize);
}

// 对应Python: cube-shell.py::zoom_in/zoom_out — util.THEME['font_size'] = size
void GlobalState::setFontSize(int pointSize)
{
    m_theme.insert(QStringLiteral("font_size"), pointSize);
}

// 对应Python: cube-shell.py — (util.THEME or {}).get("terminal_theme", "Ubuntu")
QString GlobalState::terminalTheme() const
{
    const QString t = m_theme.value(QStringLiteral("terminal_theme")).toString();
    return t.isEmpty() ? QStringLiteral("Ubuntu") : t;
}

void GlobalState::setTerminalTheme(const QString &name)
{
    m_theme.insert(QStringLiteral("terminal_theme"), name);
}

} // namespace cubeshell
