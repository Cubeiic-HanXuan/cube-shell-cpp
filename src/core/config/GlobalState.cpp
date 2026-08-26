// GlobalState.cpp — global constants and app state. See GlobalState.h.

#include "GlobalState.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "ConfigUtil.h"

namespace cubeshell {

// Resolve the per-user base dir matching Python appdirs semantics.
//   macOS:   both config and data map to ~/Library/Application Support
//   Windows: both map to %LOCALAPPDATA%
//   Linux:   config -> $XDG_CONFIG_HOME (~/.config), data -> ~/.local/share
//   OHOS:    sandbox AppConfigLocation / AppDataLocation
//            (/data/app/el2/<user>/base/<bundle>/haps/entry/files/...)
// QStandardPaths::GenericConfigLocation on macOS is ~/Library/Preferences,
// which does NOT match appdirs — hence the platform switch below.
static QString appdirsBase(bool wantData)
{
#if defined(CUBESHELL_PLATFORM_OHOS)
    // 鸿蒙：Generic*Location 在沙箱里解析为空串/沙箱外路径，配置会写丢
    // （问题1「重启后找不到配置文件」的根因）。
    //
    // 注意：不能用 AppConfigLocation——Qt 把它映射到鸿蒙的
    //   .../preferences/cube-shell
    // 目录，而 preferences/ 是鸿蒙给「轻量 KV 偏好存储(Preferences 数据库)」
    // 预留的目录，不适合放 devices.json/theme.json/groups.json 这类 JSON 文档。
    // AppDataLocation 映射到应用文件目录 files/，才是放文档的正确位置。
    // config 与 data 共用它（同 macOS/Windows 的 appdirs 语义：config == data）。
    Q_UNUSED(wantData);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#elif defined(Q_OS_MACOS)
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
#if defined(CUBESHELL_PLATFORM_OHOS)
    // 鸿蒙：AppDataLocation 已含应用名(.../files/cube-shell)，直接用即可
    // （不再追加 /cube-shell）。
    const QString dir = appdirsBase(false);
#else
    const QString dir = appdirsBase(false) + QLatin1Char('/')
                        + QLatin1String(vars::APP_NAME);
#endif
    QDir().mkpath(dir); // os.makedirs(config_dir, exist_ok=True)
    return dir;
}

// 对应Python: appdirs.user_data_dir("cube-shell", appauthor=False) 用法
// (core/ai/audit.py::AuditLogger.__init__, core/update/platform_match.py 等)
QString GlobalState::dataDir()
{
#if defined(CUBESHELL_PLATFORM_OHOS)
    const QString dir = appdirsBase(true);
#else
    const QString dir = appdirsBase(true) + QLatin1Char('/')
                        + QLatin1String(vars::APP_NAME);
#endif
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

QStringList GlobalState::hardenConfigPermissions()
{
    // devices.json —— 设备清单（迁移完成前还含明文密码）
    // config.dat   —— Python 版写的 pickle，明文密码，从未设过权限
    // tunnel.json  —— 内网穿透/跳板拓扑与账号
    // *.plain.bak  —— 迁移期的明文备份（由迁移流程自己写成 0600，这里兜底）
    static const char *const kSensitive[] = {
        "devices.json", "config.dat", "tunnel.json", "devices.json.plain.bak",
    };
    QStringList failed;
    for (const char *name : kSensitive) {
        const QString path = configFilePath(QLatin1String(name));
        if (!QFileInfo::exists(path))
            continue;
        if (!ConfigUtil::restrictPermissions(path))
            failed << path;
    }
    return failed;
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

// C++ 侧新增键；Python 版读不到该键时按自己的默认走，互不影响。
int GlobalState::scrollbackLines() const
{
    return m_theme.value(QStringLiteral("scrollback_lines")).toInt(10000);
}

void GlobalState::setScrollbackLines(int lines)
{
    m_theme.insert(QStringLiteral("scrollback_lines"), lines);
}

// C++ 侧新增键；缺省 true —— 保持加开关之前"无条件启用补全"的行为不变。
bool GlobalState::commandCompletionEnabled() const
{
    return m_theme.value(QStringLiteral("command_completion")).toBool(true);
}

void GlobalState::setCommandCompletionEnabled(bool enabled)
{
    m_theme.insert(QStringLiteral("command_completion"), enabled);
}

// C++ 侧新增键；缺省回退平台默认（macOS 15，其它 14），与设备列表历史硬编码一致。
int GlobalState::deviceListFontSize() const
{
#ifdef Q_OS_MACOS
    return m_theme.value(QStringLiteral("device_list_font_size")).toInt(15);
#else
    return m_theme.value(QStringLiteral("device_list_font_size")).toInt(14);
#endif
}

void GlobalState::setDeviceListFontSize(int pointSize)
{
    m_theme.insert(QStringLiteral("device_list_font_size"), pointSize);
}

} // namespace cubeshell
