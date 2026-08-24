// TcpBridge.cpp — TCP/Telnet ↔ 终端桥接。见 TcpBridge.h。

#include "net/TcpBridge.h"

#include <QDebug>
#include <QRegularExpression>

#include "Emulation.h"
#include "Session.h"
#include "net/LineEndings.h"
#include "net/TelnetProtocol.h"

namespace cubeshell {

namespace {

// 自动登录的时间窗。连上后超过这个时间还没走完登录流程就永久放弃。
// 30 秒足够任何 telnetd 打完 banner 并给出提示符；再长下去风险就大于收益了
// （用户可能已经进了 shell 在跑 tail，输出里的 "password:" 会被误判）。
constexpr qint64 kAutoLoginWindowMs = 30000;

// 提示符匹配只看尾部这么多字节。提示符一定紧贴输入位置之前，扫全量会把
// banner 或 MOTD 里的 "password" 字样误判成提示。
constexpr int kPromptTailBytes = 256;

// 换行模式的两套 enum 之间做映射。TcpSettings 与 cubeshell::NewlineMode 值域
// 相同但是两个类型（避免协议头文件互相 include），故显式转换。
NewlineMode toCommonNewline(TcpSettings::NewlineMode mode)
{
    switch (mode) {
    case TcpSettings::NewlineMode::Lf:   return NewlineMode::Lf;
    case TcpSettings::NewlineMode::CrLf: return NewlineMode::CrLf;
    case TcpSettings::NewlineMode::Cr:   break;
    }
    return NewlineMode::Cr;
}

} // namespace

TcpBridge::TcpBridge(Konsole::Session *session, TcpClient *client, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_client(client)
    , m_telnet(new TelnetProtocol(this))
{
    connect(m_telnet, &TelnetProtocol::sendResponse,
            this, &TcpBridge::onProtocolResponse);
    connect(m_telnet, &TelnetProtocol::serverEchoChanged,
            this, &TcpBridge::onServerEchoChanged);
}

TcpBridge::~TcpBridge()
{
    stop();
}

void TcpBridge::applySettings(const TcpSettings &settings)
{
    m_telnetMode = settings.isTelnet();
    m_newlineMode = settings.newlineMode;
    m_localEcho = settings.localEcho;
    m_rxImplicitCr = settings.rxImplicitCr;

    m_telnet->setNegotiationEnabled(settings.negotiate);
    m_telnet->setTerminalType(settings.termType);

    // 自动登录只在 telnet 模式下有意义——裸 TCP 对端不一定有登录概念，
    // 往一个 Redis 端口上打用户名密码只会污染协议。
    m_autoLogin = m_telnetMode && settings.autoLogin
                  && !settings.username.isEmpty();
    m_loginUser = settings.username;
    m_loginPassword = settings.password;
    setLoginPhase(m_autoLogin ? LoginPhase::WaitUser : LoginPhase::Idle);
}

void TcpBridge::start()
{
    using Konsole::Emulation;

    if (m_running || !m_session || !m_client)
        return;

    // 0. 断开 Session 构造时自建的 emulation→Pty 连接。
    //    不做这步的话按键会同时送到本地空闲 PTY 和网络，PTY 的行规程会把
    //    字符回显回来（receivedData → onReceiveBlock），屏幕上每个字符出现
    //    两次。与 SerialBridge / SshBridge 的第 0 步同因同解。
    m_session->runEmptyPTY();

    // 1. 用户按键 → 网络（换行转换 + IAC 转义 + 可选本地回显）。
    if (Emulation *emulation = m_session->emulation()) {
        connect(emulation, &Emulation::sendData,
                this, &TcpBridge::onEmulationSendData);
    }

    // 2. 网络数据 → 终端。
    connect(m_client, &TcpClient::dataReceived, this, &TcpBridge::onNetData);
    connect(m_client, &TcpClient::connected, this, &TcpBridge::onConnected);
    connect(m_client, &TcpClient::disconnected, this, &TcpBridge::onDisconnected);

    // 3. 回显/注入数据 → 终端。与网络来的数据共用同一个写入口。
    //    QueuedConnection：回显是在 sendData 回调里发出的，排队一拍避免
    //    在 Emulation 的信号栈里重入 onReceiveBlock。
    connect(this, &TcpBridge::echoData,
            m_session,
            [session = m_session](const QByteArray &data) {
                session->onReceiveBlock(data.constData(), int(data.size()));
            },
            Qt::QueuedConnection);

    // 4. 终端尺寸 → NAWS 子协商（RFC 1073）。
    //    这一步 SerialBridge 是明确跳过的（串口没有 pty 尺寸），Telnet 必须接：
    //    不接的话对端一直按它的默认 80x24 排版，vim/top 的画面会错行。
    connect(m_session, &Konsole::Session::terminalSizeApplied,
            this, &TcpBridge::onTerminalSizeApplied);

    m_running = true;
}

void TcpBridge::stop()
{
    if (!m_running)
        return;
    m_running = false;

    // 断开本对象参与的所有连接。析构顺序上 Session 可能先走，留着连接
    // 会在下一次 readyRead 时指向已销毁的 Session。
    if (m_session) {
        if (Konsole::Emulation *emulation = m_session->emulation())
            disconnect(emulation, nullptr, this, nullptr);
        disconnect(this, &TcpBridge::echoData, m_session, nullptr);
        disconnect(m_session, &Konsole::Session::terminalSizeApplied,
                   this, &TcpBridge::onTerminalSizeApplied);
    }
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
}

// --- 收方向 ---

void TcpBridge::onNetData(const QByteArray &data)
{
    // 1) Telnet 模式先过 IAC 状态机，剥出净数据。
    //    协商关闭时也要过：不剥的话 0xFF 开头的控制字节会当乱码上屏。
    QByteArray clean = m_telnetMode ? m_telnet->processIncoming(data) : data;
    if (clean.isEmpty())
        return;   // 整块都是协商序列，没有可显示的内容

    // 2) 自动登录在净数据上匹配（先于换行转换：提示符里的 \r\n 不影响匹配，
    //    但补出来的 CR 会让 tail 里多出无意义字节）。
    if (m_autoLogin)
        advanceAutoLogin(clean);

    // 3) 孤立 LF 补 CR。
    if (!m_rxImplicitCr) {
        // 关闭时连边界状态一起清掉，免得中途开启时带着上一次的残留判断。
        m_prevWasCr = false;
        writeToTerminal(clean);
        return;
    }
    writeToTerminal(applyRxImplicitCr(clean, m_prevWasCr));
}

void TcpBridge::writeToTerminal(const QByteArray &data)
{
    if (!m_session || data.isEmpty())
        return;
    m_session->onReceiveBlock(data.constData(), int(data.size()));
}

// --- 发方向 ---

void TcpBridge::onEmulationSendData(const char *data, int length)
{
    if (!m_running || !m_client || length <= 0)
        return;

    const QByteArray raw(data, length);
    const QByteArray converted = applyNewlineMode(raw, toCommonNewline(m_newlineMode));

    writeToNetwork(converted);

    // 本地回显：把转换后的字节回灌终端。用转换后的结果而不是原始 \r，
    // 是为了让屏幕上的换行表现与实际发出去的一致。
    // 注意回显的是**转换后、转义前**的字节——IAC 加倍是链路层的事，
    // 回显到屏幕上会多出一个 0xFF。
    if (m_localEcho)
        emit echoData(converted);
}

void TcpBridge::writeToNetwork(const QByteArray &data)
{
    if (!m_client || data.isEmpty())
        return;
    // RFC 854：数据里的 0xFF 必须加倍，否则对端把它当命令前导。
    m_client->write(m_telnetMode ? TelnetProtocol::escapeOutgoing(data) : data);
}

void TcpBridge::sendText(const QString &text)
{
    if (!m_running || !m_client)
        return;
    const QByteArray converted =
        applyNewlineMode(text.toUtf8(), toCommonNewline(m_newlineMode));
    writeToNetwork(converted);
    if (m_localEcho)
        emit echoData(converted);
}

void TcpBridge::onProtocolResponse(const QByteArray &bytes)
{
    // 协商应答已是最终字节（TelnetProtocol 内部自行处理过转义），
    // 不能再走 writeToNetwork —— 那会把 IAC 又加倍一遍。
    if (m_client)
        m_client->write(bytes);
}

void TcpBridge::onServerEchoChanged(bool serverEchoes)
{
    // 服务端声明由它回显 → 本地回显必须关，否则每个字符出现两次。
    // 反向（服务端 WONT ECHO）不自动开启：那通常意味着进了密码输入，
    // 此时开本地回显会把密码显示在屏幕上。
    if (serverEchoes && m_localEcho) {
        m_localEcho = false;
        emit localEchoChanged(false);
    }
}

// --- 连接状态 ---

void TcpBridge::onConnected()
{
    m_telnet->reset();
    m_prevWasCr = false;
    m_promptTail.clear();

    if (m_autoLogin) {
        m_loginTimer.start();
        setLoginPhase(LoginPhase::WaitUser);
    }

    // 连上就把当前终端尺寸交给协议层记着；等对端 DO NAWS 时会自动发出去。
    if (m_telnetMode && m_session) {
        const QSize sz = m_session->size();
        if (sz.width() > 0 && sz.height() > 0)
            m_telnet->sendWindowSize(sz.width(), sz.height());
    }
}

void TcpBridge::onDisconnected()
{
    setLoginPhase(m_autoLogin ? LoginPhase::WaitUser : LoginPhase::Idle);
    m_promptTail.clear();
}

void TcpBridge::onTerminalSizeApplied(int columns, int rows)
{
    if (m_telnetMode)
        m_telnet->sendWindowSize(columns, rows);
}

// --- 自动登录 ---

bool TcpBridge::looksLikeLoginPrompt(const QByteArray &tail)
{
    // 覆盖 telnetd 的 "login: "、网络设备的 "Username:"、中文固件的"用户名："。
    // 结尾允许有空格，也允许提示符后紧跟 ANSI 序列（部分设备会重绘光标）。
    static const QRegularExpression re(
        QStringLiteral("(?:login|user\\s?name|用户名)\\s*[:：]\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(QString::fromUtf8(tail)).hasMatch();
}

bool TcpBridge::looksLikePasswordPrompt(const QByteArray &tail)
{
    static const QRegularExpression re(
        QStringLiteral("(?:password|passcode|口令|密码)\\s*[:：]\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(QString::fromUtf8(tail)).hasMatch();
}

void TcpBridge::advanceAutoLogin(const QByteArray &clean)
{
    if (m_loginPhase == LoginPhase::Done || m_loginPhase == LoginPhase::Idle)
        return;

    // 超时即永久停用。放在匹配之前：宁可漏一次登录，也不能在用户已经进了
    // shell 之后还盯着输出找 "password:"。
    if (m_loginTimer.isValid() && m_loginTimer.elapsed() > kAutoLoginWindowMs) {
        setLoginPhase(LoginPhase::Done);
        return;
    }

    // 攒尾巴：提示符可能被 TCP 分段切成 "Passw" | "ord: " 两块。
    m_promptTail.append(clean);
    if (m_promptTail.size() > kPromptTailBytes)
        m_promptTail = m_promptTail.right(kPromptTailBytes);

    switch (m_loginPhase) {
    case LoginPhase::WaitUser:
        if (looksLikeLoginPrompt(m_promptTail)) {
            // 用户名固定用 CR LF 结尾，不跟随 newlineMode——RFC 854 规定
            // Telnet 的行尾就是 CR LF，登录阶段还没协商完，用规范值最稳。
            writeToNetwork(m_loginUser.toUtf8() + "\r\n");
            m_promptTail.clear();
            setLoginPhase(LoginPhase::SentUser);
        }
        break;

    case LoginPhase::SentUser:
        if (looksLikePasswordPrompt(m_promptTail)) {
            // 配置没存密码：把提示符原样留给用户手输，不甩一个空行过去
            //（空行登不上，还白耗一次认证——有些设备几次失败就锁账号）。
            if (m_loginPassword.isEmpty()) {
                setLoginPhase(LoginPhase::Done);
                break;
            }
            writeToNetwork(m_loginPassword.toUtf8() + "\r\n");
            m_promptTail.clear();
            // 密码只发一次：直接进 SentPass，后续再出现 Password: 说明认证
            // 失败了，交给用户手输，不再自动重试（避免把账号锁死）。
            setLoginPhase(LoginPhase::SentPass);
        } else if (looksLikeLoginPrompt(m_promptTail)) {
            // 又回到用户名提示 = 上一次输入没被接受。不重发，收工。
            setLoginPhase(LoginPhase::Done);
        }
        break;

    case LoginPhase::SentPass:
        // 密码已发。再见到任一提示符说明认证失败，停手让用户接管。
        if (looksLikePasswordPrompt(m_promptTail)
            || looksLikeLoginPrompt(m_promptTail)) {
            setLoginPhase(LoginPhase::Done);
        }
        break;

    case LoginPhase::Idle:
    case LoginPhase::Done:
        break;
    }
}

void TcpBridge::setLoginPhase(LoginPhase phase)
{
    if (m_loginPhase == phase)
        return;
    m_loginPhase = phase;
    emit loginPhaseChanged(m_loginPhase);
}

} // namespace cubeshell
