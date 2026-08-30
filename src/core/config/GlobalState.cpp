// GlobalState.cpp — global constants and app state. See GlobalState.h.

#include "GlobalState.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutexLocker>
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
//
// 文件 I/O 刻意在锁外做，只有最后写回成员那一下持锁：读一个 JSON 文件可能耗上
// 毫秒级，持锁做它会让并发建连的工作线程（要读 ssh_timeout / 全局代理）跟着卡。
bool GlobalState::loadTheme(const QString &themeJsonPath, QString *errorOut)
{
    QFile f(themeJsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        // File does not exist yet — initialize with sensible defaults
        // (matches Python conf/theme.json structure).
        const QJsonObject defaults{
            {QStringLiteral("appearance"), QStringLiteral("dark")},
            {QStringLiteral("font"), QStringLiteral("Monaco")},
            {QStringLiteral("font_size"), 14},
            {QStringLiteral("language"), QStringLiteral("zh_CN")},
            {QStringLiteral("terminal_theme"), QStringLiteral("Ubuntu")},
        };
        QMutexLocker lock(&m_themeMutex);
        // Always remember the target path so saveTheme() can create the file
        // later even if the file does not yet exist (first-run scenario).
        m_themePath = themeJsonPath;
        m_theme = defaults;
        return true;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) *errorOut = QStringLiteral("invalid JSON in %1: %2")
                                      .arg(themeJsonPath, perr.errorString());
        // 路径仍要记下：解析失败也可能随后被 saveTheme 覆盖成一份好的。
        QMutexLocker lock(&m_themeMutex);
        m_themePath = themeJsonPath;
        return false;
    }
    QMutexLocker lock(&m_themeMutex);
    m_themePath = themeJsonPath;
    m_theme = doc.object();
    return true;
}

// 对应Python: function/theme.py::MainWindow._set_appearance (util.write_json 部分, indent=4)
//
// 同 loadTheme：先在锁内取一份快照，写盘在锁外做。
bool GlobalState::saveTheme(QString *errorOut) const
{
    QString path;
    QJsonObject snapshot;
    {
        QMutexLocker lock(&m_themeMutex);
        path = m_themePath;
        snapshot = m_theme;
    }
    if (path.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("no theme file loaded");
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    // QJsonDocument::Indented ≈ Python json.dump(indent=4); both remain
    // mutually parseable regardless of indentation width.
    f.write(QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(path);
        return false;
    }
    return true;
}

QJsonObject GlobalState::theme() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme;
}

void GlobalState::setTheme(const QJsonObject &theme)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme = theme;
}

// 对应Python: function/theme.py — str(data.get("appearance") or "dark").lower()
//
// 以下每个访问器都持 m_themeMutex（理由见 GlobalState.h 里那段注释）。
// 形态统一是"取锁 → 单次 value()/insert() → 放锁"，彼此不互相调用，故无递归风险。
QString GlobalState::appearance() const
{
    QMutexLocker lock(&m_themeMutex);
    const QString a = m_theme.value(QStringLiteral("appearance")).toString().toLower();
    return a.isEmpty() ? QStringLiteral("dark") : a;
}

void GlobalState::setAppearance(const QString &appearance)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("appearance"), appearance.toLower());
}

// 对应Python: conf/theme.json 的 "language" 字段 + language_manager 默认 zh_CN
QString GlobalState::language() const
{
    QMutexLocker lock(&m_themeMutex);
    const QString l = m_theme.value(QStringLiteral("language")).toString();
    return l.isEmpty() ? QStringLiteral("zh_CN") : l;
}

void GlobalState::setLanguage(const QString &langCode)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("language"), langCode);
}

QString GlobalState::fontFamily() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("font")).toString();
}

// 对应Python: function/theme.py — data.get('font_size', 14)
int GlobalState::fontSize() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("font_size")).toInt(14);
}

// 对应Python: function/theme.py::MainWindow.apply_font_settings (保存部分)
void GlobalState::setFont(const QString &family, int pointSize)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("font"), family);
    m_theme.insert(QStringLiteral("font_size"), pointSize);
}

// 对应Python: cube-shell.py::zoom_in/zoom_out — util.THEME['font_size'] = size
void GlobalState::setFontSize(int pointSize)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("font_size"), pointSize);
}

// 对应Python: cube-shell.py — (util.THEME or {}).get("terminal_theme", "Ubuntu")
QString GlobalState::terminalTheme() const
{
    QMutexLocker lock(&m_themeMutex);
    const QString t = m_theme.value(QStringLiteral("terminal_theme")).toString();
    return t.isEmpty() ? QStringLiteral("Ubuntu") : t;
}

void GlobalState::setTerminalTheme(const QString &name)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("terminal_theme"), name);
}

// C++ 侧新增键；Python 版读不到该键时按自己的默认走，互不影响。
int GlobalState::scrollbackLines() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("scrollback_lines")).toInt(10000);
}

void GlobalState::setScrollbackLines(int lines)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("scrollback_lines"), lines);
}

// C++ 侧新增键；缺省 true —— 保持加开关之前"无条件启用补全"的行为不变。
bool GlobalState::commandCompletionEnabled() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("command_completion")).toBool(true);
}

void GlobalState::setCommandCompletionEnabled(bool enabled)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("command_completion"), enabled);
}

// 键早已被设置页双写进 theme.json，这里只是补上读侧（见 GlobalState.h 注释）。
// 缺省 15 与 SettingsDialog::loadCurrentSettings 里的 toInt(15) 一致。
int GlobalState::sshConnectTimeoutSeconds() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("ssh_timeout")).toInt(15);
}

void GlobalState::setSshConnectTimeoutSeconds(int seconds)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("ssh_timeout"), seconds);
}

int GlobalState::hostKeyVerification() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("host_key_verification")).toInt(1); // default Ask
}

void GlobalState::setHostKeyVerification(int mode)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("host_key_verification"), mode);
}

bool GlobalState::sshKeepaliveEnabled() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("ssh_keepalive_enabled")).toBool(true);
}

void GlobalState::setSshKeepaliveEnabled(bool enabled)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("ssh_keepalive_enabled"), enabled);
}

int GlobalState::sshKeepaliveIntervalSeconds() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("ssh_keepalive_interval_sec")).toInt(30);
}

void GlobalState::setSshKeepaliveIntervalSeconds(int seconds)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("ssh_keepalive_interval_sec"), seconds);
}

int GlobalState::sshKeepaliveGraceSeconds() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("ssh_keepalive_grace_sec")).toInt(60);
}

void GlobalState::setSshKeepaliveGraceSeconds(int seconds)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("ssh_keepalive_grace_sec"), seconds);
}

// --- 会话日志录制 ---

QString GlobalState::sessionLogDir() const
{
    QMutexLocker lock(&m_themeMutex);
    const QString d = m_theme.value(QStringLiteral("session_log_dir")).toString();
    return d.isEmpty() ? QString() : d;   // 空 = 用默认 dataDir()/session-logs
}

void GlobalState::setSessionLogDir(const QString &dir)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("session_log_dir"), dir);
}

bool GlobalState::sessionLogAutoName() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("session_log_auto_name")).toBool(true);
}

void GlobalState::setSessionLogAutoName(bool enabled)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("session_log_auto_name"), enabled);
}

bool GlobalState::sessionLogTimestamps() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("session_log_timestamps")).toBool(false);
}

void GlobalState::setSessionLogTimestamps(bool enabled)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("session_log_timestamps"), enabled);
}

int GlobalState::sessionLogMaxMB() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("session_log_max_mb")).toInt(0); // 0=不轮转
}

void GlobalState::setSessionLogMaxMB(int mb)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("session_log_max_mb"), mb);
}

int GlobalState::sessionLogBackupCount() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_theme.value(QStringLiteral("session_log_backup_count")).toInt(5);
}

void GlobalState::setSessionLogBackupCount(int count)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("session_log_backup_count"), count);
}

// 全局代理。ProxyConfig 自带 JSON 读写，键名平铺（proxyType/proxyHost/...），
// 与 devices.json 里设备自己那份代理配置**用同一组键名**——两处存的是同一种
// 东西，键名一致才能让"把全局代理复制到某台设备"这类操作是纯搬运。
//
// 缺 proxyType 键时 fromJson 给出 ProxyType::None，即"不走代理"，
// 与加这个功能之前的行为逐字节一致。
ProxyConfig GlobalState::sshProxyConfig() const
{
    QMutexLocker lock(&m_themeMutex);
    return ProxyConfig::fromJson(m_theme);
}

void GlobalState::setSshProxyConfig(const ProxyConfig &proxy)
{
    QMutexLocker lock(&m_themeMutex);
    proxy.writeJson(m_theme);
}

// 与上面两个共用同一把锁，理由同样是"建连在工作线程上读、设置页在 UI 线程写"。
// 存的是 QString 而不是塞进 m_theme：m_theme 会被 saveTheme() 整个写进
// theme.json，明文口令绝不能走那条路（见 setSshProxyPassword 的声明处）。
void GlobalState::setSshProxyPassword(const QString &password)
{
    QMutexLocker lock(&m_themeMutex);
    m_sshProxyPassword = password;
}

QString GlobalState::sshProxyPassword() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_sshProxyPassword;
}

// 跳板机凭据快照。与上面那把口令共用 m_themeMutex：读者同样是工作线程
// （建链的 SshJumpChain），写者同样是 UI 线程（设备增删改之后重推一次）。
// QList 是 COW 且引用计数非原子，所以进出都必须在锁里做一次真正的赋值/复制。
void GlobalState::setJumpHostCatalog(const QList<DeviceEntry> &hosts)
{
    QMutexLocker lock(&m_themeMutex);
    m_jumpHostCatalog = hosts;
}

QList<DeviceEntry> GlobalState::jumpHostCatalog() const
{
    QMutexLocker lock(&m_themeMutex);
    return m_jumpHostCatalog;
}

// C++ 侧新增键；缺省回退平台默认（macOS 15，其它 14），与设备列表历史硬编码一致。
int GlobalState::deviceListFontSize() const
{
    QMutexLocker lock(&m_themeMutex);
#ifdef Q_OS_MACOS
    return m_theme.value(QStringLiteral("device_list_font_size")).toInt(15);
#else
    return m_theme.value(QStringLiteral("device_list_font_size")).toInt(14);
#endif
}

void GlobalState::setDeviceListFontSize(int pointSize)
{
    QMutexLocker lock(&m_themeMutex);
    m_theme.insert(QStringLiteral("device_list_font_size"), pointSize);
}

} // namespace cubeshell
