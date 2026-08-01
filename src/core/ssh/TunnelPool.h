#pragma once

// TunnelPool.h — 多隧道生命周期管理（forwarder.py 管理层 + tunnel.json 配置）。
//
// 对应Python: cube-shell.py::MainWindow.tunnel_refresh / Tunnel.start_tunnel /
//             Tunnel.stop_tunnel / AddTunnelConfig.addTunnel +
//             core/forwarder.py::ForwarderManager（生命周期部分）
//
// 职责：
//  - 读写 conf/tunnel.json（键名与 Python 侧 core/vars.py::KEYS 完全一致，
//    双向互兼容：tunnel_type / browser_open / device_name /
//    remote_bind_address / local_bind_address）；
//  - 按名称启动/停止隧道（内部委托 ForwarderManager 完成 SSH 连接与
//    Local/Remote/Dynamic 转发器构建）；
//  - 状态监控 + 断线自动重连（指数退避，Python 侧无此能力，为 C++ 增强，
//    默认关闭时行为与 Python 完全一致）；
//  - 信号 tunnelStateChanged(name, state) 通知 UI。
//
// 线程模型：TunnelPool 本体运行在创建它的线程（通常是 UI 线程）。
// ForwarderManager::startTunnel 是阻塞的（SSH 握手），因此每次启动都放到
// 一次性 QThread 上执行，结果经 Qt::QueuedConnection 送回本线程；
// ForwarderManager 转发来的信号也全部是 QueuedConnection，所以内部状态表
// m_runtime 只在本线程读写，无须加锁。

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include "ForwarderManager.h"

namespace cubeshell {

// tunnel.json 中的一条隧道配置（值与 Python UI 写入的字符串原样保存）。
// 对应Python: cube-shell.py::TunnelConfig.as_dict / AddTunnelConfig.addTunnel 的 dic
struct TunnelEntry {
    QString tunnelType;        // "本地"/"远程"/"动态"（兼容 "local"/"remote"/"dynamic"）
    QString browserOpen;       // 启动后自动打开的 URL（可为空）
    QString deviceName;        // config.dat 中的设备名（SSH 凭据来源）
    QString remoteBindAddress; // "host:port"；动态模式为空
    QString localBindAddress;  // "host:port"

    bool isValid() const { return !tunnelType.isEmpty() && !localBindAddress.isEmpty(); }

    QJsonObject toJson() const;
    static TunnelEntry fromJson(const QJsonObject &obj);
};

// 对应Python: cube-shell.py 隧道管理层（tunnel_refresh + Tunnel 集合）
class TunnelPool : public QObject {
    Q_OBJECT
public:
    // 隧道运行状态。
    enum class TunnelState {
        Stopped,      // 未运行（初始 / 用户停止）
        Connecting,   // 正在建立 SSH 连接 + 启动转发器
        Running,      // 转发器已监听
        Reconnecting, // 意外断开，等待退避后重连
        Failed        // 启动失败且不再重试
    };
    Q_ENUM(TunnelState)

    // 由 device_name 解析 SSH 连接参数（host/port/user/password/key*）。
    // 返回 false 表示设备不存在或凭据无法解析（填 error）。
    // 对应Python: cube-shell.py::open_data(ssh) + util.parse_host_port
    using CredentialResolver =
        std::function<bool(const QString &deviceName, TunnelSpec &spec, QString &error)>;

    explicit TunnelPool(QObject *parent = nullptr);
    ~TunnelPool() override;

    // --- tunnel.json 配置（与 Python 侧互兼容） ---
    void setConfigPath(const QString &path) { m_configPath = path; }
    QString configPath() const { return m_configPath; }
    // 对应Python: cube-shell.py::tunnel_refresh 中 util.read_json(file_path)
    bool loadConfig(QString *errorOut = nullptr);
    // 对应Python: cube-shell.py::save_config / addTunnel 中 util.write_json
    bool saveConfig(QString *errorOut = nullptr) const;

    QStringList tunnelNames() const;
    TunnelEntry entry(const QString &name) const { return m_entries.value(name); }
    bool contains(const QString &name) const { return m_entries.contains(name); }
    // 新增/更新一条配置（内存态；persist=true 时立即写回 tunnel.json）。
    void setEntry(const QString &name, const TunnelEntry &entry, bool persist = true);
    // 对应Python: cube-shell.py::Tunnel.delete_tunnel（del data[name] + write_json）
    bool removeEntry(const QString &name, bool persist = true);

    // --- 生命周期 ---
    void setCredentialResolver(CredentialResolver resolver) { m_resolver = std::move(resolver); }

    // 异步启动：立即返回；结果经 tunnelStateChanged 通知。
    // 对应Python: cube-shell.py::Tunnel.start_tunnel
    void startTunnel(const QString &name);
    // 用户主动停止（清除重连计划）。
    // 对应Python: cube-shell.py::Tunnel.stop_tunnel
    void stopTunnel(const QString &name);
    // 对应Python: cube-shell.py::do_killall_ssh（关闭所有隧道）
    void stopAll();

    TunnelState state(const QString &name) const;
    bool isRunning(const QString &name) const { return state(name) == TunnelState::Running; }

    // --- 自动重连（指数退避：1s, 2s, 4s ... 封顶 60s） ---
    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }
    bool autoReconnect() const { return m_autoReconnect; }
    // 最大重连次数；-1 表示不限（默认）。
    void setMaxReconnectAttempts(int attempts) { m_maxReconnectAttempts = attempts; }

    ForwarderManager *manager() { return &m_manager; }

    // 类型字符串映射（"本地"/"local" -> Local ...）。解析失败返回 false。
    static bool parseTunnelType(const QString &text, TunnelType &type);
    // 第 attempt 次（从 0 起）重连前的退避毫秒数（纯函数，便于单测）。
    static int backoffDelayMs(int attempt);

signals:
    // 状态变化（含启动成功/失败/断开/进入重连等待）。
    void tunnelStateChanged(const QString &name, cubeshell::TunnelPool::TunnelState state);
    // 人类可读日志（转发 ForwarderManager 的 log/error）。
    void tunnelLog(const QString &name, const QString &message);

private:
    // 每条隧道的运行时记录（仅在本线程访问）。
    struct Runtime {
        TunnelState state = TunnelState::Stopped;
        int reconnectAttempts = 0;
        bool userStopped = false; // 用户主动停止 -> 不重连
        bool starting = false;    // 有 in-flight 的启动线程
    };

    // 由配置项构建 ForwarderManager::startTunnel 所需的 TunnelSpec。
    bool buildSpec(const QString &name, TunnelSpec &spec, QString &error) const;
    // 在工作线程上执行阻塞启动，结果回投本线程。
    void attemptStart(const QString &name);
    void setState(const QString &name, TunnelState state);
    void scheduleReconnect(const QString &name);

    QString m_configPath;
    QHash<QString, TunnelEntry> m_entries;
    QHash<QString, Runtime> m_runtime;
    ForwarderManager m_manager;
    CredentialResolver m_resolver;
    bool m_autoReconnect = true;
    int m_maxReconnectAttempts = -1;
};

} // namespace cubeshell
