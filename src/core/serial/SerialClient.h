#pragma once

// SerialClient.h — 串口（Serial Port）会话客户端。
//
// Python 版无对应实现，属 C++ 侧新增的第三种会话类型（与 SSH / RDP 并列）。
// 设计上取 RdpClient 的异步状态机形态：QSerialPort 的 readyRead 天生异步，
// 不需要 SshClient 那样的阻塞读线程，全部在主线程的事件循环里跑。
//
// 职责边界：本类只管"打开端口 / 收发字节 / 端口枚举 / 热插拔 / 落日志"，
// 不碰终端渲染——字节流到 qtermwidget Session 的接线在 SerialBridge 里。
//
// 仅在 CUBESHELL_WITH_SERIAL=ON 时编译（顶层 CMake 开关）。

#include <QList>
#include <QObject>
#include <QSerialPort>
#include <QString>

#include "terminal/session_recorder.h"

class QTimer;

namespace cubeshell {

// 连接参数。串口无网络语义，故没有 host/port/凭据，取而代之是
// 端口名 + 波特率 + 4 个帧格式参数（数据位/校验/停止位/流控）。
struct SerialSettings {
    QString portName;      // "COM3"（Windows）/ "/dev/ttyUSB0"（Unix）
    qint32  baudRate = 115200;

    QSerialPort::DataBits    dataBits    = QSerialPort::Data8;
    QSerialPort::Parity      parity      = QSerialPort::NoParity;
    QSerialPort::StopBits    stopBits    = QSerialPort::OneStop;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;

    // --- 终端行为（不影响物理链路，只影响 SerialBridge 的收发处理） ---

    // 发送时如何转换终端产生的换行。终端按回车键给出的是 \r，但不同设备
    // 期望不同：Linux 控制台要 \n，多数嵌入式/网络设备要 \r\n。
    enum class NewlineMode { Cr, Lf, CrLf };
    NewlineMode newlineMode = NewlineMode::Cr;

    // 接收时给孤立的 LF 补一个 CR（对应 PuTTY 的 "Implicit CR in every LF"、
    // minicom 的 "Add carriage return"、Tera Term 的 Receive: CR+LF）。
    //
    // 为什么需要：VT 规范里 LF 只下移一行、不回行首（回行首是 CR 的职责），
    // 所以对端发裸 \n 时屏幕上会出现阶梯状输出——
    //     line two
    //             line two
    //                     line two
    // SSH 里见不到这个现象，因为 pty 的行规程（termios ONLCR）已经在内核里
    // 把 \n 展开成 \r\n 了；串口对面是裸设备，没人做这个转换。
    //
    // 默认 true：串口的典型对端（单片机、路由器 Console、AT 指令模组）发裸 LF
    // 是常态，默认关掉会让每个新用户都先撞一次阶梯。需要观察原始行为时可关闭。
    //
    // 注意这与 newlineMode 是两个独立方向：newlineMode 管发出去的字节，
    // 本项管收进来的字节，互不影响。
    bool rxImplicitCr = true;

    // 本地回显。串口设备（尤其无 shell 的裸机固件）通常不回显输入，
    // 关掉这个会出现"打字看不见"的假死感。
    bool localEcho = false;

    // 帧格式的紧凑写法，如 "8N1"（数据位/校验/停止位），用于状态栏展示。
    QString frameFormat() const;
};

// 可用串口的描述（QSerialPortInfo 的快照）。
struct SerialPortDesc {
    QString portName;      // 系统端口名，open() 用这个
    QString description;   // 设备描述，如 "USB-SERIAL CH340"
    QString manufacturer;
    quint16 vendorId = 0;
    quint16 productId = 0;
    bool hasVidPid = false;

    // 下拉框展示用："COM3 — USB-SERIAL CH340"（无描述时退化为纯端口名）。
    QString displayName() const;
};

class SerialClient : public QObject {
    Q_OBJECT
public:
    // 仿 RdpClient::State，但串口打开是同步的，没有 Connecting 中间态。
    enum class State { Disconnected, Connected };
    Q_ENUM(State)

    explicit SerialClient(QObject *parent = nullptr);
    ~SerialClient() override;

    State state() const { return m_state; }
    SerialSettings settings() const { return m_settings; }
    bool isOpen() const;

    // 枚举当前可用串口。无串口时返回空表（CI / 无硬件环境不会崩）。
    static QList<SerialPortDesc> availablePorts();

    // 打开串口。成功发 connected()，失败发 errorOccurred() 并返回 false。
    bool open(const SerialSettings &settings);
    // 主动关闭。已关闭时是空操作（不会重复发 disconnected()）。
    void close();

    // 写裸字节到串口。未打开时丢弃并返回 false。
    // 注意：换行转换由 SerialBridge 负责，到这里的字节已是最终形态。
    bool write(const QByteArray &data);

    // --- 会话日志 ---
    // 把收到的原始字节 append 写入文件（发送的字节不记，避免与设备回显重复）。
    // path 为空 = 停止录制并关闭文件。返回是否成功打开。
    // 时间戳/轮转选项取自全局设置（GlobalState session_log_*）。
    bool setLogFile(const QString &path);
    bool isLogging() const { return m_recorder.isActive(); }
    QString logFilePath() const;

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void stateChanged(cubeshell::SerialClient::State state);
    // 收到串口数据（已落日志，未做任何字符转换）。
    void dataReceived(const QByteArray &data);
    // 可用端口集合发生变化（热插拔轮询发现）。UI 据此刷新端口下拉框。
    void portsChanged();

private slots:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);
    // 热插拔轮询：Qt 没有跨平台的串口插拔通知，轮询 QSerialPortInfo 是标准做法。
    void pollPorts();

private:
    void setState(State state);
    void closeLogFile();
    // 当前可用端口名集合，用于与上一次快照比对。
    static QStringList currentPortNames();

    QSerialPort *m_port = nullptr;
    SerialSettings m_settings;
    State m_state = State::Disconnected;

    QTimer *m_pollTimer = nullptr;
    QStringList m_knownPorts;   // 上一次轮询的端口名快照
    // 当前端口在 open() 时是否出现在 QSerialPortInfo 的枚举里。
    // 只有枚举得到的端口才能用"从枚举中消失"判定拔出，详见 pollPorts()。
    bool m_portEnumerated = false;

    // 会话日志录制器（值成员）：原始字节先落盘再发信号。
    SessionRecorder m_recorder;
};

} // namespace cubeshell
