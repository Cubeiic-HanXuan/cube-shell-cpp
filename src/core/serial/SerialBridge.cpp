// SerialBridge.cpp — 串口 ↔ 终端桥接。见 SerialBridge.h。

#include "SerialBridge.h"

#include <QDebug>

#include "Emulation.h"
#include "Session.h"
#include "net/LineEndings.h"

namespace cubeshell {

SerialBridge::SerialBridge(Konsole::Session *session, SerialClient *client,
                           QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_client(client)
{
}

SerialBridge::~SerialBridge()
{
    stop();
}

void SerialBridge::start()
{
    using Konsole::Emulation;

    if (m_running || !m_session || !m_client)
        return;

    // 0. 断开 Session 构造时自建的 emulation→Pty 连接。
    //    不做这步的话按键会同时送到本地空闲 PTY 和串口，PTY 的行规程会把
    //    字符回显回来（receivedData → onReceiveBlock），屏幕上每个字符出现
    //    两次。与 SshBridge::start() 的第 0 步同因同解。
    m_session->runEmptyPTY();

    // 1. 用户按键 → 串口（含换行转换 + 可选本地回显）。
    if (Emulation *emulation = m_session->emulation()) {
        connect(emulation, &Emulation::sendData,
                this, &SerialBridge::onEmulationSendData);
    }

    // 2. 串口数据 → 终端。
    connect(m_client, &SerialClient::dataReceived,
            this, &SerialBridge::onSerialData);

    // 3. 回显/注入数据 → 终端。与串口来的数据共用同一个写入口。
    //    QueuedConnection：回显是在 sendData 回调里发出的，排队一拍避免
    //    在 Emulation 的信号栈里重入 onReceiveBlock。
    connect(this, &SerialBridge::echoData,
            m_session,
            [session = m_session](const QByteArray &data) {
                session->onReceiveBlock(data.constData(), int(data.size()));
            },
            Qt::QueuedConnection);

    // 4. 不接 terminalSizeApplied —— 串口没有 pty 尺寸。

    m_running = true;
}

void SerialBridge::stop()
{
    if (!m_running)
        return;
    m_running = false;

    // 断开本对象参与的所有连接。析构顺序上 Session 可能先走，留着连接
    // 会在下一次 readyRead 时指向已销毁的 Session。
    if (m_session) {
        if (Konsole::Emulation *emulation = m_session->emulation())
            disconnect(emulation, nullptr, this, nullptr);
        disconnect(this, &SerialBridge::echoData, m_session, nullptr);
    }
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
}

void SerialBridge::onEmulationSendData(const char *data, int length)
{
    if (!m_running || !m_client || length <= 0)
        return;

    const QByteArray raw(data, length);
    const QByteArray converted = applyNewlineMode(raw, m_newlineMode);

    m_client->write(converted);

    // 本地回显：把转换后的字节回灌终端。用转换后的结果而不是原始 \r，
    // 是为了让屏幕上的换行表现与实际发出去的一致。
    if (m_localEcho)
        emit echoData(converted);
}

void SerialBridge::onSerialData(const QByteArray &data)
{
    if (!m_rxImplicitCr) {
        // 关闭时连边界状态一起清掉，免得中途开启时带着上一次的残留判断。
        m_prevWasCr = false;
        writeToTerminal(data);
        return;
    }
    writeToTerminal(applyRxImplicitCr(data, m_prevWasCr));
}

void SerialBridge::writeToTerminal(const QByteArray &data)
{
    if (!m_session || data.isEmpty())
        return;
    m_session->onReceiveBlock(data.constData(), int(data.size()));
}

void SerialBridge::sendText(const QString &text)
{
    if (!m_running || !m_client)
        return;
    const QByteArray converted = applyNewlineMode(text.toUtf8(), m_newlineMode);
    m_client->write(converted);
    if (m_localEcho)
        emit echoData(converted);
}

QByteArray SerialBridge::applyNewlineMode(const QByteArray &input,
                                          SerialSettings::NewlineMode mode)
{
    // 串口的 NewlineMode → 协议中立的 cubeshell::NewlineMode，实现在
    // core/net/LineEndings.cpp（与 TcpBridge 共用同一份）。
    switch (mode) {
    case SerialSettings::NewlineMode::Lf:
        return cubeshell::applyNewlineMode(input, NewlineMode::Lf);
    case SerialSettings::NewlineMode::CrLf:
        return cubeshell::applyNewlineMode(input, NewlineMode::CrLf);
    case SerialSettings::NewlineMode::Cr:
        break;
    }
    return cubeshell::applyNewlineMode(input, NewlineMode::Cr);
}

QByteArray SerialBridge::applyRxImplicitCr(const QByteArray &input, bool &prevWasCr)
{
    return cubeshell::applyRxImplicitCr(input, prevWasCr);
}

} // namespace cubeshell
