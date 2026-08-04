#pragma once

// SerialBridge.h — 串口字节流 ↔ qtermwidget Session 的桥接。
//
// 结构对照 SshBridge（core/ssh/SshBridge.h），但去掉了两样东西：
//   1. 读线程：QSerialPort::readyRead 本身就是主线程事件循环里的异步回调，
//      不像 libssh2 那样必须用阻塞读，故没有 worker thread。
//   2. resize：串口是裸字节链路，没有 pty 窗口尺寸的概念，不接
//      Session::terminalSizeApplied。
//
// 同样不做 OSC7 / MFA / AI 哨兵过滤——那些都是 shell 语义，串口对面可能
// 是个没有 shell 的单片机，按字节原样透传才是正确行为。
//
// 仅在 CUBESHELL_WITH_SERIAL=ON 时编译。

#include <QByteArray>
#include <QObject>

#include "SerialClient.h"

namespace Konsole { class Session; }

namespace cubeshell {

class SerialBridge : public QObject {
    Q_OBJECT
public:
    // session: 终端所附着的 qtermwidget Session。
    // client:  串口客户端（可以还没 open，接线与连接状态无关）。
    SerialBridge(Konsole::Session *session, SerialClient *client,
                 QObject *parent = nullptr);
    ~SerialBridge() override;

    void start();
    void stop();

    // 换行模式 / 本地回显是随时可改的终端行为，不需要重开串口。
    void setNewlineMode(SerialSettings::NewlineMode mode) { m_newlineMode = mode; }
    SerialSettings::NewlineMode newlineMode() const { return m_newlineMode; }
    void setLocalEcho(bool enabled) { m_localEcho = enabled; }
    bool localEcho() const { return m_localEcho; }

    // 把一段文本当作用户输入发出去（含换行转换）。供"发送字符串"类功能复用。
    void sendText(const QString &text);

    // 终端产生的 \r 按 mode 转换成实际要写入串口的字节序列。
    // 静态纯函数，便于单测（见 tests/serial_test.cpp）。
    static QByteArray applyNewlineMode(const QByteArray &input,
                                       SerialSettings::NewlineMode mode);

signals:
    // 供面板把字节回灌到终端（本地回显走这条路，与串口来的数据同一入口）。
    void echoData(const QByteArray &data);

private slots:
    void onSerialData(const QByteArray &data);

private:
    void onEmulationSendData(const char *data, int length);
    void writeToTerminal(const QByteArray &data);

    Konsole::Session *m_session = nullptr;
    SerialClient *m_client = nullptr;

    SerialSettings::NewlineMode m_newlineMode = SerialSettings::NewlineMode::Cr;
    bool m_localEcho = false;
    bool m_running = false;
};

} // namespace cubeshell
