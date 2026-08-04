// SerialBridge.cpp — 串口 ↔ 终端桥接。见 SerialBridge.h。

#include "SerialBridge.h"

#include <QDebug>

#include "Emulation.h"
#include "Session.h"

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
    writeToTerminal(data);
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
    // 终端的回车键产生的是 \r（CR）。Cr 模式即原样透传，是最常见的默认。
    if (mode == SerialSettings::NewlineMode::Cr)
        return input;

    QByteArray out;
    out.reserve(input.size() + 8);   // CrLf 最坏情况每个 \r 多一字节

    for (int i = 0; i < input.size(); ++i) {
        const char c = input.at(i);
        if (c != '\r') {
            out.append(c);
            continue;
        }
        // 已经是 CRLF 的（终端一般不会产生，但粘贴的文本里会有）不再重复加 LF。
        const bool nextIsLf = (i + 1 < input.size() && input.at(i + 1) == '\n');
        switch (mode) {
        case SerialSettings::NewlineMode::Lf:
            out.append('\n');
            if (nextIsLf)
                ++i;          // 吃掉原有的 \n，避免变成 \n\n
            break;
        case SerialSettings::NewlineMode::CrLf:
            out.append('\r');
            out.append('\n');
            if (nextIsLf)
                ++i;          // 原本就是 \r\n，别写成 \r\n\n
            break;
        case SerialSettings::NewlineMode::Cr:
            out.append('\r'); // 上面已提前返回，这里只为编译器穷尽 switch
            break;
        }
    }
    return out;
}

} // namespace cubeshell
