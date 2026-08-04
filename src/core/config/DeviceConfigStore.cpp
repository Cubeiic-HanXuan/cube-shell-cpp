// DeviceConfigStore.cpp — device config persistence. See DeviceConfigStore.h.

#include "DeviceConfigStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "PickleReader.h"

namespace cubeshell {

// Mirrors util.parse_host_port: IPv4 "[v6]:port", bare IPv6, host:port, host.
HostPort parseHostPort(const QString &hostStr, quint16 defaultPort)
{
    HostPort out{hostStr.trimmed(), defaultPort};
    const QString s = hostStr.trimmed();
    if (s.isEmpty())
        return out;

    if (s.startsWith(QLatin1Char('['))) {
        const int end = s.indexOf(QLatin1Char(']'));
        if (end > 0) {
            out.host = s.mid(1, end - 1);
            const QString rest = s.mid(end + 1);
            if (rest.startsWith(QLatin1Char(':'))) {
                bool ok = false;
                const int p = rest.mid(1).toInt(&ok);
                out.port = ok ? quint16(p) : defaultPort;
            } else {
                out.port = defaultPort;
            }
            return out;
        }
    }

    const int colons = s.count(QLatin1Char(':'));
    if (colons == 1) {
        const int idx = s.lastIndexOf(QLatin1Char(':'));
        out.host = s.left(idx);
        bool ok = false;
        const int p = s.mid(idx + 1).toInt(&ok);
        out.port = ok ? quint16(p) : defaultPort;
    } else if (colons > 1) {
        // Bare IPv6 without brackets -> no port.
        out.host = s;
        out.port = defaultPort;
    } else {
        out.host = s;
        out.port = defaultPort;
    }
    return out;
}

// Mirrors util.format_host_port: bracket IPv6, plain otherwise.
QString formatHostPort(const QString &host, quint16 port)
{
    QString h = host.trimmed();
    h.remove(QLatin1Char('[')).remove(QLatin1Char(']'));
    if (h.count(QLatin1Char(':')) > 1) // IPv6
        return QStringLiteral("[%1]:%2").arg(h).arg(port);
    return QStringLiteral("%1:%2").arg(h).arg(port);
}

HostPort DeviceEntry::hostPort() const
{
    // The pickle form stores host as "host:port"; the explicit `port` field is
    // only populated for JSON-loaded entries. Prefer parsing the host string.
    // RDP 设备默认端口 3389。对应Python: RDP 分支的 '3389' 默认值
    const quint16 def = isRdp() ? 3389 : 22;
    if (host.contains(QLatin1Char(':')) || host.startsWith(QLatin1Char('[')))
        return parseHostPort(host, port ? port : def);
    return {host, quint16(port ? port : def)};
}

const DeviceEntry *DeviceConfigStore::find(const QString &name) const
{
    auto it = m_devices.constFind(name);
    return it == m_devices.constEnd() ? nullptr : &it.value();
}

bool DeviceConfigStore::load(const QString &configDatPath, QString *errorOut)
{
    QFile f(configDatPath);
    if (!f.exists()) {
        if (errorOut) *errorOut = QStringLiteral("file not found: %1").arg(configDatPath);
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot open %1").arg(configDatPath);
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    // Empty / trivially-empty pickle ({}) yields zero devices, which is fine.
    // 用原始 Value 树区分 list（SSH）与 dict（RDP）条目。
    // 对应Python: util.device_protocol — dict 且 __type__=="rdp" 为 RDP 设备
    PickleReader::Value root;
    if (!PickleReader::parseRoot(data, root, errorOut))
        return false;

    using V = PickleReader::Value;
    for (const auto &kv : root.dict) {
        if (kv.first.type != V::Str)
            continue;
        DeviceEntry e;
        e.name = kv.first.str;
        const V &val = kv.second;
        if (val.type == V::Dict) {
            // RDP 条目：dict {__type__, username, password, host, domain, auth}
            // 对应Python: cube-shell.py::AddConfigUi.addDev 的 RDP 保存格式
            QHash<QString, QString> m;
            for (const auto &p : val.dict) {
                if (p.first.type == V::Str && p.second.type == V::Str)
                    m.insert(p.first.str, p.second.str);
            }
            if (m.value(QStringLiteral("__type__")) != QLatin1String("rdp"))
                continue;   // 未知 dict 条目，跳过（向前兼容）
            e.protocol = QStringLiteral("rdp");
            e.username = m.value(QStringLiteral("username"));
            e.password = m.value(QStringLiteral("password"));
            e.host     = m.value(QStringLiteral("host"));
            e.domain   = m.value(QStringLiteral("domain"));
            const QString auth = m.value(QStringLiteral("auth"));
            e.auth = auth.isEmpty() ? QStringLiteral("ntlm") : auth;
            e.port = parseHostPort(e.host, 3389).port;
        } else if (val.type == V::List) {
            // SSH 条目（既有逻辑）：
            // [user, password, host] or [user, password, host, keyType, keyFile]
            QStringList f0;
            for (const V &item : val.list)
                f0 << (item.type == V::Str ? item.str : QString());
            if (f0.size() > 0) e.username = f0[0];
            if (f0.size() > 1) e.password = f0[1];
            if (f0.size() > 2) e.host     = f0[2];
            if (f0.size() > 3) e.keyType  = f0[3];
            if (f0.size() > 4) e.keyFile  = f0[4];
        } else {
            continue;   // 其它类型条目不识别，跳过
        }
        m_devices.insert(e.name, e);
    }
    return true;
}

bool DeviceConfigStore::saveJson(const QString &jsonPath, QString *errorOut) const
{
    QJsonArray arr;
    for (const DeviceEntry &e : m_devices) {
        QJsonObject o;
        o[QStringLiteral("name")]     = e.name;
        o[QStringLiteral("username")] = e.username;
        o[QStringLiteral("password")] = e.password;
        o[QStringLiteral("host")]     = e.host;
        o[QStringLiteral("port")]     = int(e.port);
        o[QStringLiteral("keyType")]  = e.keyType;
        o[QStringLiteral("keyFile")]  = e.keyFile;
        // RDP 字段。对应Python: RDP dict 的 __type__/domain/auth
        o[QStringLiteral("protocol")] = e.protocol;
        o[QStringLiteral("domain")]   = e.domain;
        o[QStringLiteral("auth")]     = e.auth;
        // 串口字段（C++ 侧新增；非串口条目也一并写出，保持 JSON 结构一致）。
        o[QStringLiteral("portName")]    = e.portName;
        o[QStringLiteral("baudRate")]    = e.baudRate;
        o[QStringLiteral("dataBits")]    = e.dataBits;
        o[QStringLiteral("parity")]      = e.parity;
        o[QStringLiteral("stopBits")]    = e.stopBits;
        o[QStringLiteral("flowControl")] = e.flowControl;
        o[QStringLiteral("newlineMode")] = e.newlineMode;
        o[QStringLiteral("localEcho")]   = e.localEcho;
        arr.append(o);
    }

    QSaveFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot write %1").arg(jsonPath);
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("commit failed for %1").arg(jsonPath);
        return false;
    }
    return true;
}

bool DeviceConfigStore::loadJson(const QString &jsonPath, QString *errorOut)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("cannot open %1").arg(jsonPath);
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        if (errorOut) *errorOut = QStringLiteral("invalid JSON in %1").arg(jsonPath);
        return false;
    }
    m_devices.clear();
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        DeviceEntry e;
        e.name     = o[QStringLiteral("name")].toString();
        e.username = o[QStringLiteral("username")].toString();
        e.password = o[QStringLiteral("password")].toString();
        e.host     = o[QStringLiteral("host")].toString();
        e.port     = quint16(o[QStringLiteral("port")].toInt(22));
        e.keyType  = o[QStringLiteral("keyType")].toString();
        e.keyFile  = o[QStringLiteral("keyFile")].toString();
        // RDP 字段（旧版 JSON 无这些键时回落到 ssh/ntlm 默认值）。
        // 对应Python: util.device_protocol 的容错判别
        const QString protocol = o[QStringLiteral("protocol")].toString();
        e.protocol = protocol.isEmpty() ? QStringLiteral("ssh") : protocol;
        e.domain   = o[QStringLiteral("domain")].toString();
        const QString auth = o[QStringLiteral("auth")].toString();
        e.auth = auth.isEmpty() ? QStringLiteral("ntlm") : auth;
        // 串口字段（旧版 JSON 无这些键时保持结构体默认值，同上面的容错风格）。
        e.portName = o[QStringLiteral("portName")].toString();
        e.baudRate = o[QStringLiteral("baudRate")].toInt(115200);
        e.dataBits = o[QStringLiteral("dataBits")].toInt(8);
        const QString parity = o[QStringLiteral("parity")].toString();
        e.parity = parity.isEmpty() ? QStringLiteral("none") : parity;
        const QString stopBits = o[QStringLiteral("stopBits")].toString();
        e.stopBits = stopBits.isEmpty() ? QStringLiteral("1") : stopBits;
        const QString flow = o[QStringLiteral("flowControl")].toString();
        e.flowControl = flow.isEmpty() ? QStringLiteral("none") : flow;
        const QString newline = o[QStringLiteral("newlineMode")].toString();
        e.newlineMode = newline.isEmpty() ? QStringLiteral("cr") : newline;
        e.localEcho = o[QStringLiteral("localEcho")].toBool(false);
        if (!e.name.isEmpty())
            m_devices.insert(e.name, e);
    }
    return true;
}

} // namespace cubeshell
