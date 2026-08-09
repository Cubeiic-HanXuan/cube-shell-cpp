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

// 协议 → 默认端口。ssh=22 / rdp=3389 / telnet=23 / tcp=23 / 其他=22。
//
// 这个知识点原先重复在三处（DeviceEntry::hostPort、添加设备对话框的协议切换、
// UrlHandler 的 rdp 解析），加协议时容易漏改一处，故收敛到这里。
quint16 defaultPortFor(const QString &protocol);

// 协议 → 缺省发送换行符（"cr" | "lf" | "crlf"）。
// serial=cr（RS-232 设备惯例）/ telnet=crlf（RFC 854 规定的行尾）/
// tcp=lf（对端多是 Unix 侧服务，收到 CR 会回显成 ^M）/ 其他=cr。
//
// 存在的理由同 defaultPortFor：newlineMode 是串口与 TCP/Telnet 共用的字段，
// 配置文件缺这个键时不能一律按串口默认回落，否则手改出来的 telnet 条目回车
// 不生效（发 CR 而不是 CR LF）。注意不能把 "cr" 在映射层纠成 CRLF——CR 是
// 下拉框里用户可以主动选的一项，那样改会让显式选择失效。
QString defaultNewlineModeFor(const QString &protocol);

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
    QString protocol = QStringLiteral("ssh");  // "ssh" | "rdp" | "serial" | "tcp" | "telnet"
    QString domain;    // Windows 域（RDP 专用，对应Python RDP dict["domain"]）
    QString auth = QStringLiteral("ntlm");  // RDP 认证方式："ntlm" | "plain"（对应Python RDP dict["auth"]）

    // 串口字段（protocol == "serial" 时有效）。Python 版无对应实现，为 C++ 侧新增。
    // 沿用 RDP 的做法：各协议字段平铺在同一个 DeviceEntry 上，用不到的留空。
    // 存字符串/int 而非 QSerialPort 枚举，是为了让 config 层不依赖 Qt6::SerialPort
    //（CUBESHELL_WITH_SERIAL=OFF 时本结构体仍要能编译），映射在 UI 层做。
    QString portName;                               // "COM3" | "/dev/ttyUSB0"
    int     baudRate    = 115200;
    int     dataBits    = 8;                        // 5|6|7|8
    QString parity      = QStringLiteral("none");   // none|even|odd|mark|space
    QString stopBits    = QStringLiteral("1");      // 1|1.5|2
    QString flowControl = QStringLiteral("none");   // none|hardware|software
    QString newlineMode = QStringLiteral("cr");     // cr|lf|crlf
    bool    localEcho   = false;
    bool    rxImplicitCr = true;                    // 接收时给孤立的 LF 补 CR（默认开启）

    // TCP/Telnet 字段（protocol == "tcp" | "telnet" 时有效）。Python 版无对应实现。
    // host/port/username/password 与 newlineMode/localEcho/rxImplicitCr 直接复用
    // 上面的通用字段——后三项本就是协议中立的终端行为，不是串口专属。
    bool    telnetNegotiate = true;                              // 是否做 IAC 选项协商
    QString termType = QStringLiteral("xterm-256color");         // TERMINAL-TYPE 上报值
    bool    autoLogin = false;                                   // 匹配提示自动送凭据

    bool isRdp() const { return protocol == QLatin1String("rdp"); }
    bool isSerial() const { return protocol == QLatin1String("serial"); }
    bool isTcp() const { return protocol == QLatin1String("tcp"); }
    bool isTelnet() const { return protocol == QLatin1String("telnet"); }
    // SSH 是历史上的隐式兜底（protocol 为空的旧配置也算 SSH），故不写成
    // protocol == "ssh" 而是"不是其他任何一种"。
    bool isSsh() const { return !isRdp() && !isSerial() && !isTcp() && !isTelnet(); }

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
