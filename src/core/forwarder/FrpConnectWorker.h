#pragma once

// FrpConnectWorker.h — 内网穿透（FRP）连接/停止的后台流程线程。
//
// 对应Python: cube-shell.py::FRPConnectThread（L328-502）
//
// 流程与 Python 版逐步对齐（顺序、文案、sleep 时长均一致）：
//   run()            → 解析 host、可达性检查、SSH 连接、分发 start/stop
//   startServices()  → 低端口 root 检查 → frpc/frps 按需安装 → 远端 frps
//                      重启 → 本地 frpc.toml 落盘
//   stopServices()   → 远端 pkill -9 frps
//
// 与 Python 的唯一职责差异：本地 frpc 进程的启停不在本线程做。
// Python 用 os.system/subprocess 起裸进程，与线程无关；C++ 侧本地 frpc 由
// FrpManager 的 QProcess 托管，而 QProcess 必须与其宿主对象同线程，因此
// 「启动本地 frpc」交给主线程在 finishedSignal 回调里执行（见 NatDialog）。
// 本线程只负责把 frpc.toml 写到与 Python 版完全相同的位置（两版可互读）。
//
// 中断协议（C++ 特有）：run() 是纯阻塞式实现，没有事件循环，因此 quit() /
// exit() 对它无效。要提前结束必须调用 requestInterruption()，run() 会在各
// 阶段之间检查 isInterruptionRequested() 并静默返回（不再发 finishedSignal，
// 因为此时接收方通常正在关闭）。单次阻塞调用（SSH 握手、远端 exec、下载）
// 无法被打断，故调用方 wait() 应带超时，见 NatDialog::shutdownWorker。

#include <QString>
#include <QThread>

#include "net/ProxyConfig.h"

namespace cubeshell {

class SshClient;
class CommandExecutor;
class SftpClient;

// 对应Python: FRPConnectThread 的 params 字典
// 端口保持字符串（与 Python 的 params['server_prot'] / ['local_port'] 一致）。
struct FrpConnectParams {
    QString host;      // 可带端口，如 "1.2.3.4:2222"
    QString username;
    QString password;
    QString keyType;   // 空 = 密码认证
    QString keyFile;
    QString token;
    QString antType;   // "TCP"/"UDP"/"HTTP"/...
    QString localPort;
    QString serverPort; // 对应Python: params['server_prot']

    // 代理（C++ 特有；Python 版没有代理功能）。与 password 一样由调用方从
    // resolved() 出来的设备条目里带进来，见 NatDialog::onConnectClicked。
    ProxyConfig proxy;
};

class FrpConnectWorker : public QThread {
    Q_OBJECT
public:
    explicit FrpConnectWorker(const FrpConnectParams &params, bool isStop,
                              QObject *parent = nullptr);
    ~FrpConnectWorker() override;

    // --- 纯工具（无 SSH，可单测） ---

    // TCP 可达性探测（1 秒超时）。
    // 对应Python: function/util.py::check_server_accessibility
    static bool checkServerAccessibility(const QString &host, quint16 port,
                                         int timeoutMs = 1000);

    // 本地 frpc.toml 的落盘路径 —— 必须与 Python 的 abspath('frpc.toml')
    // （工程根 conf/frpc.toml）一致，否则两版无法互读配置。
    // 对应Python: cube-shell.py::abspath('frpc.toml')
    static QString frpcConfigPath();

    // 清理游离的本地 frpc 进程（Python 版留下的裸进程也在此列）。
    // 对应Python: os.system("pkill -9 frpc") / taskkill /f /im frpc.exe
    static void killLocalFrpc();

    // 端口文本校验：要求是纯数字且落在 1-65535 内。
    // C++ 特有安全修复：Python 的 int('abc') 会抛异常并被 run() 的 except 兜
    // 成错误提示；而 QString::toInt() 失败只静默返回 0，会误触发低端口 root
    // 检查、并写出 localPort=0 的非法 frpc.toml，故必须显式校验。
    static bool isValidPort(const QString &text, int *portOut = nullptr);

signals:
    // 对应Python: status_updated = Signal(str)
    void statusUpdated(const QString &message);
    // 对应Python: progress_updated = Signal(int)
    void progressUpdated(int percent);
    // 对应Python: finished_signal = Signal(bool, str, bool)
    //             (成功与否, 错误消息, is_start_action)
    void finishedSignal(bool success, const QString &errorMessage, bool isStartAction);

protected:
    void run() override;

private:
    // 对应Python: FRPConnectThread._start_services
    void startServices(CommandExecutor *exec, SftpClient *sftp);
    // 对应Python: FRPConnectThread._stop_services
    void stopServices(CommandExecutor *exec);
    // 分片睡眠，中断请求到达时立即返回 false（C++ 特有：让 wait() 能及时收口）。
    // 未被中断时的总时长与 Python 的 time.sleep 一致。
    bool interruptibleSleep(int ms);

    FrpConnectParams m_params;
    bool m_isStop;
};

} // namespace cubeshell
