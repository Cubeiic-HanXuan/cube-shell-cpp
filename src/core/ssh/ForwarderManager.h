#pragma once

// ForwarderManager.h — C++ port of core/forwarder.py's ForwarderManager.
//
// Owns a map of active port-forwarding tunnels keyed by tunnel id, plus the
// SshClient each tunnel runs on. All public methods are thread-safe (Python
// used a threading.Lock; we use a QMutex). Lifecycle events from the tunnels
// are re-emitted as Qt signals so the UI can observe them without polling.

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

#include "SshClient.h"

namespace cubeshell {

class PortForwarder;

// Type of tunnel, mirroring forwarder.py's tunnel_type strings.
enum class TunnelType {
    Local,   // "local"   — listen locally, forward to a remote host via the server
    Remote,  // "remote"  — server listens, forward back to a local target
    Dynamic  // "dynamic" — local SOCKS5 proxy over the SSH connection
};

// Everything needed to open a tunnel (mirrors start_tunnel()'s kwargs).
struct TunnelSpec {
    TunnelType type = TunnelType::Local;
    QString localHost;      // bind address for local/dynamic; for remote see note
    quint16 localPort = 0;  // local/dynamic bind port; remote: server listen port
    QString remoteHost;     // local: where the server should connect; remote: local target host
    quint16 remotePort = 0; // local: remote target port; remote: local target port

    // SSH connection parameters (forwarders create their own SshClient).
    QString sshHost;
    quint16 sshPort = 22;
    QString sshUser;
    QString sshPassword;
    QString keyType;   // "Ed25519Key"/"RSAKey"/"ECDSAKey"/"DSSKey" (empty = password/agent)
    QString keyFile;
    QString keyPassphrase;

    // 代理。由凭据解析回调从设备条目里带过来（见 MainWindow::setupTunnels）——
    // 不能让 connectClient 自己去读配置：它跑在工作线程上，而且隧道用的是
    // 哪台设备只有解析回调知道。
    ProxyConfig proxy;
};

// 对应C++: class ForwarderManager
class ForwarderManager : public QObject {
    Q_OBJECT
public:
    explicit ForwarderManager(QObject *parent = nullptr);
    ~ForwarderManager() override;

    // Register an already-created tunnel under tunnelId (mirrors add_tunnel).
    void addTunnel(const QString &tunnelId, PortForwarder *tunnel);

    // Stop and remove the tunnel under tunnelId (mirrors remove_tunnel); also
    // closes its SSH client if no other tunnel shares it.
    void removeTunnel(const QString &tunnelId);

    // Create + connect an SshClient, build the right forwarder, start it, and
    // register it under tunnelId. Returns the started tunnel, or nullptr on
    // failure (fills errorMessage). Mirrors start_tunnel().
    //
    // The privileged-port check (<1024 needs root on POSIX) from start_tunnel()
    // is enforced here and reported via errorMessage.
    PortForwarder *startTunnel(const QString &tunnelId, const TunnelSpec &spec,
                               QString &errorMessage);

    // True if the SSH transport backing a tunnel is still alive+authenticated.
    bool isTunnelAlive(const QString &tunnelId);

    // Stop and remove every tunnel.
    void stopAll();

    int tunnelCount() const;

signals:
    // Relayed from the contained tunnels (queued to the UI thread).
    void tunnelStarted(const QString &tunnelId);
    void tunnelStopped(const QString &tunnelId);
    void tunnelError(const QString &tunnelId, const QString &message);
    void tunnelLog(const QString &tunnelId, const QString &message);

private:
    // Connect+authenticate an SshClient from a spec. Returns nullptr on failure.
    SshClient *connectClient(const TunnelSpec &spec, QString &errorMessage);
    // Close the client if no remaining tunnel uses it (call with m_lock held).
    void closeClientIfUnusedUnsafe(SshClient *client);

    QHash<QString, PortForwarder *> m_tunnels; // tunnelId -> tunnel
    QHash<SshClient *, int> m_clientRefs;      // client -> # of tunnels using it
    mutable QMutex m_lock;
};

} // namespace cubeshell
