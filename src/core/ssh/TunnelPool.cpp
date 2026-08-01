// TunnelPool.cpp — 多隧道生命周期管理。见 TunnelPool.h 的设计说明。

#include "TunnelPool.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

#include "PortForwarder.h"
#include "config/ConfigUtil.h"
#include "config/DeviceConfigStore.h" // parseHostPort

Q_LOGGING_CATEGORY(tunnelPoolLog, "cubeshell.tunnelpool")

namespace cubeshell {

// tunnel.json 键名，必须与 Python 侧 core/vars.py::KEYS 逐字一致。
// 对应Python: core/vars.py::KEYS
namespace {
const QString kKeyTunnelType        = QStringLiteral("tunnel_type");
const QString kKeyBrowserOpen       = QStringLiteral("browser_open");
const QString kKeyDeviceName        = QStringLiteral("device_name");
const QString kKeyRemoteBindAddress = QStringLiteral("remote_bind_address");
const QString kKeyLocalBindAddress  = QStringLiteral("local_bind_address");
} // namespace

// 对应Python: cube-shell.py::TunnelConfig.as_dict
QJsonObject TunnelEntry::toJson() const
{
    QJsonObject obj;
    obj.insert(kKeyTunnelType, tunnelType);
    obj.insert(kKeyBrowserOpen, browserOpen);
    obj.insert(kKeyDeviceName, deviceName);
    obj.insert(kKeyRemoteBindAddress, remoteBindAddress);
    obj.insert(kKeyLocalBindAddress, localBindAddress);
    return obj;
}

TunnelEntry TunnelEntry::fromJson(const QJsonObject &obj)
{
    TunnelEntry e;
    e.tunnelType        = obj.value(kKeyTunnelType).toString();
    e.browserOpen       = obj.value(kKeyBrowserOpen).toString();
    e.deviceName        = obj.value(kKeyDeviceName).toString();
    e.remoteBindAddress = obj.value(kKeyRemoteBindAddress).toString();
    e.localBindAddress  = obj.value(kKeyLocalBindAddress).toString();
    return e;
}

TunnelPool::TunnelPool(QObject *parent)
    : QObject(parent)
{
    // ForwarderManager 的信号来自转发器工作线程（其内部已用 QueuedConnection
    // 转发到 manager），这里再显式 QueuedConnection 转到 pool 所在线程，
    // 确保 m_runtime 只在本线程被触碰。
    connect(&m_manager, &ForwarderManager::tunnelStarted, this,
            [this](const QString &name) {
                Runtime &rt = m_runtime[name];
                rt.reconnectAttempts = 0;
                setState(name, TunnelState::Running);
            },
            Qt::QueuedConnection);
    connect(&m_manager, &ForwarderManager::tunnelStopped, this,
            [this](const QString &name) {
                Runtime &rt = m_runtime[name];
                if (rt.userStopped || !m_entries.contains(name)) {
                    setState(name, TunnelState::Stopped);
                    return;
                }
                // 意外断开（SSH 断线 / 监听 socket 出错）—— 自动重连。
                scheduleReconnect(name);
            },
            Qt::QueuedConnection);
    connect(&m_manager, &ForwarderManager::tunnelError, this,
            [this](const QString &name, const QString &msg) {
                emit tunnelLog(name, msg);
            },
            Qt::QueuedConnection);
    connect(&m_manager, &ForwarderManager::tunnelLog, this,
            [this](const QString &name, const QString &msg) {
                emit tunnelLog(name, msg);
            },
            Qt::QueuedConnection);
}

TunnelPool::~TunnelPool()
{
    // 析构时不再重连；ForwarderManager 的析构会 stopAll()。
    for (auto it = m_runtime.begin(); it != m_runtime.end(); ++it)
        it.value().userStopped = true;
}

// ---------------------------------------------------------------------------
// tunnel.json 读写
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py::tunnel_refresh（read_json + 逐项构建）
bool TunnelPool::loadConfig(QString *errorOut)
{
    QString err;
    const QJsonValue root = ConfigUtil::readJson(m_configPath, &err);
    if (root.isUndefined()) {
        if (errorOut)
            *errorOut = err;
        return false;
    }
    m_entries.clear();
    const QJsonObject obj = root.toObject(); // "{}" -> 空表，与 Python 语义一致
    for (auto it = obj.begin(); it != obj.end(); ++it)
        m_entries.insert(it.key(), TunnelEntry::fromJson(it.value().toObject()));
    return true;
}

// 对应Python: function/util.py::write_json（indent=4）
bool TunnelPool::saveConfig(QString *errorOut) const
{
    QJsonObject root;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        root.insert(it.key(), it.value().toJson());
    return ConfigUtil::writeJson(m_configPath, root, errorOut);
}

QStringList TunnelPool::tunnelNames() const
{
    QStringList names = m_entries.keys();
    names.sort(); // 对应Python: sorted(self.data.keys())
    return names;
}

void TunnelPool::setEntry(const QString &name, const TunnelEntry &entry, bool persist)
{
    m_entries.insert(name, entry);
    if (persist)
        saveConfig();
}

bool TunnelPool::removeEntry(const QString &name, bool persist)
{
    if (!m_entries.contains(name))
        return false;
    stopTunnel(name);
    m_entries.remove(name);
    m_runtime.remove(name);
    if (persist)
        saveConfig();
    return true;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py::Tunnel.start_tunnel 中的 type_ 判断
bool TunnelPool::parseTunnelType(const QString &text, TunnelType &type)
{
    const QString t = text.trimmed().toLower();
    if (t == QStringLiteral("本地") || t == QStringLiteral("local")) {
        type = TunnelType::Local;
        return true;
    }
    if (t == QStringLiteral("远程") || t == QStringLiteral("remote")) {
        type = TunnelType::Remote;
        return true;
    }
    if (t == QStringLiteral("动态") || t == QStringLiteral("dynamic")) {
        type = TunnelType::Dynamic;
        return true;
    }
    return false;
}

int TunnelPool::backoffDelayMs(int attempt)
{
    // 1s, 2s, 4s, 8s ... 封顶 60s（指数退避）。
    if (attempt < 0)
        attempt = 0;
    if (attempt > 6) // 2^6 = 64 > 60，已到顶
        return 60000;
    const int delay = 1000 << attempt;
    return delay > 60000 ? 60000 : delay;
}

// 对应Python: cube-shell.py::Tunnel.start_tunnel（参数装配部分）
bool TunnelPool::buildSpec(const QString &name, TunnelSpec &spec, QString &error) const
{
    const TunnelEntry e = m_entries.value(name);
    if (!e.isValid()) {
        error = QStringLiteral("Tunnel '%1' has no valid configuration").arg(name);
        return false;
    }

    if (!parseTunnelType(e.tunnelType, spec.type)) {
        error = QStringLiteral("Invalid tunnel type: %1").arg(e.tunnelType);
        return false;
    }

    const HostPort local = parseHostPort(e.localBindAddress, 0);
    if (local.port == 0) {
        error = QStringLiteral("Invalid local bind address: %1").arg(e.localBindAddress);
        return false;
    }
    spec.localHost = local.host;
    spec.localPort = local.port;

    if (spec.type != TunnelType::Dynamic) {
        const HostPort remote = parseHostPort(e.remoteBindAddress, 0);
        if (remote.port == 0) {
            error = QStringLiteral("Invalid remote bind address: %1").arg(e.remoteBindAddress);
            return false;
        }
        spec.remoteHost = remote.host;
        spec.remotePort = remote.port;
    }

    if (!m_resolver) {
        error = QStringLiteral("No credential resolver set");
        return false;
    }
    return m_resolver(e.deviceName, spec, error);
}

void TunnelPool::startTunnel(const QString &name)
{
    Runtime &rt = m_runtime[name];
    if (rt.starting || rt.state == TunnelState::Running)
        return;
    rt.userStopped = false;
    rt.reconnectAttempts = 0;
    attemptStart(name);
}

void TunnelPool::attemptStart(const QString &name)
{
    Runtime &rt = m_runtime[name];
    if (rt.starting || rt.userStopped)
        return;

    TunnelSpec spec;
    QString err;
    if (!buildSpec(name, spec, err)) {
        emit tunnelLog(name, err);
        setState(name, TunnelState::Failed);
        return;
    }

    rt.starting = true;
    setState(name, rt.reconnectAttempts > 0 ? TunnelState::Reconnecting
                                            : TunnelState::Connecting);

    // 阻塞的 SSH 连接放到一次性工作线程；结果经 QueuedConnection 回到本线程。
    QThread *worker = QThread::create([this, name, spec]() {
        QString startErr;
        PortForwarder *tunnel = m_manager.startTunnel(name, spec, startErr);
        const bool ok = (tunnel != nullptr);
        QMetaObject::invokeMethod(this, [this, name, ok, startErr]() {
            Runtime &rt2 = m_runtime[name];
            rt2.starting = false;
            if (ok) {
                // Running 状态由 ForwarderManager::tunnelStarted 信号确认；
                // 这里只在启动失败时处理。
                return;
            }
            emit tunnelLog(name, startErr);
            if (rt2.userStopped) {
                setState(name, TunnelState::Stopped);
                return;
            }
            scheduleReconnect(name);
        }, Qt::QueuedConnection);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void TunnelPool::scheduleReconnect(const QString &name)
{
    Runtime &rt = m_runtime[name];
    if (!m_autoReconnect
        || (m_maxReconnectAttempts >= 0 && rt.reconnectAttempts >= m_maxReconnectAttempts)) {
        setState(name, TunnelState::Failed);
        return;
    }

    const int delay = backoffDelayMs(rt.reconnectAttempts);
    rt.reconnectAttempts += 1;
    setState(name, TunnelState::Reconnecting);
    emit tunnelLog(name, QStringLiteral("Reconnecting in %1 ms (attempt %2)")
                             .arg(delay).arg(rt.reconnectAttempts));

    QTimer::singleShot(delay, this, [this, name]() {
        const Runtime rt2 = m_runtime.value(name);
        if (rt2.userStopped || rt2.state != TunnelState::Reconnecting)
            return; // 期间被用户停止/删除/手动重启
        attemptStart(name);
    });
}

// 对应Python: core/forwarder.py::ForwarderManager.remove_tunnel
void TunnelPool::stopTunnel(const QString &name)
{
    Runtime &rt = m_runtime[name];
    rt.userStopped = true;
    rt.reconnectAttempts = 0;
    m_manager.removeTunnel(name);
    setState(name, TunnelState::Stopped);
}

void TunnelPool::stopAll()
{
    for (auto it = m_runtime.begin(); it != m_runtime.end(); ++it) {
        it.value().userStopped = true;
        it.value().reconnectAttempts = 0;
    }
    m_manager.stopAll();
    for (auto it = m_runtime.begin(); it != m_runtime.end(); ++it)
        setState(it.key(), TunnelState::Stopped);
}

TunnelPool::TunnelState TunnelPool::state(const QString &name) const
{
    return m_runtime.value(name).state;
}

void TunnelPool::setState(const QString &name, TunnelState state)
{
    Runtime &rt = m_runtime[name];
    if (rt.state == state)
        return;
    rt.state = state;
    qCInfo(tunnelPoolLog) << "tunnel" << name << "->" << int(state);
    emit tunnelStateChanged(name, state);
}

} // namespace cubeshell
