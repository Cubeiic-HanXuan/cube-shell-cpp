#pragma once

// RdpClient.h — RDP 远程桌面客户端（连接管理）。
//
// C++ port of core/rdp/rdp_client.py（RDPInterfaceThread 的连接管理部分）。
// Python 版基于纯 Python 的 aardwolf 库；C++ 侧无对应库，实现为双后端策略：
//   1. 首选 FreeRDP 库集成（CMake find_package(FreeRDP QUIET) 命中时定义
//      CUBESHELL_HAVE_FREERDP）：libfreerdp 建连 + 软件 GDI 渲染，帧经
//      frameUpdated(QImage) 信号回主线程嵌入面板显示——对应 Python 版
//      RDPImage/result 信号链路。
//   2. 命令行后备（FreeRDP 库不可用时）：QProcess 调用外部客户端
//      xfreerdp（Linux/macOS）/ mstsc（Windows），画面在外部窗口显示。
//
// 连接参数：host/port/username/password/domain/resolution（RdpSettings）。
// 整个 rdp/ 目录仅在 CUBESHELL_WITH_RDP=ON 时编译（顶层 CMake 开关）。
//
// Python 版中针对 aardwolf 的模块级补丁（ASN.1 编译缓存、滚轮标志修复、
// read 诊断）均为该库专属的 workaround，不随移植（见 cpp/NOT_PORTED.md）。

#include <QChar>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

namespace cubeshell {

// 连接参数。对应Python: build_rdp_url 的入参 + RDPIOSettings 的分辨率字段
struct RdpSettings {
    QString host;
    int port = 3389;
    QString username;
    QString password;
    QString domain;
    int width = 1280;   // 对应 iosettings.video_width
    int height = 800;   // 对应 iosettings.video_height
    bool fullscreen = false;
};

// 键盘扫描码事件（对应Python: RDP_KEYBOARD_SCANCODE）
struct RdpKeyEvent {
    quint16 scancode = 0;
    bool isExtended = false;
    bool isPressed = true;
};

// Unicode键盘事件（对应Python: RDP_KEYBOARD_UNICODE）
struct RdpUnicodeKeyEvent {
    QChar character;
    bool isPressed = true;
};

// 鼠标事件（坐标+标志位，对应Python: RDP_MOUSE）
struct RdpMouseEvent {
    int x = 0;
    int y = 0;
    quint16 flags = 0;  // PTR_FLAGS_* 组合
};

// 归一化后的连接目标（主机串内联端口优先于 RdpSettings::port）。
struct RdpHostPort {
    QString host;
    int port = 3389;
};

// 主机串归一化：去首尾/内嵌空白与控制字符、剥离 rdp[+xxx]:// 前缀与 userinfo
// 及路径尾巴、脱掉 IPv6 方括号，并拆出 "host:port" 的内联端口。
// FreeRDP 把 ServerHostname 原样交给 getaddrinfo()，主机串里任何多余字符都会
// 让建连失败在 ERRCONNECT_DNS_NAME_NOT_FOUND(0x00020005)——它发生在安全层
// 协商之前，与 NLA/TLS 配置无关，故所有入口统一在此收敛。
RdpHostPort normalizeRdpHost(const QString &rawHost, int defaultPort = 3389);

// （buildRdpUrl 已删除：唯一用途是把明文密码 percent-encode 进 URL，
//  属不安全的死代码，无任何调用点。）

class RdpClient : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected };
    Q_ENUM(State)

    // 实际生效的后端（编译期确定）。
    enum class Backend { FreeRdp, CommandLine };
    Q_ENUM(Backend)

    explicit RdpClient(QObject *parent = nullptr);
    ~RdpClient() override;

    static Backend backend();
    // 命令行后备解析到的外部客户端（xfreerdp/mstsc/open），找不到返回空。
    static QString commandLineProgram();

    State state() const { return m_state; }
    RdpSettings settings() const { return m_settings; }

    // 命令行后端的完整参数（program + args），供测试断言命令行不含明文密码。
    // 仅在 CommandLine 后端有值；FreeRDP 库后端或解析不到客户端时返回空。
    QStringList commandLineArgsForTest() const;

    // 对应Python: RDPInterfaceThread.start（异步建连，结果经信号回报）
    void connectToHost(const RdpSettings &settings);
    // 对应Python: RDPInterfaceThread.stop / RDPWidget.stop
    void disconnectFromHost();

    // 对应Python: in_q.put(RDP_KEYBOARD_SCANCODE)
    void sendKeyEvent(const RdpKeyEvent &event);
    // 对应Python: in_q.put(RDP_KEYBOARD_UNICODE)
    void sendKeyUnicode(const RdpUnicodeKeyEvent &event);
    // 对应Python: in_q.put(RDP_MOUSE)
    void sendMouseEvent(const RdpMouseEvent &event);
    // 对应Python: in_q.put(RDP_CLIPBOARD_DATA_TXT)
    void sendClipboardText(const QString &text);
    // 对应Python: clipboard_send_files
    void clipboardSendFiles(const QStringList &paths);

signals:
    void stateChanged(cubeshell::RdpClient::State state);
    void connected();
    // 对应Python: connection_terminated 信号
    void disconnected();
    // 对应Python: connection_error 信号
    void errorOccurred(const QString &message);
    // 对应Python: result 信号（RDPImage 帧）；仅 FreeRDP 后端发射
    void frameUpdated(const QImage &frame);

private:
    void setState(State state);
    QString resolveCommandLineArgs(QStringList *args) const; // 返回程序名

    State m_state = State::Disconnected;
    RdpSettings m_settings;
    QProcess *m_process = nullptr; // 命令行后备的外部进程
    QString m_lastProcessError;    // 进程错误暂存，由 finished 槽统一发射

#ifdef CUBESHELL_HAVE_FREERDP
    class FreeRdpWorker;
    FreeRdpWorker *m_worker = nullptr;
#endif
};

} // namespace cubeshell
