#pragma once

// GlobalState.h — global constants, config/data directory resolution and
// application state (theme / language / appearance).
//
// 对应Python: core/vars.py (常量) + function/util.py (APP_NAME/THEME 全局)
//           + cube-shell.py::get_config_directory/get_config_path (路径逻辑)
//
// Path compatibility: the Python side uses
//     appdirs.user_config_dir("cube-shell", appauthor=False)
//     appdirs.user_data_dir("cube-shell", appauthor=False)
// which resolve to:
//     macOS   ~/Library/Application Support/cube-shell   (config == data)
//     Windows %LOCALAPPDATA%\cube-shell                  (config == data)
//     Linux   ~/.config/cube-shell  /  ~/.local/share/cube-shell
// The C++ implementation MUST produce the exact same on-disk paths so both
// versions share config.dat / groups.json / tunnel.json etc.

#include <QJsonObject>
#include <QString>

namespace cubeshell {

// 对应Python: core/vars.py 模块级常量与 KEYS/ICONS/CMDS 类
namespace vars {

// 对应Python: core/vars.py::CONF_FILE
inline constexpr const char CONF_FILE[] = "tunnel.json";

// 对应Python: function/util.py::APP_NAME
inline constexpr const char APP_NAME[] = "cube-shell";

// 对应Python: core/vars.py::KEYS
namespace keys {
inline constexpr const char TUNNEL_TYPE[]         = "tunnel_type";
inline constexpr const char BROWSER_OPEN[]        = "browser_open";
inline constexpr const char DEVICE_NAME[]         = "device_name";
inline constexpr const char SSH_ADDRESS[]         = "ssh_address";
inline constexpr const char SSH_PORT[]            = "ssh_port";
inline constexpr const char SSH_USERNAME[]        = "ssh_username";
inline constexpr const char SSH_PRIVATE_KEY[]     = "ssh_private_key";
inline constexpr const char REMOTE_BIND_ADDRESS[] = "remote_bind_address";
inline constexpr const char LOCAL_BIND_ADDRESS[]  = "local_bind_address";
} // namespace keys

// 对应Python: core/vars.py::ICONS (Qt resource paths)
namespace icons {
inline constexpr const char TUNNEL[]   = ":tunnel-diode.png";
inline constexpr const char START[]    = ":open.png";
inline constexpr const char STOP[]     = ":off.png";
inline constexpr const char KILL_SSH[] = ":on-off.png";
} // namespace icons

// 对应Python: core/vars.py::CMDS
namespace cmds {
inline constexpr const char SSH[]          = "ssh";
inline constexpr const char SSH_KILL_NIX[] = "pkill ssh";
inline constexpr const char SSH_KILL_WIN[] = "taskkill /im ssh.exe /t /f";
} // namespace cmds

} // namespace vars

// Application-wide state singleton (theme json mirrors util.THEME).
class GlobalState {
public:
    static GlobalState &instance();

    GlobalState(const GlobalState &) = delete;
    GlobalState &operator=(const GlobalState &) = delete;

    // --- path resolution (static, no instance state involved) ---

    // User config dir, created on demand (mkpath, like os.makedirs exist_ok).
    // 对应Python: cube-shell.py::get_config_directory / appdirs.user_config_dir
    static QString configDir();

    // User data dir (audit db, updates, automation…), created on demand.
    // 对应Python: appdirs.user_data_dir("cube-shell", appauthor=False)
    static QString dataDir();

    // Full path of a file inside configDir().
    // 对应Python: cube-shell.py::get_config_path
    static QString configFilePath(const QString &fileName);

    // Convenience: configDir()/tunnel.json (vars.CONF_FILE).
    static QString tunnelConfigPath();
    // Convenience: configDir()/groups.json (group_manager._get_groups_file_path).
    static QString groupsConfigPath();

    // --- theme / language state (conf/theme.json, util.THEME equivalent) ---

    // Load theme.json into memory; remembers the path for saveTheme().
    // 对应Python: function/theme.py::MainWindow._load_current_settings (读取部分)
    bool loadTheme(const QString &themeJsonPath, QString *errorOut = nullptr);
    // Persist the in-memory theme back to the loaded path (indent, UTF-8).
    // 对应Python: function/theme.py::MainWindow._set_appearance (写回部分)
    bool saveTheme(QString *errorOut = nullptr) const;

    QJsonObject theme() const { return m_theme; }
    void setTheme(const QJsonObject &theme) { m_theme = theme; }

    // "dark" | "light", defaults to "dark" (Python: data.get("appearance") or "dark").
    QString appearance() const;
    void setAppearance(const QString &appearance);

    // e.g. "zh_CN"; defaults to "zh_CN" like i18n/language_manager.py.
    QString language() const;
    void setLanguage(const QString &langCode);

    QString fontFamily() const;
    int fontSize() const;           // default 14
    void setFont(const QString &family, int pointSize);
    // 仅更新字号(滚轮缩放)。对应Python: util.THEME['font_size'] = size
    void setFontSize(int pointSize);

    // 终端配色方案名 (theme.json 的 "terminal_theme")，默认 "Ubuntu"。
    // 对应Python: cube-shell.py — (util.THEME or {}).get("terminal_theme", "Ubuntu")
    QString terminalTheme() const;
    void setTerminalTheme(const QString &name);

    // 终端回滚行数 (theme.json 的 "scrollback_lines")，默认 10000。
    // 查日志/搜索历史都吃这个缓冲：太小则 tail 几屏后就翻不回去，也搜不到。
    // 0 表示不保留历史，负数表示无限（落磁盘临时文件，见 setHistorySize）。
    int scrollbackLines() const;
    void setScrollbackLines(int lines);

private:
    GlobalState() = default;

    QJsonObject m_theme;
    QString m_themePath;
};

} // namespace cubeshell
