#pragma once

// DeviceConfigStore.h — device/connection configuration persistence.
//
// Reads the existing Python-pickle config.dat (via PickleReader) and writes
// new/modified configs as JSON, per the chosen migration strategy
// ("read pickle, write JSON"). The device model mirrors cube-shell's
// config.dat entries: name -> [user, password, host, keyType, keyFile].

#include <QHash>
#include <QString>
#include <QStringList>

namespace cubeshell {

// Split/format "host:port" like cube-shell's util.parse_host_port /
// format_host_port (IPv4, [IPv6]:port, bare host -> default port).
struct HostPort {
    QString host;
    quint16 port;
};
HostPort parseHostPort(const QString &hostStr, quint16 defaultPort = 22);
QString formatHostPort(const QString &host, quint16 port);

// A single saved device/connection.
struct DeviceEntry {
    QString name;
    QString username;
    QString password;
    QString host;      // stored as "host:port" (pickle compat) — see hostPort()
    quint16  port = 22;
    QString keyType;   // "Ed25519Key" | "RSAKey" | "ECDSAKey" | "DSSKey" | ""
    QString keyFile;

    // RDP 支持。对应Python: config.dat 中 RDP 条目为 dict，含 __type__/domain/auth
    // 字段（cube-shell.py::AddConfigUi.addDev，util.device_protocol）
    QString protocol = QStringLiteral("ssh");  // "ssh" | "rdp"
    QString domain;    // Windows 域（RDP 专用，对应Python RDP dict["domain"]）
    QString auth = QStringLiteral("ntlm");  // RDP 认证方式："ntlm" | "plain"（对应Python RDP dict["auth"]）

    bool isRdp() const { return protocol == QLatin1String("rdp"); }

    // Resolved host/port (parses the "host:port" string form).
    HostPort hostPort() const;
    // True if this entry authenticates with a private key rather than password.
    bool usesKey() const { return !keyType.isEmpty() && !keyFile.isEmpty(); }
};

class DeviceConfigStore {
public:
    // Load devices from config.dat (pickle). Returns false if the file does
    // not exist or cannot be parsed (out is still filled with whatever parsed).
    bool load(const QString &configDatPath, QString *errorOut = nullptr);

    // Save devices as JSON (the new forward format).
    bool saveJson(const QString &jsonPath, QString *errorOut = nullptr) const;
    // Load devices previously saved as JSON.
    bool loadJson(const QString &jsonPath, QString *errorOut = nullptr);

    QList<DeviceEntry> devices() const { return m_devices.values(); }
    void addDevice(const DeviceEntry &entry) { m_devices.insert(entry.name, entry); }
    bool removeDevice(const QString &name) { return m_devices.remove(name) > 0; }
    const DeviceEntry *find(const QString &name) const;
    int count() const { return m_devices.size(); }
    bool isEmpty() const { return m_devices.isEmpty(); }
    QStringList names() const { return m_devices.keys(); }

private:
    QHash<QString, DeviceEntry> m_devices;
};

} // namespace cubeshell
