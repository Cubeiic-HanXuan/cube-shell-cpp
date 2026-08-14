#pragma once

// ConnectionTester.h — 「测试连接」后台探测。
//
// 为「添加/编辑设备」对话框提供一种轻量的连通性校验：不发起完整会话、不开
// pty / 不登录串口终端，只验证「这套配置能不能连上」。判定标准按协议分层：
//
//   SSH     完成 TCP + SSH 握手 + 认证（connectToHost，不开 shell）。
//   Telnet  完成 TCP 连接即视为可达（登录提示词因设备而异，不在此判定）。
//   TCP     同上，裸 TCP 没有登录概念。
//   RDP     只做到 host:port 的 TCP 可达（完整 RDP/NLA 握手无法无头完成）。
//   Serial  能按给定帧参数打开端口即视为可用（无网络对端可探测）。
//
// 三种协议的异步模型不同（SSH 阻塞、TCP 事件驱动、Serial 同步），本类把它们
// 收敛成同一个信号 finished(ok, message)，对话框只接这一个信号。
//
// 线程模型：SSH 在 worker QThread 上跑（connectToHost 是阻塞调用），
// cancel() 借 SshClient::shutdownSocket() 把它从阻塞的 connect() 里打醒——
// 这正是标签页关闭路径使用的机制。TCP/Serial 都在创建本对象的线程（UI 线程）
// 上完成，不另起线程。整体有一个兜底定时器：SSH 的阻塞 connect() 自身没有
// 超时（内核 SYN 重传可能拖到一两分钟），超时即 cancel 并报失败。

#include <memory>

#include <QObject>
#include <QString>

#include "config/DeviceConfigStore.h"   // DeviceEntry
#include "net/TcpClient.h"              // TcpSettings / TcpClient
#ifdef CUBESHELL_WITH_SERIAL
#include "serial/SerialClient.h"        // SerialSettings
#endif

class QTimer;
class QThread;

namespace cubeshell {

class SshClient;

class ConnectionTester : public QObject {
    Q_OBJECT
public:
    explicit ConnectionTester(QObject *parent = nullptr);
    ~ConnectionTester() override;

    bool isRunning() const { return m_running; }

    // 各 start 入口：返回 false 表示已有测试在进行（本次请求被忽略）。
    // 结果经 finished() 恰好回报一次。
    bool testSsh(const DeviceEntry &entry);
    // telnet / tcp / rdp 的可达性测试：只看 TCP 能不能连上 host:port。
    bool testTcp(const DeviceEntry &entry);
#ifdef CUBESHELL_WITH_SERIAL
    bool testSerial(const SerialSettings &settings);
#endif

    // 打断进行中的测试（若有）。SSH 路径借此把阻塞的 connect() 唤醒。
    void cancel();

signals:
    void finished(bool ok, const QString &message);

private:
    void finish(bool ok, const QString &message);
    void startGuardTimer(int ms);
    void cleanupTcp();

    struct SshResult;   // 定义在 .cpp（含结果 + 代数令牌）
    void onSshDone(const std::shared_ptr<SshResult> &result);

    bool m_running = false;
    // 代数令牌：每次 start/cancel 递增。迟到的 SSH worker 结果凭它识别并丢弃，
    // 避免「上一个已超时的测试」误收新一次测试的果子。
    quint64 m_generation = 0;
    // 兜底超时。一旦触发就 cancel 并按失败回报；之后迟到的结果由 finish()
    // 里的 m_running 闸门挡掉，保证 finished() 只发一次。
    QTimer *m_guard = nullptr;

    // --- TCP/Telnet/RDP 路径（UI 线程，事件驱动） ---
    TcpClient *m_tcp = nullptr;

    // --- SSH 路径（worker 线程，detached） ---
    // 与 worker 共享：worker 持有自己的一份 shared_ptr 副本，本成员仅供
    // cancel()/shutdownSocket() 跨线程打断。不 join worker（见 .cpp 析构注释），
    // 故无需保存 QThread 指针——它 finished 后 deleteLater 自我回收。
    std::shared_ptr<SshClient> m_sshClient;
};

} // namespace cubeshell
