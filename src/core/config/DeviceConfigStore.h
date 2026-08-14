#pragma once

// DeviceConfigStore.h — device/connection configuration persistence.
//
// Reads the existing Python-pickle config.dat (via PickleReader) and writes
// new/modified configs as JSON, per the chosen migration strategy
// ("read pickle, write JSON"). The device model mirrors cube-shell's
// config.dat entries: name -> [user, password, host, keyType, keyFile].

#include <QHash>
#include <QJsonArray>
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
    // 稳定标识，与 name 解耦。钥匙串里就是按它索引密码的。
    //
    // 必须有这个字段：m_devices 以 name 为键，重命名走的是 remove + add，
    // 若密码也挂在 name 上，改个名字密码就成了孤儿。id 一旦分配终生不变。
    // 空串表示「尚未分配」——旧配置文件读进来就是这样，由迁移补齐。
    QString id;

    QString name;
    QString username;
    // 明文密码，**仅存在于内存**，绝不写进 devices.json（迁移完成后）。
    // 从 find()/devices() 拿到的条目此字段为空；需要密码请用
    // DeviceConfigStore::resolved()，它会按需解锁钥匙串。
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

    // 该条目由 jms:// URL 现场构造，连的是 JumpServer 跳板机（koko）而非资产本身。
    // **纯运行时标记，不参与 JSON 序列化**（toJsonArray/loadJson 都是逐字段枚举，
    // 不会顺带写盘）——它描述的是"这次连接怎么来的"，不是设备的持久属性；
    // 侧栏里保存的设备永远为 false。
    // 用途：koko 的 SFTP 子系统是与资产无关的虚拟命名空间，面板要据此给出
    // 准确说明而不是抛 libssh2 原始错误（见 SftpBrowserWidget::setBastionProxied）。
    bool viaBastion = false;

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

    // 存一个条目。id 为空时自动分配（兜底——正常路径由对话框在构造时就赋好）。
    //
    // entry.password 非空 → 一并写入密码表；为空 → **不动**已存密码
    //（编辑时没重新输入密码是常态，为空就抹掉会让人一改端口就丢密码）。
    // 要真的清除密码，显式调 setPassword(id, "")。
    // 无论哪种情况，落进 m_devices 的副本 password 一律清空。
    void addDevice(const DeviceEntry &entry);

    // QHash::remove 在 Qt6 返回 bool（Qt5 是 int），直接透传；写 > 0 会触发 MSVC C4804。
    bool removeDevice(const QString &name) { return m_devices.remove(name); }
    const DeviceEntry *find(const QString &name) const;
    int count() const { return m_devices.size(); }
    bool isEmpty() const { return m_devices.isEmpty(); }
    QStringList names() const { return m_devices.keys(); }

    // --- 密码 ---------------------------------------------------------------
    //
    // 不变量：m_devices 里的条目**永远不带密码**。密码只在 m_secrets 里，按 id 索引。
    // 于是「这里没有密码」是编译期可见的事实而不是运行期惊喜：设备树、搜索、
    // 分组都不会碰钥匙串，只有真要发起连接或编辑时才碰。

    // find() 的值副本 + 填好密码。首次调用会解锁钥匙串（可能弹一次授权框）。
    // 找不到该名字时返回一个 name 为空的默认条目。
    DeviceEntry resolved(const QString &name) const;

    // 该 id 是否存有密码。用于「编辑时不必重输密码」——只问在不在，不取值。
    bool hasPassword(const QString &id) const;

    // 按 id 取密码，必要时先从钥匙串加载（与 resolved() 同一套加载语义）。
    // 找不到 / 未存返回空串。用途：编辑设备时「测试连接」要用真实凭据，
    // 而密码框此时是空的（占位符"留空则不修改"），调用方只有 id 没有 name。
    QString resolvedPassword(const QString &id) const;

    // 显式设置/清除密码（空串即清除）。要落盘还得调 flushSecrets()。
    void setPassword(const QString &id, const QString &password);

    // 只看内存表，**绝不碰钥匙串**。用于导入：外部文件带进来的密码已经在
    // 内存里了，此时若走 resolved() 会顺带去解锁本机钥匙串，既没必要，
    // 万一 id 撞上还会取到别人的密码。
    QString cachedPassword(const QString &id) const { return m_secrets.value(id); }

    // 把整张密码表写回钥匙串（单条聚合，见 secretAccount()）。
    bool flushSecrets(QString *errorOut = nullptr) const;

    // 生成新的设备 id。
    static QString newDeviceId();

    // --- 迁移闸门 -----------------------------------------------------------

    // saveJson 是否仍把 password 写进 JSON。
    //
    // 这是整个方案里最要紧的一个开关。迁移窗口期内（旧的明文还在文件里、
    // 新的副本还没确认进钥匙串）任何一次保存若按新格式写，就会在密码只剩
    // 一个副本时把它抹掉。所以：只有「存进钥匙串 + 读回逐条校验通过」之后，
    // 才允许翻成 false。
    //
    // 值由载入决定，不靠调用方记得设：loadJson 见到 password 键 → true，
    // 没见到 → false；pickle load() 恒为 true（pickle 里必然是明文）。
    // 全新安装两个都没走过，保持默认 false，于是第一次保存就是新格式。
    bool inlinePasswords() const { return m_inlinePasswords; }
    void setInlinePasswords(bool on) { m_inlinePasswords = on; }

    // 这份配置是否还没迁移过。等价于 inlinePasswords()——文件里还有 password
    // 键就说明是旧格式。取这个名字是为了让调用点读起来是意图而不是实现。
    // 崩溃后重跑也靠它：pass-1 写完但没来得及存钥匙串时，文件里仍有 password
    // 键，下次启动照样判定为「需要迁移」。
    bool needsMigration() const { return m_inlinePasswords; }

    // 内存密码表的快照 / 恢复。迁移的读回校验要先清空再从钥匙串重读，
    // 校验失败时得原样放回去，否则这一趟就把内存里的明文也弄丢了。
    QHash<QString, QString> secretsSnapshot() const;
    void restoreSecrets(const QHash<QString, QString> &snapshot);
    // 丢掉缓存，强制下次访问重新读钥匙串（读回校验用）。
    void invalidateSecretCache();

    // 导出专用：不含密码，也不含 id
    //（id 是本机钥匙串的索引，带出去只会在别的机器上撞车）。
    bool exportJson(const QString &jsonPath, QString *errorOut = nullptr) const;

    // 钥匙串坐标。单条聚合：一个条目装下全部设备密码的 {id: password} JSON。
    // 好处是升级后（ad-hoc 签名导致 ACL 失配）只弹一次授权，而不是每设备一次。
    static QString secretService();
    static QString secretAccount();

private:
    // 序列化成 JSON 数组。withSecrets/withIds 供导出路径关掉。
    QJsonArray toJsonArray(bool withSecrets, bool withIds) const;
    // 按需从钥匙串加载整张表（幂等；已在内存里的条目优先，不被覆盖）。
    void ensureSecretsLoaded() const;

    QHash<QString, DeviceEntry> m_devices;

    // id -> 明文密码。仅内存，进程退出即消失。
    mutable QHash<QString, QString> m_secrets;
    mutable bool m_secretsLoaded = false;
    // 默认 false：全新安装既不走 loadJson 也不走 load()，第一次保存就该是新格式。
    // 旧配置文件由 loadJson/load 自己把它置 true。
    bool m_inlinePasswords = false;
};

} // namespace cubeshell
