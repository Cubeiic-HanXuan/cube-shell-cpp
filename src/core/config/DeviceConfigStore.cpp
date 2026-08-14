// DeviceConfigStore.cpp — device config persistence. See DeviceConfigStore.h.

#include "DeviceConfigStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include "ConfigUtil.h"
#include "GlobalState.h"
#include "PickleReader.h"
#include "Secrets.h"

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

quint16 defaultPortFor(const QString &protocol)
{
    if (protocol == QLatin1String("rdp"))
        return 3389;
    // 裸 TCP 没有 IANA 默认端口，但新建会话最常见的场景是先连 telnet 试探，
    // 故与 telnet 同取 23；用户自己填的端口会覆盖它。
    if (protocol == QLatin1String("telnet") || protocol == QLatin1String("tcp"))
        return 23;
    return 22;   // ssh / serial（不用端口）/ 空值旧配置
}

QString defaultNewlineModeFor(const QString &protocol)
{
    if (protocol == QLatin1String("telnet"))
        return QStringLiteral("crlf");
    if (protocol == QLatin1String("tcp"))
        return QStringLiteral("lf");
    return QStringLiteral("cr");   // serial / 其他
}

HostPort DeviceEntry::hostPort() const
{
    // The pickle form stores host as "host:port"; the explicit `port` field is
    // only populated for JSON-loaded entries. Prefer parsing the host string.
    // 各协议默认端口收敛在 defaultPortFor()。
    const quint16 def = defaultPortFor(protocol);
    if (host.contains(QLatin1Char(':')) || host.startsWith(QLatin1Char('[')))
        return parseHostPort(host, port ? port : def);
    return {host, quint16(port ? port : def)};
}

const DeviceEntry *DeviceConfigStore::find(const QString &name) const
{
    auto it = m_devices.constFind(name);
    return it == m_devices.constEnd() ? nullptr : &it.value();
}

// ---------------------------------------------------------------------------
// 密码：内存表 + 单条聚合钥匙串条目
// ---------------------------------------------------------------------------

QString DeviceConfigStore::secretService()
{
    return QLatin1String(vars::APP_NAME);
}

QString DeviceConfigStore::secretAccount()
{
    // 单条聚合：所有设备密码打包成一个 {id: password} JSON 存进这一个条目。
    // 每设备一条的方案在 ad-hoc 签名下会让升级后每台设备首次连接各弹一次
    // 授权框（21 台 = 21 次）；聚合成一条只弹一次，ACL 保护强度完全不变。
    return QStringLiteral("device-passwords");
}

QString DeviceConfigStore::newDeviceId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void DeviceConfigStore::ensureSecretsLoaded() const
{
    if (m_secretsLoaded)
        return;
    m_secretsLoaded = true;   // 失败也只尝试一次，别让每次连接都去撞钥匙串

    QString err;
    const QString blob = Secrets::retrieveSecret(secretService(), secretAccount(), &err);
    if (blob.isEmpty()) {
        if (!err.isEmpty())
            qWarning("读取钥匙串失败: %s", qPrintable(err));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8());
    if (!doc.isObject()) {
        qWarning("钥匙串里的设备密码不是合法 JSON，忽略");
        return;
    }
    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        // 内存里已有的优先：那是刚从旧 JSON 灌进来、或用户刚改过的，比钥匙串新。
        if (!m_secrets.contains(it.key()))
            m_secrets.insert(it.key(), it.value().toString());
    }
}

void DeviceConfigStore::addDevice(const DeviceEntry &entry)
{
    DeviceEntry e = entry;
    if (e.id.isEmpty())
        e.id = newDeviceId();
    // 密码搬进密码表；空串表示「不改动已存密码」（见头文件说明）。
    if (!e.password.isEmpty())
        m_secrets.insert(e.id, e.password);
    e.password.clear();       // 不变量：m_devices 里永远不带密码
    m_devices.insert(e.name, e);
}

DeviceEntry DeviceConfigStore::resolved(const QString &name) const
{
    auto it = m_devices.constFind(name);
    if (it == m_devices.constEnd())
        return {};
    ensureSecretsLoaded();
    DeviceEntry e = it.value();
    e.password = m_secrets.value(e.id);
    return e;
}

bool DeviceConfigStore::hasPassword(const QString &id) const
{
    if (id.isEmpty())
        return false;
    ensureSecretsLoaded();
    return !m_secrets.value(id).isEmpty();
}

QString DeviceConfigStore::resolvedPassword(const QString &id) const
{
    if (id.isEmpty())
        return QString();
    ensureSecretsLoaded();
    return m_secrets.value(id);
}

void DeviceConfigStore::setPassword(const QString &id, const QString &password)
{
    if (id.isEmpty())
        return;
    if (password.isEmpty())
        m_secrets.remove(id);
    else
        m_secrets.insert(id, password);
}

QHash<QString, QString> DeviceConfigStore::secretsSnapshot() const
{
    return m_secrets;
}

void DeviceConfigStore::restoreSecrets(const QHash<QString, QString> &snapshot)
{
    m_secrets = snapshot;
}

void DeviceConfigStore::invalidateSecretCache()
{
    m_secrets.clear();
    m_secretsLoaded = false;
}

bool DeviceConfigStore::flushSecrets(QString *errorOut) const
{
    // 只保留仍有设备引用的 id：设备删掉之后密码不该继续留在钥匙串里。
    QSet<QString> live;
    for (const DeviceEntry &e : m_devices)
        live.insert(e.id);

    QJsonObject obj;
    for (auto it = m_secrets.constBegin(); it != m_secrets.constEnd(); ++it) {
        if (!it.value().isEmpty() && live.contains(it.key()))
            obj.insert(it.key(), it.value());
    }

    if (obj.isEmpty()) {
        // 一条密码都没有 → 删掉条目而不是存一个空 JSON。
        // deleteSecret 在条目本就不存在时返回 false，这不是错误。
        Secrets::deleteSecret(secretService(), secretAccount());
        return true;
    }
    const QString blob =
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    return Secrets::storeSecret(secretService(), secretAccount(), blob, errorOut);
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
    // pickle（Python 版的 config.dat）里的密码必然是明文，因此这条路径读进来的
    // 配置一律「需要迁移」。
    m_inlinePasswords = true;
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
        // pickle 没有 id 的概念，一律新分配；密码搬进密码表，保持
        //「m_devices 里不带密码」的不变量。addDevice 两件事都做了。
        addDevice(e);
    }
    return true;
}

QJsonArray DeviceConfigStore::toJsonArray(bool withSecrets, bool withIds) const
{
    QJsonArray arr;
    for (const DeviceEntry &e : m_devices) {
        QJsonObject o;
        if (withIds)
            o[QStringLiteral("id")]   = e.id;
        o[QStringLiteral("name")]     = e.name;
        o[QStringLiteral("username")] = e.username;
        // 密码只在迁移窗口期内写（inlinePasswords()），迁移完成后这一行不再执行。
        // 注意取值来源是密码表而不是 e.password —— 后者按不变量恒为空。
        if (withSecrets)
            o[QStringLiteral("password")] = m_secrets.value(e.id);
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
        o[QStringLiteral("rxImplicitCr")] = e.rxImplicitCr;
        // TCP/Telnet 字段（同上，非 TCP/Telnet 条目也一并写出）。
        o[QStringLiteral("telnetNegotiate")] = e.telnetNegotiate;
        o[QStringLiteral("termType")]        = e.termType;
        o[QStringLiteral("autoLogin")]       = e.autoLogin;
        arr.append(o);
    }
    return arr;
}

bool DeviceConfigStore::saveJson(const QString &jsonPath, QString *errorOut) const
{
    const QJsonArray arr = toJsonArray(m_inlinePasswords, /*withIds=*/true);
    // 走 writeSecure：原子写 + 0600。devices.json 里有用户名/主机/端口，
    // 迁移完成前还有明文密码——默认的 0644 等于把它摊给同机所有用户。
    return ConfigUtil::writeSecure(
        jsonPath, QJsonDocument(arr).toJson(QJsonDocument::Compact), errorOut);
}

bool DeviceConfigStore::exportJson(const QString &jsonPath, QString *errorOut) const
{
    // 导出物要给人拷来拷去，因此无论迁移状态如何都不带密码；
    // id 也去掉——它是本机钥匙串的索引，带到另一台机器上只会撞车。
    const QJsonArray arr = toJsonArray(/*withSecrets=*/false, /*withIds=*/false);
    return ConfigUtil::writeSecure(
        jsonPath, QJsonDocument(arr).toJson(QJsonDocument::Compact), errorOut);
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
    m_secrets.clear();
    m_secretsLoaded = false;
    // 旧格式判定：只要有任何一条带 password 键，就说明这份文件还没迁移过，
    // 保存时必须继续写明文，直到迁移确认密码已在钥匙串里。
    m_inlinePasswords = false;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        DeviceEntry e;
        e.id       = o[QStringLiteral("id")].toString();
        e.name     = o[QStringLiteral("name")].toString();
        e.username = o[QStringLiteral("username")].toString();
        e.host     = o[QStringLiteral("host")].toString();
        // 协议要先于端口解析：port 键缺失时的回落值取决于协议
        //（手改配置文件只写了 protocol=telnet 而没写 port 是常见情形，
        //  一律回落 22 会让它连到 SSH 端口上）。
        // RDP 字段（旧版 JSON 无这些键时回落到 ssh/ntlm 默认值）。
        // 对应Python: util.device_protocol 的容错判别
        const QString protocol = o[QStringLiteral("protocol")].toString();
        e.protocol = protocol.isEmpty() ? QStringLiteral("ssh") : protocol;
        e.port     = quint16(o[QStringLiteral("port")].toInt(defaultPortFor(e.protocol)));
        e.keyType  = o[QStringLiteral("keyType")].toString();
        e.keyFile  = o[QStringLiteral("keyFile")].toString();
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
        // 缺键按协议回落（同上面 port 的做法），不能一律用串口的 "cr"：
        // 手改配置只写 protocol=telnet 时，发 CR 而不是 CR LF 会让回车不生效。
        e.newlineMode = newline.isEmpty() ? defaultNewlineModeFor(e.protocol) : newline;
        e.localEcho = o[QStringLiteral("localEcho")].toBool(false);
        // 默认 true：本键是后加的，旧配置文件里没有。缺键时回落成 false 会让
        // 已保存的串口设备表现得和新建的不一样（阶梯输出），故回落成开启。
        e.rxImplicitCr = o[QStringLiteral("rxImplicitCr")].toBool(true);
        // TCP/Telnet 字段（旧版 JSON 无这些键时的回落值同上面的容错风格）。
        // telnetNegotiate 缺键回落 true：协商是 Telnet 的正常行为，回落成
        // false 会让保存过的设备表现得和新建的不一样（拿不到 NAWS/TTYPE）。
        e.telnetNegotiate = o[QStringLiteral("telnetNegotiate")].toBool(true);
        const QString termType = o[QStringLiteral("termType")].toString();
        e.termType = termType.isEmpty() ? QStringLiteral("xterm-256color") : termType;
        // autoLogin 缺键回落 false：自动送密码是需要用户显式开启的行为。
        e.autoLogin = o[QStringLiteral("autoLogin")].toBool(false);
        if (e.name.isEmpty())
            continue;
        // id 缺失（旧格式）就地补一个。此刻它还没落盘——由迁移的 pass-1 负责
        // 持久化。若迁移没跑成，inlinePasswords 保持 true，下次保存连同 id 和
        // 明文一起写出去，什么都不会丢。
        if (e.id.isEmpty())
            e.id = newDeviceId();
        if (o.contains(QStringLiteral("password"))) {
            m_inlinePasswords = true;   // 这份文件是旧格式
            const QString pw = o[QStringLiteral("password")].toString();
            if (!pw.isEmpty())
                m_secrets.insert(e.id, pw);
        }
        m_devices.insert(e.name, e);   // e.password 恒为空，不变量成立
    }
    return true;
}

} // namespace cubeshell
