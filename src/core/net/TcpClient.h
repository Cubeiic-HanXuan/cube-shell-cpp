#pragma once

// TcpClient.h — TCP / Telnet 会话客户端。
//
// Python 版无对应实现，属 C++ 侧新增的第四、五种会话类型（与 SSH / RDP /
// Serial 并列）。形态取 SerialClient：QTcpSocket 的 readyRead 天生异步，
// 不需要 SshClient 那样的阻塞读线程，全部在主线程的事件循环里跑。
//
// 职责边界：本类只管"建连 / 收发字节 / 落日志"，不认识 Telnet 的 IAC 协商
// （那在 TelnetProtocol 里），也不碰终端渲染（那在 TcpBridge 里）。
// 一个 telnet 会话与一个裸 TCP 会话在本层完全一样，只有 settings().mode 不同。
//
// 无条件编译：Qt6::Network 是顶层必需组件，鸿蒙上同样可用。

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpSocket;
class QTimer;
class QFile;

namespace cubeshell {

// 连接参数。
struct TcpSettings {
    // "telnet"（带 IAC 选项协商）| "tcp"（裸字节管道，等价 PuTTY 的 Raw 模式）。
    // 用字符串而非 enum，与 DeviceEntry::protocol 的持久化形态直接对齐。
    QString mode = QStringLiteral("telnet");

    QString host;
    quint16 port = 23;

    // 连接超时。QTcpSocket 自身的连接超时各平台不一致（Linux 上可能拖到
    // 2 分钟的内核 SYN 重传上限），故自己起定时器兜。
    int connectTimeoutMs = 15000;

    // --- 终端行为（不影响链路，只影响 TcpBridge 的收发处理） ---

    // 发送时如何转换终端产生的换行。语义同串口，取值集合也一样。
    // 默认 CrLf：RFC 854 规定 Telnet 的行尾就是 CR LF。
    enum class NewlineMode { Cr, Lf, CrLf };
    NewlineMode newlineMode = NewlineMode::CrLf;

    // 接收时给孤立的 LF 补一个 CR。默认 true，理由同串口：对端可能是不守
    // 规矩的嵌入式实现，发裸 \n 时屏幕会出现阶梯状输出。
    bool rxImplicitCr = true;

    // 本地回显。Telnet 服务端通常会 WILL ECHO 自己回显，届时 TcpBridge 会
    // 自动把这项关掉；裸 TCP 对端（nc 之类）不回显，需要手动打开。
    bool localEcho = false;

    // --- Telnet 专用（mode == "telnet" 时有效） ---

    // 是否做 IAC 选项协商。关掉即退化成裸 TCP 的收发行为，但仍会剥除对端
    // 发来的 IAC 序列（否则 0xFF 开头的控制字节会当成乱码上屏）。
    bool negotiate = true;

    // TERMINAL-TYPE(24) 子协商上报的终端类型。
    QByteArray termType = QByteArrayLiteral("xterm-256color");

    // 自动登录：连上后匹配 login:/Password: 提示自动发送下面的凭据。
    // 默认关闭——提示词因设备而异（网络设备常是 Username:），误判会把密码
    // 打到错误的地方并回显在屏幕上。
    bool autoLogin = false;
    QString username;
    QString password;

    bool isTelnet() const { return mode == QLatin1String("telnet"); }

    // 状态栏展示用："telnet 10.0.0.1:23"。
    QString displayTarget() const;
};

class TcpClient : public QObject {
    Q_OBJECT
public:
    // 比 SerialClient::State 多一个 Connecting —— 串口打开是同步的，
    // TCP 建连要等三次握手（或 DNS 解析）。
    enum class State { Disconnected, Connecting, Connected };
    Q_ENUM(State)

    explicit TcpClient(QObject *parent = nullptr);
    ~TcpClient() override;

    State state() const { return m_state; }
    TcpSettings settings() const { return m_settings; }
    bool isOpen() const;

    // 发起连接。参数非法时立即发 errorOccurred() 并返回 false；否则进入
    // Connecting 态，成功发 connected()，失败发 errorOccurred() + disconnected()。
    // 注意返回 true 只表示"已发起"，不表示已连上。
    bool connectToHost(const TcpSettings &settings);
    // 主动断开。已断开时是空操作（不会重复发 disconnected()）。
    void disconnectFromHost();

    // 写裸字节。未连接时丢弃并返回 false。
    // 注意：换行转换与 IAC 转义都由 TcpBridge 负责，到这里的字节已是最终形态。
    bool write(const QByteArray &data);

    // --- 会话日志 ---
    // 把收到的原始字节 append 写入文件（发送的字节不记，避免与对端回显重复）。
    // path 为空 = 停止录制并关闭文件。返回是否成功打开。
    bool setLogFile(const QString &path);
    bool isLogging() const { return m_logFile != nullptr; }
    QString logFilePath() const;

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void stateChanged(cubeshell::TcpClient::State state);
    // 收到网络数据（已落日志，未做任何协议解析或字符转换）。
    void dataReceived(const QByteArray &data);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onConnectTimeout();

private:
    void setState(State state);
    void closeLogFile();

    QTcpSocket *m_socket = nullptr;
    TcpSettings m_settings;
    State m_state = State::Disconnected;

    QTimer *m_connectTimer = nullptr;
    // 连接阶段失败时只发一次 errorOccurred：abort() 会再触发一轮
    // stateChanged/errorOccurred，不去重的话状态栏会闪两条消息。
    bool m_errorReported = false;

    QFile *m_logFile = nullptr;
};

} // namespace cubeshell
