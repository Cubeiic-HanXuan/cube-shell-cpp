#pragma once

// TelnetProtocol.h — Telnet 选项协商（IAC）状态机。
//
// RFC 854（Telnet 协议）/ 855（选项规范）/ 856（BINARY）/ 857（ECHO）/
// 858（SGA）/ 1073（NAWS）/ 1091（TERMINAL-TYPE）。
//
// 职责：从字节流里剥出 IAC 命令并生成应答，把剩下的净数据交给终端。
// 不碰 socket（应答通过 sendResponse() 信号交给 TcpClient），不碰终端渲染。
//
// 跨调用状态是本类的核心难点。TCP 分段会把 IAC 序列从任意位置切开——一个
// `IAC DO NAWS` 完全可能拆成两次 readyRead，一段子协商能跨十几个包。因此
// processIncoming() 必须是可重入喂食的增量解析器（对照 core/ai/SseClient.h
// 的 SseEventParser::feed，以及 net/LineEndings.h 的 prevWasCr 跨块状态）。
//
// 协商策略是保守的：只应答，不主动发起。老设备的 Telnet 实现质量参差，
// 主动 DO/WILL 一串选项容易触发协商环路；等对端问、我们答，最稳。
// 认识的选项之外一律拒绝（DONT/WONT），不留半开状态。

#include <QByteArray>
#include <QObject>
#include <QSet>

namespace cubeshell {

class TelnetProtocol : public QObject {
    Q_OBJECT
public:
    // --- RFC 854 命令码 ---
    static constexpr unsigned char IAC  = 255;   // Interpret As Command
    static constexpr unsigned char DONT = 254;
    static constexpr unsigned char DO   = 253;
    static constexpr unsigned char WONT = 252;
    static constexpr unsigned char WILL = 251;
    static constexpr unsigned char SB   = 250;   // 子协商开始
    static constexpr unsigned char GA   = 249;   // Go Ahead
    static constexpr unsigned char EL   = 248;
    static constexpr unsigned char EC   = 247;
    static constexpr unsigned char AYT  = 246;   // Are You There
    static constexpr unsigned char AO   = 245;
    static constexpr unsigned char IP   = 244;   // Interrupt Process
    static constexpr unsigned char BRK  = 243;
    static constexpr unsigned char DM   = 242;   // Data Mark
    static constexpr unsigned char NOP  = 241;
    static constexpr unsigned char SE   = 240;   // 子协商结束

    // --- 选项码 ---
    static constexpr unsigned char OPT_BINARY = 0;    // RFC 856
    static constexpr unsigned char OPT_ECHO   = 1;    // RFC 857
    static constexpr unsigned char OPT_SGA    = 3;    // RFC 858 Suppress Go Ahead
    static constexpr unsigned char OPT_TTYPE  = 24;   // RFC 1091 Terminal Type
    static constexpr unsigned char OPT_NAWS   = 31;   // RFC 1073 窗口尺寸

    // TERMINAL-TYPE 子协商命令
    static constexpr unsigned char TTYPE_IS   = 0;
    static constexpr unsigned char TTYPE_SEND = 1;

    explicit TelnetProtocol(QObject *parent = nullptr);

    // 重连时清空全部状态（半解析的序列、已协商选项、窗口尺寸缓存）。
    void reset();

    // 是否生成协商应答。false 时仍然剥除 IAC 序列（不剥的话 0xFF 开头的控制
    // 字节会当成乱码上屏），只是不回任何 WILL/WONT/DO/DONT。
    void setNegotiationEnabled(bool enabled) { m_negotiate = enabled; }
    bool negotiationEnabled() const { return m_negotiate; }

    // TERMINAL-TYPE 子协商上报的终端类型。须在连接前设置。
    void setTerminalType(const QByteArray &type) { m_termType = type; }
    QByteArray terminalType() const { return m_termType; }

    // 收方向：剥掉 IAC 序列、把 IAC IAC 还原成单个 0xFF，返回给终端的净数据。
    // 协商应答通过 sendResponse() 发出，不混在返回值里。
    // 可任意分块喂入：解析状态跨调用保持。
    QByteArray processIncoming(const QByteArray &in);

    // 发方向：把数据里的 0xFF 转义成 IAC IAC（RFC 854 强制，否则对端会把
    // 用户输入的 0xFF 当成命令前导）。静态纯函数，便于单测。
    static QByteArray escapeOutgoing(const QByteArray &in);

    // 记录并（协商通过时）上报窗口尺寸。对端还没 DO NAWS 时只记不发，
    // 等协商完成后由 onNawsAgreed 补发一次——终端在建连前就已经有尺寸了。
    void sendWindowSize(int columns, int rows);

    // 服务端是否已声明由它回显（WILL ECHO）。true 时本地回显必须关掉，
    // 否则每个字符出现两次。
    bool serverEcho() const { return m_serverEcho; }

signals:
    // 协商应答/子协商数据，直接交 TcpClient::write（已是最终字节）。
    void sendResponse(const QByteArray &bytes);
    // 服务端回显状态变化，供面板自动勾掉"本地回显"。
    void serverEchoChanged(bool serverEchoes);

private:
    // 增量解析的状态。命名对照 RFC 854 的术语。
    enum class Phase {
        Data,          // 普通数据
        Iac,           // 收到 IAC，等命令字节
        Negotiate,     // 收到 WILL/WONT/DO/DONT，等选项字节
        SubOptionCode, // 收到 IAC SB，等选项码字节
        SubData,       // 子协商载荷中
        SubIac,        // 子协商载荷里收到 IAC，等下一字节（SE 或转义的 IAC）
    };

    void handleWill(unsigned char option);
    void handleWont(unsigned char option);
    void handleDo(unsigned char option);
    void handleDont(unsigned char option);
    void handleSubNegotiation();       // 处理 m_subBuffer 里攒齐的一段子协商

    void respond(unsigned char command, unsigned char option);
    void sendTerminalType();
    void emitWindowSize();

    Phase m_phase = Phase::Data;
    unsigned char m_pendingCommand = 0;   // Negotiate 阶段暂存的 WILL/WONT/DO/DONT
    unsigned char m_subOption = 0;        // 当前子协商的选项码
    QByteArray m_subBuffer;               // 子协商载荷（不含 IAC SB <opt> 与 IAC SE）

    bool m_negotiate = true;
    QByteArray m_termType = QByteArrayLiteral("xterm-256color");
    bool m_serverEcho = false;

    // 已应答过的协商，用于抑制重复应答。RFC 855 要求：状态没变化时不得回复，
    // 否则两端会来回 DO/WILL 到天荒地老（经典的 negotiation loop）。
    QSet<int> m_agreedWill;   // 我方已 WILL 的选项
    QSet<int> m_agreedDo;     // 我方已 DO 的选项
    QSet<int> m_refusedWill;  // 我方已 WONT 的选项
    QSet<int> m_refusedDo;    // 我方已 DONT 的选项

    bool m_nawsAgreed = false;
    int m_columns = 0;
    int m_rows = 0;
};

} // namespace cubeshell
