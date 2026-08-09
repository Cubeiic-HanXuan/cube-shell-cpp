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

    // 换行模式 / 本地回显 / 接收时补 CR 都是随时可改的终端行为，不需要重开串口。
    void setNewlineMode(SerialSettings::NewlineMode mode) { m_newlineMode = mode; }
    SerialSettings::NewlineMode newlineMode() const { return m_newlineMode; }
    void setLocalEcho(bool enabled) { m_localEcho = enabled; }
    bool localEcho() const { return m_localEcho; }
    void setRxImplicitCr(bool enabled) { m_rxImplicitCr = enabled; }
    bool rxImplicitCr() const { return m_rxImplicitCr; }

    // 把一段文本当作用户输入发出去（含换行转换）。供"发送字符串"类功能复用。
    void sendText(const QString &text);

    // 终端产生的 \r 按 mode 转换成实际要写入串口的字节序列。
    // 静态纯函数，便于单测（见 tests/serial_test.cpp）。
    //
    // 实现已抽到 core/net/LineEndings（TcpBridge 共用），这两个静态成员保留为
    // 转发壳子：串口的 NewlineMode 是自己的 enum，映射在此完成，调用点不必改。
    static QByteArray applyNewlineMode(const QByteArray &input,
                                       SerialSettings::NewlineMode mode);

    // 接收方向：给孤立的 LF 补上 CR，已经是 \r\n 的不动。
    //
    // prevWasCr 是**跨调用的状态**，必须由调用方持有并原样传回来：串口的
    // \r 和 \n 完全可能被拆到两次 readyRead 里（尤其低波特率或大块数据），
    // 若每次调用都从零开始判断，边界上的 \n 会被误当成孤立 LF 而多补一个 \r，
    // 屏幕上就多出一个空行。函数返回前会把它更新为「本块最后一个字节是否为 \r」。
    static QByteArray applyRxImplicitCr(const QByteArray &input, bool &prevWasCr);

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
    bool m_rxImplicitCr = true;   // 默认打开，避免每个新用户都先撞一次阶梯
    bool m_prevWasCr = false;     // 跨 readyRead 的边界状态：上一块最后一字节是否为 \r
    bool m_running = false;
};

} // namespace cubeshell
