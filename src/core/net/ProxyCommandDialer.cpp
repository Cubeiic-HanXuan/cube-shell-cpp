// ProxyCommandDialer.cpp — 见 ProxyCommandDialer.h。
//
// 结构：
//   ProxyCommandTransport   句柄，交给 SshClient 持有；析构 = 拆掉整套载体
//   PumpState               句柄与泵线程共享的那一小块状态（shared_ptr 持有）
//   runPump()               泵线程主体：拥有 QProcess，跑一个事件循环对搬字节
//
// 为什么泵线程里跑事件循环，而不是一个 select 轮询循环：
//   子进程的 stdin/stdout 在 Qt 里只有 QProcess 这一个门面，而 QProcess 不导出
//   底层管道 fd，没法和 socket 一起塞进同一个 select。轮询两边（waitForReadyRead
//   20ms + select 20ms 交替）能跑，但那 20ms 会被用户直接感知成打字延迟。
//   事件循环 + QSocketNotifier 把两个方向都变成回调，零轮询、零附加延迟，
//   而且 QProcess 的读写始终在它自己的线程里发生（跨线程碰 QIODevice 是 UB）。

#include "ProxyCommandDialer.h"

#include <QDeadlineTimer>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>
#include <memory>

// 复用 ProxyConnector.cpp 注册的 "cubeshell.proxy" 分类：代理相关的日志开一个
// 开关就该全部出来，分两个分类反而要记两个名字。
Q_DECLARE_LOGGING_CATEGORY(proxyLog)

namespace cubeshell {

using namespace socket_util;

namespace {

// 中继缓冲。32KB 与 SSH 报文上限同量级；再小会让 SFTP 批量传输在回调上打转。
constexpr int kRelayChunkBytes = 32 * 1024;

// 往载体进程 stdin 攒的字节上限。QProcess::write 的缓冲是无上限的，载体消化不过来
// （慢链路上的 nc）时会一路吃内存。到线就关掉读通知器，等 bytesWritten 落回来再开
// ——这就是背压。
constexpr qint64 kStdinHighWaterBytes = 1024 * 1024;

// 单次「stdout → socket」的写超时。libssh2 那一端一直在读，正常永远用不到；
// 设上限是为了不被一个已经不读了的对端把泵线程钉死。
constexpr int kRelayWriteTimeoutMs = 30 * 1000;

// 载体进程收尾的宽限：先关 stdin 让它自己退（nc 见到 EOF 就退），到点 terminate，
// 再到点 kill。整条路径有界，这是泵线程能被按时 join 的前提。
constexpr int kProcessGraceMs     = 1000;
constexpr int kProcessKillGraceMs = 500;

// 等泵线程退出的上限。它是事件循环，shutdown() 会立刻唤醒它，上面那两段宽限也都
// 有界，正常路径远用不到这个数。
constexpr int kPumpJoinMs = 5000;

// 心跳：每片回头查一次 stop 标志。
//
// 为什么不能只靠「shutdown(serverEnd) → 读通知器醒」这一条唤醒路径：背压生效时
// 读通知器正被关着，那一下就唤不醒。心跳把「醒不过来」这件事整类兜掉，代价是每个
// 活跃的代理命令连接每秒多 4 次空回调。
constexpr int kHeartbeatMs = 250;

// 诊断信息容量上限。载体进程可能在 stderr 上无限刷（把 -v 开满的 nc），
// 攒的目的只是给用户一句"真正的原因"，几 KB 足够。
constexpr int kMaxDiagnosticBytes = 4 * 1024;

void setErr(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

// 句柄与泵线程共享的状态。
//
// 单独提成一个 shared_ptr 持有的结构体、而不是直接放进 ProxyCommandTransport，
// 是为了让「等不到泵线程退出」这种极端情况能**安全地泄漏**：线程体自己捏着一份
// shared_ptr，句柄对象析构掉也不会把它脚下的内存抽走，没有 UAF 窗口。
struct PumpState {
    QString command;
    int startTimeoutMs = kDefaultConnectTimeoutMs;

    // 泵线程这一端。**只由本结构体的析构负责回收**——别处一律只 shutdown 不 close，
    // 否则 fd 号会被系统重新分配给别人，而 shutdown() 还在往那个号上打。
    qintptr serverEnd = kInvalidSocket;

    std::atomic<bool> stop{false};

    // 下面这些字段两条线程都碰，统一由 mutex 保护。
    // mutable：diagnostics() 是 const（ProxyTransport 的接口）。
    mutable QMutex mutex;
    QWaitCondition cond;
    bool startSettled = false;   // 泵线程已给出「起来了 / 起不来」的结论
    bool startOk = false;
    QString startError;
    QString diagnostics;

    ~PumpState() { closeSocket(serverEnd); }

    void settleStart(bool ok, const QString &error)
    {
        QMutexLocker lock(&mutex);
        if (startSettled)
            return;          // 只认第一个结论
        startSettled = true;
        startOk = ok;
        startError = error;
        cond.wakeAll();
    }

    void appendDiagnostic(const QString &text)
    {
        QMutexLocker lock(&mutex);
        if (diagnostics.size() >= kMaxDiagnosticBytes)
            return;
        diagnostics += text;
    }
};

// 起载体进程。
void startProcess(QProcess &proc, const QString &command)
{
#ifdef Q_OS_WIN
    // Windows 上不套 shell：cmd.exe /c 的引号规则与用户从 ~/.ssh/config 抄来的
    // 命令行常常不兼容，套了反而更容易出人意料。按 Qt 的规则拆词直接起进程，
    // 代价是管道/重定向这类 shell 操作符不支持（ProxyCommand 里极少用到）。
    proc.startCommand(command);
#else
    // 与 OpenSSH 一致：ProxyCommand 交给 shell 执行。用户抄过来的命令行里那些
    // $VAR、引号、偶尔的管道因此能原样工作。
    proc.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
#endif
}

// 泵线程主体。st 按值捕获（shared_ptr）——见 PumpState 的注释。
void runPump(std::shared_ptr<PumpState> st)
{
    // **必须最先构造**，在 QProcess 之前。QThread::create 起的线程不跑 exec()，
    // 因此本线程一开始**没有事件分发器**；而 QProcess 只在分发器已存在时才给
    // stdout/stderr/进程状态装上通知器，装不上就再也不会补装——表现是进程明明
    // 在跑、readyRead 一次都不来，中继静默地什么都不搬。QEventLoop 的构造函数
    // 会 ensureEventDispatcher()，所以顺序反了就是个死局。
    QEventLoop loop;

    // QProcess 必须在**用它的那个线程**里创建：它的读写靠本线程的事件分发器，
    // 跨线程碰它是 UB。
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    // stderr 单独收着当诊断。载体进程的 stderr 往往是唯一说清病因的地方
    //（"nc: connect to 10.0.0.1 port 22 failed: Connection refused"），而那一刻
    // 拨号早已成功返回，用户能看到的只有 libssh2 一句 "Failed getting banner"。
    QObject::connect(&proc, &QProcess::readyReadStandardError, &proc, [st, &proc]() {
        const QByteArray raw = proc.readAllStandardError();
        if (raw.isEmpty())
            return;
        const QString text = QString::fromLocal8Bit(raw);
        st->appendDiagnostic(text);
        qCWarning(proxyLog).noquote() << "代理命令 stderr:" << text.trimmed();
    });

    startProcess(proc, st->command);
    if (!proc.waitForStarted(st->startTimeoutMs)) {
        // 绝大多数情况是载体二进制不存在（用户填了 nc 但机器上没装）。
        // 把命令原文带上，否则用户无从判断是哪一段写错了。
        st->settleStart(false, QStringLiteral("启动代理命令失败：%1（命令：%2）")
                                   .arg(proc.errorString(), st->command));
        return;
    }
    // 起来了。往下一切取消都走 st->stop —— 拨号线程那个 cancelled 标志指向
    // SshClient::m_dialCancelled，只在拨号期间有效，泵线程活得比它长。
    st->settleStart(true, QString());

    QSocketNotifier reader(st->serverEnd, QSocketNotifier::Read);
    bool finishing = false;   // 已进入收尾，别再打开读通知器

    // 统一的收工入口：幂等，可从任何一个回调里调。
    auto finish = [&](const QString &why) {
        if (finishing)
            return;
        finishing = true;
        reader.setEnabled(false);
        qCDebug(proxyLog).noquote() << "代理命令中继结束：" << why;
        loop.quit();
    };

    // 载体进程 stdout → socket（给 libssh2 读）。
    auto flushStdout = [&]() {
        const QByteArray out = proc.readAllStandardOutput();
        if (out.isEmpty())
            return;
        QString err;
        // sendAll 会在 socket 写满时阻塞住事件循环。可以接受：对端是 libssh2，
        // 它一直在读，而 socket pair 两端都有 256KB 缓冲（kPairSocketBufferBytes）。
        // 传 &st->stop 是为了万一真堵上了，shutdown() 还能把它捞出来。
        if (!sendAll(st->serverEnd, out.constData(), out.size(), kRelayWriteTimeoutMs,
                     &err, &st->stop)) {
            finish(QStringLiteral("写回 libssh2 失败：%1").arg(err));
        }
    };
    QObject::connect(&proc, &QProcess::readyReadStandardOutput, &proc, flushStdout);

    // socket（libssh2 写出的）→ 载体进程 stdin。
    QObject::connect(&reader, &QSocketNotifier::activated, &reader, [&]() {
        // 载体已经没了就别再往 stdin 灌：对一个已关闭的 QProcess 调 write 会打出
        // "QIODevice::write (QProcess): device not open"，把真正的病因刷没了。
        // finished 信号和本回调的到达顺序不保证，所以这里要自己挡一道。
        if (proc.state() == QProcess::NotRunning) {
            finish(QStringLiteral("载体进程已不在运行"));
            return;
        }
        char buf[kRelayChunkBytes];
        for (;;) {
            QString err;
            const qint64 n = recvNonBlocking(st->serverEnd, buf, sizeof(buf), &err);
            if (n > 0) {
                proc.write(buf, int(n));
                if (proc.bytesToWrite() >= kStdinHighWaterBytes) {
                    reader.setEnabled(false);   // 背压：等 bytesWritten 再开
                    return;
                }
                // Windows 的通知器是边沿触发的：不读干净就不会再响。所以这里必须
                // 一直读到 -1（EAGAIN）为止，不能读一块就返回。
                continue;
            }
            if (n == -1)
                return;                          // 这一刻没数据了，回事件循环
            finish(n == 0 ? QStringLiteral("libssh2 侧已关闭连接")
                          : QStringLiteral("读取 libssh2 侧失败：%1").arg(err));
            // stdin 给个 EOF，让载体进程自己正常退出（比 terminate 干净）。
            if (proc.state() != QProcess::NotRunning)
                proc.closeWriteChannel();
            return;
        }
    });

    // 背压解除。
    QObject::connect(&proc, &QProcess::bytesWritten, &proc, [&](qint64) {
        if (!finishing && !reader.isEnabled() && proc.bytesToWrite() < kStdinHighWaterBytes)
            reader.setEnabled(true);
    });

    // 载体进程退出：把它最后吐出来的字节送完，然后让 libssh2 那一端看到 EOF。
    QObject::connect(&proc, &QProcess::finished, &proc,
                     [&](int exitCode, QProcess::ExitStatus status) {
        flushStdout();
        // 非零退出码要留痕：`nc` 起得来、然后立刻因为 connection refused 退出
        // 是最常见的失败形态，而那时拨号已经返回成功了。
        if (status != QProcess::NormalExit || exitCode != 0) {
            st->appendDiagnostic(QStringLiteral("代理命令异常退出（退出码 %1）\n")
                                     .arg(exitCode));
        }
        shutdownFd(st->serverEnd);   // libssh2 的读立刻拿到 EOF，不必等超时
        finish(QStringLiteral("载体进程已退出（退出码 %1）").arg(exitCode));
    });

    // 心跳：兜住所有「醒不过来」的情形（理由见 kHeartbeatMs）。
    QTimer heartbeat;
    heartbeat.setInterval(kHeartbeatMs);
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&]() {
        if (st->stop.load())
            finish(QStringLiteral("收到停止请求"));
    });
    heartbeat.start();

    // 进循环之前先抽一次：waitForStarted 期间可能已经有 stdout 落进缓冲，
    // 那一份的 readyRead 信号我们还没接上，不抽就一直压在里面。
    flushStdout();
    if (!st->stop.load())
        loop.exec();

    // --- 收尾。顺序要紧 ---
    reader.setEnabled(false);                     // 不再往 stdin 灌
    if (proc.state() != QProcess::NotRunning) {
        proc.closeWriteChannel();                 // stdin EOF：载体自己退
        if (!proc.waitForFinished(kProcessGraceMs)) {
            proc.terminate();
            if (!proc.waitForFinished(kProcessKillGraceMs)) {
                proc.kill();
                proc.waitForFinished(kProcessKillGraceMs);
            }
        }
    }
    // 载体先挂掉时 libssh2 那一端还开着：给它一个 EOF，否则读循环会一直等一个
    // 永不到来的字节。shutdown 而不是 close——fd 的回收归 ~PumpState。
    shutdownFd(st->serverEnd);
}

// 「代理命令」的载体句柄。生命周期由 SshClient 持有（见 ProxyTransport）。
class ProxyCommandTransport : public ProxyTransport {
public:
    ~ProxyCommandTransport() override
    {
        shutdown();
        if (m_thread) {
            if (m_thread->wait(kPumpJoinMs)) {
                delete m_thread;
            } else {
                // 泵线程没在预算内退出。**宁可泄漏也不能删**：删一个还在跑的
                // QThread 是 UB，而 PumpState 由线程体自己捏着一份 shared_ptr，
                // 泄漏这个 QThread 对象不会造成 UAF，只是一个线程 + 一对 fd 留到
                // 进程结束。手法同 SshBridge::stop 里那段 detach。
                qCWarning(proxyLog) << "代理命令泵线程未在" << kPumpJoinMs
                                    << "ms 内退出，放弃回收（泄漏一个线程）";
            }
            m_thread = nullptr;
        }
        // 没被 takeClientEnd() 取走时才需要关（启动失败路径）。
        closeSocket(m_clientEnd);
    }

    // 起进程 + 泵线程，并等到「进程确实起来了」才返回。
    bool start(const QString &command, int timeoutMs,
               const std::atomic<bool> *cancelled, QString *errorOut)
    {
        const int budgetMs = timeoutMs > 0 ? timeoutMs : kDefaultConnectTimeoutMs;

        auto st = std::make_shared<PumpState>();
        st->command = command;
        st->startTimeoutMs = budgetMs;

        QString pairErr;
        if (!makeSocketPair(m_clientEnd, st->serverEnd, &pairErr)) {
            setErr(errorOut, QStringLiteral("为代理命令建立本地 socket 失败：%1").arg(pairErr));
            return false;
        }
        // 泵线程这一端走通知器驱动，必须非阻塞：通知器有伪唤醒，一次阻塞 recv
        // 就能把整个事件循环钉死。交给 libssh2 的那一端保持阻塞——与直连产物的
        // 状态一致，后面的 handshake/openShell 才不用改。
        setNonBlocking(st->serverEnd, true);

        m_state = st;
        m_thread = QThread::create([st]() { runPump(st); });
        m_thread->setObjectName(QStringLiteral("proxy-cmd-pump"));
        m_thread->start();

        // 等结论。按切片查取消：cancelled 指向 SshClient::m_dialCancelled，
        // 用户这一刻关掉标签页要能立刻放弃，而不是等满整条预算。
        QDeadlineTimer deadline(budgetMs);
        QMutexLocker lock(&st->mutex);
        while (!st->startSettled) {
            if (cancelled && cancelled->load()) {
                setErr(errorOut, QStringLiteral("连接已取消"));
                return false;
            }
            const qint64 left = deadline.remainingTime();
            if (left <= 0) {
                setErr(errorOut, QStringLiteral("代理命令启动超时（%1 ms）：%2")
                                     .arg(budgetMs).arg(command));
                return false;
            }
            st->cond.wait(&st->mutex,
                          QDeadlineTimer(qMin<qint64>(kSelectSliceMs, left)));
        }
        if (!st->startOk) {
            setErr(errorOut, st->startError);
            return false;
        }
        return true;
    }

    // 交给 libssh2 的那一端。取走之后本对象不再负责它的回收。
    qintptr takeClientEnd()
    {
        const qintptr fd = m_clientEnd;
        m_clientEnd = kInvalidSocket;
        return fd;
    }

    void shutdown() override
    {
        if (!m_state)
            return;
        m_state->stop.store(true);
        // 唤醒读通知器。只 shutdown 不 close：fd 的回收归 ~PumpState，提前 close
        // 会让 fd 号被系统分配给别人，而这里还在往那个号上打。
        shutdownFd(m_state->serverEnd);
    }

    QString diagnostics() const override
    {
        if (!m_state)
            return QString();
        QMutexLocker lock(&m_state->mutex);
        return m_state->diagnostics;
    }

private:
    std::shared_ptr<PumpState> m_state;
    QThread *m_thread = nullptr;
    qintptr m_clientEnd = kInvalidSocket;
};

} // namespace

CommandDialer makeProxyCommandDialer()
{
    return [](const QString &command, int timeoutMs, const std::atomic<bool> *cancelled,
              ProxyTransportPtr *transportOut, QString *errorOut) -> qintptr {
        // 约定：0 = 预算已耗尽，不是"用默认值"（见 SocketUtil.h 那段）。
        if (timeoutMs == 0) {
            setErr(errorOut, QStringLiteral("代理命令没有可用的建连预算（超时已耗尽）"));
            return kInvalidSocket;
        }
        // 已经取消了就别起进程。start() 里的等待循环也查这个标志，但那是在
        // 「进程已经 fork 出去」之后——多跳链里前几跳失败/用户已关标签页时，
        // 白起一个子进程再杀掉是能避免的。
        if (cancelled && cancelled->load()) {
            setErr(errorOut, QStringLiteral("连接已取消"));
            return kInvalidSocket;
        }
        if (!transportOut) {
            // 没人接手句柄，就意味着子进程和泵线程会在本函数返回时立刻被拆掉，
            // 交出去的 fd 随即变成一根断管。这是调用方的编程错误——宁可在这里
            // 明确失败，也不要交出一个几毫秒后开始随机掉线的连接。
            setErr(errorOut, QStringLiteral("内部错误：代理命令的载体句柄无人接管"));
            return kInvalidSocket;
        }

        auto transport = std::make_shared<ProxyCommandTransport>();
        if (!transport->start(command, timeoutMs, cancelled, errorOut))
            return kInvalidSocket;   // 析构里拆掉进程/线程/socket pair

        const qintptr fd = transport->takeClientEnd();
        *transportOut = std::move(transport);
        qCDebug(proxyLog).noquote() << "代理命令已启动：" << command;
        return fd;
    };
}

} // namespace cubeshell
