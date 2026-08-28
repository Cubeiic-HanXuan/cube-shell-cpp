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
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "net/ProxyConfig.h"
// DeviceEntry：跳板机凭据快照就是一串它（见 setJumpHostCatalog）。
// 反向不成立——DeviceConfigStore.h 不认识 GlobalState，所以没有循环包含。
#include "DeviceConfigStore.h"

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

    // 把配置目录里含凭据的文件权限收敛到 0600（幂等，启动时调一次）。
    //
    // 只让新写出的文件是 0600 不够：用户磁盘上**已经存在**的那一份才是正在
    // 泄露的那一份，而它是历史版本按 umask 建出来的 0644。config.dat 尤其要紧
    // ——Python 版往里写的是明文密码，且从未设过权限。
    // 返回未能收敛的文件列表（正常情况为空；非 POSIX 卷上可能非空）。
    static QStringList hardenConfigPermissions();

    // --- theme / language state (conf/theme.json, util.THEME equivalent) ---

    // Load theme.json into memory; remembers the path for saveTheme().
    // 对应Python: function/theme.py::MainWindow._load_current_settings (读取部分)
    bool loadTheme(const QString &themeJsonPath, QString *errorOut = nullptr);
    // Persist the in-memory theme back to the loaded path (indent, UTF-8).
    // 对应Python: function/theme.py::MainWindow._set_appearance (写回部分)
    bool saveTheme(QString *errorOut = nullptr) const;

    QJsonObject theme() const;
    void setTheme(const QJsonObject &theme);

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

    // 终端命令补全总开关 (theme.json 的 "command_completion")，默认开启。
    // 关闭后终端不再弹候选窗、Ctrl+Space 也不唤起；历史命令仍照常记录，
    // 便于再次开启时立刻有候选可用。
    bool commandCompletionEnabled() const;
    void setCommandCompletionEnabled(bool enabled);

    // SSH 建连超时秒数 (theme.json 的 "ssh_timeout")，默认 15。
    // 覆盖 TCP 建连 + SSH 握手 + 认证三段（见 SshClient::connectToHost）。
    //
    // 键与设置页早就在写的那个键是同一个（SettingsDialog 的 kSshTimeoutKey），
    // 但在此之前**没有任何调用点读它**——用户设成多少都不影响实际行为，
    // 因为 connectToHost 走的是裸阻塞 ::connect。这对访问器就是让它真正生效。
    int sshConnectTimeoutSeconds() const;
    void setSshConnectTimeoutSeconds(int seconds);

    // 全局代理 (theme.json 里平铺的 proxyType/proxyHost/... 一组键)。
    // 设备的代理类型选「全局代理」时取的就是这一份，见
    // ProxyConfig::resolveGlobalProxy 与 SshClient::connectToHost
    // （后者在调用方没显式给出全局那一份时会自己来这里取）。
    //
    // 代理密码**不在这里**：与设备密码同一个不变量——凭据只进钥匙串，
    // 不进任何 JSON。读出来的 ProxyConfig::password 恒为空，由上层填。
    ProxyConfig sshProxyConfig() const;
    void setSshProxyConfig(const ProxyConfig &proxy);

    // 全局代理的口令。**只在内存里**，不进 theme.json（明文只进钥匙串，见上）。
    //
    // 由持有 DeviceConfigStore 的一方推进来（MainWindow::publishGlobalProxyPassword），
    // connectToHost 回退去读全局代理时用它补上口令。做成"推"而不是"回调去查"是
    // 刻意的：建连跑在工作线程上，而 DeviceConfigStore 没有锁——让工作线程去
    // 调它的 resolvedGlobalProxyPassword() 会与 UI 线程的设备编辑撞在一起。
    // 推过来的是一个字符串副本，读写都在这把锁下面，没有这个问题。
    //
    // 建连的 5 个入口里有 3 个在 core 层拿不到设备存储，所以这份口令必须有一个
    // 进程级的落点；否则"要认证的全局代理"会静默表现成认证失败。
    void setSshProxyPassword(const QString &password);
    QString sshProxyPassword() const;

    // --- 跳板机凭据目录 ----------------------------------------------------
    //
    // 「跳转服务器」按 DeviceEntry::id 引用侧栏里已保存的设备，凭据复用那台设备
    // 自己的记录。可是建链跑在**工作线程**上，而 DeviceConfigStore 没有锁
    // （见 setSshProxyPassword 上面那段——同一个理由），所以这里放一份由持有
    // 设备存储的一方推过来的快照（MainWindow::publishJumpHostCatalog）。
    //
    // 为什么必须是进程级的落点而不是逐入口注入：建连有 5 个入口（终端 / 测试连接 /
    // 隧道 / SFTP 并行克隆 / frp），后三个在 core 层压根拿不到设备存储。靠每处
    // 记得注入，漏掉的表现是那条路径上「跳转服务器」静默报"跳板机已不存在"。
    //
    // 装的是 DeviceEntry 而不是新造一个凭据结构体：要的字段（host/port/username/
    // password/keyType/keyFile/proxy/protocol）它全都有，另造一份只会多一处要
    // 同步的地方。password 与 proxy.password 在这份快照里是**填好的**——这与
    // 「m_devices 里的条目永远不带密码」那条不变量不冲突：这不是 m_devices，
    // 是一份为了建连而临时解析出来的副本，且只包含真正被引用为跳板的设备。
    void setJumpHostCatalog(const QList<DeviceEntry> &hosts);
    QList<DeviceEntry> jumpHostCatalog() const;

    // 设备列表字号 (theme.json 的 "device_list_font_size")。
    // C++ 侧新增键；Python 版读不到该键时按自己的默认走，互不影响。
    // 默认值与设备列表历史硬编码一致（macOS 15，其它平台 14）。
    int deviceListFontSize() const;
    void setDeviceListFontSize(int pointSize);

private:
    GlobalState() = default;

    // 保护 m_theme / m_themePath 的每一次读写。
    //
    // 加锁的由来：这个单例长期只被 UI 线程碰，但 SSH 的建连超时与全局代理是在
    // **工作线程**上读的（SshClient::connectToHost 跑在 TunnelPool / ConnectionTester
    // / ssh_terminal_widget 起的 worker 上），而用户按下设置页的"确定"会在 UI
    // 线程改写 m_theme。QJsonObject 是 COW、引用计数非原子，两边同时碰同一个对象
    // 就是数据竞争。锁在这里加一次，所有访问器（以及以后每个新读者）自动安全。
    //
    // 不必担心递归：每个访问器都是"取锁 → 单次 value()/insert() → 放锁"，
    // 彼此不互相调用。theme() 按值返回（COW 复制很便宜），拿到副本后就与锁无关。
    mutable QMutex m_themeMutex;
    QJsonObject m_theme;
    QString m_themePath;
    // 全局代理口令。刻意**不**放进 m_theme——saveTheme() 会把 m_theme 整个写盘。
    QString m_sshProxyPassword;
    // 跳板机凭据快照（见 setJumpHostCatalog）。同样不进 m_theme。
    QList<DeviceEntry> m_jumpHostCatalog;
};

} // namespace cubeshell
