#pragma once

// TcpBridge.h — TCP/Telnet 字节流 ↔ qtermwidget Session 的桥接。
//
// 结构对照 SerialBridge（core/serial/SerialBridge.h）：同样没有读线程
//（QTcpSocket::readyRead 就是主线程事件循环里的异步回调），同样不做
// OSC7 / MFA / AI 哨兵过滤（对端可能是个没有 shell 的裸 TCP 服务）。
//
// 与 SerialBridge 的三点不同：
//   1. 接 Session::terminalSizeApplied —— Telnet 有 NAWS（RFC 1073），
//      窗口尺寸要同步给对端，串口没有这个概念。
//   2. 收发两个方向各多一层 TelnetProtocol 处理（IAC 剥离 / IAC 转义）。
//   3. 可选的自动登录：匹配 login:/Password: 提示自动发送凭据。
//
// 无条件编译。

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "net/TcpClient.h"

namespace Konsole { class Session; }

namespace cubeshell {

class TelnetProtocol;

class TcpBridge : public QObject {
    Q_OBJECT
public:
    // 自动登录状态机。做成公开 enum 是为了单测能直接断言状态迁移。
    enum class LoginPhase {
        Idle,       // 未启用自动登录
        WaitUser,   // 等 login:/Username: 提示
        SentUser,   // 已发用户名，等 Password: 提示
        SentPass,   // 已发密码
        Done,       // 收工（成功、超时或无需登录），此后不再匹配
    };
    Q_ENUM(LoginPhase)

    // session: 终端所附着的 qtermwidget Session。
    // client:  TCP 客户端（可以还没连上，接线与连接状态无关）。
    TcpBridge(Konsole::Session *session, TcpClient *client, QObject *parent = nullptr);
    ~TcpBridge() override;

    void start();
    void stop();

    // 运行时可改的终端行为（不需要重连）。
    void setNewlineMode(TcpSettings::NewlineMode mode) { m_newlineMode = mode; }
    TcpSettings::NewlineMode newlineMode() const { return m_newlineMode; }
    void setLocalEcho(bool enabled) { m_localEcho = enabled; }
    bool localEcho() const { return m_localEcho; }
    void setRxImplicitCr(bool enabled) { m_rxImplicitCr = enabled; }
    bool rxImplicitCr() const { return m_rxImplicitCr; }

    // 按 settings 配置协议层与自动登录。应在 client 建连前调用；
    // 连上之后由 onConnected() 复位状态机并开始匹配提示。
    void applySettings(const TcpSettings &settings);

    TelnetProtocol *protocol() const { return m_telnet; }
    LoginPhase loginPhase() const { return m_loginPhase; }

    // 把一段文本当作用户输入发出去（含换行转换与 IAC 转义）。
    void sendText(const QString &text);

    // --- 自动登录的提示词匹配（静态纯函数，便于单测） ---
    //
    // tail 是最近收到的一小段净数据。只看尾部而非整个会话缓冲，是因为
    // 提示符一定紧贴在输入位置之前；扫全量会把 banner 里出现的
    // "password" 字样误判成提示。
    static bool looksLikeLoginPrompt(const QByteArray &tail);
    static bool looksLikePasswordPrompt(const QByteArray &tail);

signals:
    // 供面板把字节回灌到终端（本地回显走这条路，与网络来的数据同一入口）。
    void echoData(const QByteArray &data);
    // 自动登录状态变化，供状态栏提示。
    void loginPhaseChanged(cubeshell::TcpBridge::LoginPhase phase);
    // 本地回显被协议层改动（服务端 WILL ECHO 时自动关闭），供面板同步复选框。
    // 只在 bridge 自己改动时发出，setLocalEcho() 不发——那是面板发起的，
    // 再发回去会绕成环。
    void localEchoChanged(bool enabled);

private slots:
    void onNetData(const QByteArray &data);
    void onProtocolResponse(const QByteArray &bytes);
    void onServerEchoChanged(bool serverEchoes);
    void onConnected();
    void onDisconnected();
    void onTerminalSizeApplied(int columns, int rows);

private:
    void onEmulationSendData(const char *data, int length);
    void writeToTerminal(const QByteArray &data);
    // 把已做完换行转换的字节写到链路（telnet 时先做 IAC 转义）。
    void writeToNetwork(const QByteArray &data);
    // 在净数据上推进自动登录状态机。
    void advanceAutoLogin(const QByteArray &clean);
    void setLoginPhase(LoginPhase phase);

    Konsole::Session *m_session = nullptr;
    TcpClient *m_client = nullptr;
    TelnetProtocol *m_telnet = nullptr;   // 始终存在；裸 TCP 模式下不参与收发

    bool m_telnetMode = true;
    TcpSettings::NewlineMode m_newlineMode = TcpSettings::NewlineMode::CrLf;
    bool m_localEcho = false;
    bool m_rxImplicitCr = true;
    bool m_prevWasCr = false;   // 跨 readyRead 的边界状态，见 net/LineEndings.h
    bool m_running = false;

    // --- 自动登录 ---
    bool m_autoLogin = false;
    QString m_loginUser;
    QString m_loginPassword;
    LoginPhase m_loginPhase = LoginPhase::Idle;
    // 最近收到的净数据尾巴，用于跨包匹配被切断的提示符（"Passw" | "ord: "）。
    QByteArray m_promptTail;
    // 自动登录的时间窗。超时后永久停用，避免会话中途把密码打进业务输出——
    // 比如 tail 一个含 "password:" 字样的日志文件。
    QElapsedTimer m_loginTimer;
};

} // namespace cubeshell
