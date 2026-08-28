// 「代理命令」拨号器的单元测试：ProxyCommandDialer.cpp（子进程 stdin/stdout ↔
// socket pair 的双向中继）。
//
// 为什么必须有这个测试：那份实现是本次代理功能里唯一「跨线程 + 子进程 + 事件
// 循环」三样凑一块的地方，出错的表现全是难查的：装不上通知器 → 一个字节都不搬；
// 通知器不读干净 → 大文件传输随机卡死；载体拆早了 → 连上几秒后随机掉线。这些
// 都无法靠读代码确认，只能真起进程搬字节。
//
// 不需要任何外部服务：载体进程用 /bin/cat（把 stdin 原样吐回 stdout，正好是一个
// 完美的回环代理），失败路径用一个不存在的二进制和一句 `sh -c 'echo ... >&2; exit 3'`。

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

#include "net/ProxyCommandDialer.h"
#include "net/SocketUtil.h"

using namespace cubeshell;
using namespace cubeshell::socket_util;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 正常路径：起 /bin/cat 当回环载体，反复写进去再读回来。
//
// 刻意「写一块 → 立刻读回一块」而不是「写满 800KB 再一次读完」：后者会真的死锁
// ——两个方向各只有 256KB 的 socket 缓冲（kPairSocketBufferBytes），回程塞满之后
// 泵线程就不再读去程，测试进程也就永远写不下去。这不是实现的 bug，是任何
// 不读回程的对端都会撞上的事，而真实的 libssh2 一直在读。
static void testRoundTrip()
{
    CommandDialer dial = makeProxyCommandDialer();

    ProxyTransportPtr transport;
    QString err;
    const qintptr fd = dial(QStringLiteral("cat"), 5000, nullptr, &transport, &err);
    CHECK(fd >= 0);
    if (fd < 0) {
        qWarning() << "  dial failed:" << err;
        return;
    }
    CHECK(transport != nullptr);

    // 800KB 分 200 次搬完：足够跑过很多轮通知器回调，也跨过了单次 32KB 的
    // 中继缓冲边界（kRelayChunkBytes）。
    constexpr int kChunk = 4096;
    constexpr int kRounds = 200;
    QByteArray out(kChunk, 0);
    QByteArray back(kChunk, 0);
    bool allEqual = true;
    for (int r = 0; r < kRounds && allEqual; ++r) {
        // 每轮换内容，避免"读到的其实是上一轮残留"这种假通过。
        for (int i = 0; i < kChunk; ++i)
            out[i] = char((i * 31 + r * 7) & 0xff);

        if (!sendAll(fd, out.constData(), out.size(), 5000, &err)) {
            qWarning() << "  round" << r << "sendAll failed:" << err;
            allEqual = false;
            break;
        }
        if (!recvExact(fd, back.data(), back.size(), 5000, &err)) {
            qWarning() << "  round" << r << "recvExact failed:" << err;
            allEqual = false;
            break;
        }
        if (back != out) {
            qWarning() << "  round" << r << "payload mismatch";
            allEqual = false;
        }
    }
    CHECK(allEqual);

    // 拆除必须是快的、且不能挂住。这里量的就是「泵线程能被叫醒并按时退出」——
    // 唤醒路径坏掉时它会一直守着 fd，直到 kPumpJoinMs（5s）才放弃并泄漏线程。
    closeFd(fd);
    QElapsedTimer timer;
    timer.start();
    transport.reset();
    const qint64 teardownMs = timer.elapsed();
    CHECK(teardownMs < 2500);
    if (teardownMs >= 2500)
        qWarning() << "  teardown took" << teardownMs << "ms (泵线程没被及时叫醒)";
}

// 载体二进制不存在。
//
// 注意这里**不会**走 QProcess 的 FailedToStart：Unix 上命令是交给 /bin/sh -c 执行的
//（与 OpenSSH 一致，好让用户能原样粘贴 ~/.ssh/config 里的命令行），所以起来的是
// sh，它起得来得很，然后打一句 not found、以 127 退出。也就是说**拨号会成功返回**，
// 病因只在 stderr 和退出码里——这正是 ProxyTransport::diagnostics() 存在的理由，
// 没有它用户看到的就只有 libssh2 一句 "Failed getting banner"。
static void testMissingBinaryReportedViaDiagnostics()
{
    CommandDialer dial = makeProxyCommandDialer();
    ProxyTransportPtr transport;
    QString err;
    const qintptr fd = dial(QStringLiteral("/nonexistent/cubeshell-proxy-helper-xyz"),
                            3000, nullptr, &transport, &err);
    CHECK(fd >= 0);
    if (fd < 0) {
        qWarning() << "  dial failed:" << err;
        return;
    }

    // 载体一退出，libssh2 那一端必须立刻拿到 EOF，而不是干等到超时。
    char buf[16] = {0};
    CHECK(!recvExact(fd, buf, 1, 3000, &err));

    // 诊断里必须能看出是**哪个**命令没找到。shell 的措辞随 locale 变，所以只断言
    // 路径本身（各家 sh 都会把命令名带上）和退出码那一行。
    QString diag;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        diag = transport->diagnostics();
        if (diag.contains(QStringLiteral("cubeshell-proxy-helper-xyz"))
            && diag.contains(QStringLiteral("退出码")))
            break;
        QThread::msleep(20);
    }
    CHECK(diag.contains(QStringLiteral("cubeshell-proxy-helper-xyz")));
    CHECK(diag.contains(QStringLiteral("退出码")));
    if (!diag.contains(QStringLiteral("cubeshell-proxy-helper-xyz")))
        qWarning() << "  diagnostics:" << diag;

    closeFd(fd);
    transport.reset();
}

// 载体起得来、随即失败退出——这是 `nc` 遇到 connection refused 的真实形态。
// 拨号会**成功返回**（进程确实起来了），病因只在 stderr 里，所以
// diagnostics() 必须把它捞出来，否则用户只能看到 libssh2 一句 "Failed getting banner"。
static void testDiagnosticsFromDyingCarrier()
{
    CommandDialer dial = makeProxyCommandDialer();
    ProxyTransportPtr transport;
    QString err;
    const qintptr fd = dial(
        QStringLiteral("echo 'connect to 10.0.0.1 port 22 failed' >&2; exit 3"),
        3000, nullptr, &transport, &err);
    CHECK(fd >= 0);
    if (fd < 0) {
        qWarning() << "  dial failed:" << err;
        return;
    }

    // 载体一退出，libssh2 那一端应当立刻拿到 EOF 而不是干等超时。
    char buf[16] = {0};
    const bool gotEof = !recvExact(fd, buf, 1, 3000, &err);
    CHECK(gotEof);

    // stderr 与退出码都是异步收上来的，给它们一点时间落地（正常几毫秒）。
    // 必须等**两条都到**：只等 stderr 会在退出码那行还没 append 时就跳出去。
    QString diag;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        diag = transport->diagnostics();
        if (diag.contains(QStringLiteral("port 22 failed"))
            && diag.contains(QStringLiteral("退出码 3")))
            break;
        QThread::msleep(20);
    }
    CHECK(diag.contains(QStringLiteral("port 22 failed")));
    CHECK(diag.contains(QStringLiteral("退出码 3")));
    if (!diag.contains(QStringLiteral("port 22 failed"))
        || !diag.contains(QStringLiteral("退出码 3")))
        qWarning() << "  diagnostics:" << diag;

    closeFd(fd);
    transport.reset();
}

// 约定校验：timeoutMs == 0 表示「预算已耗尽」，不是「用默认值」。
// 搞反了的后果是多跳链最后一跳还能再白等 15 秒，整体超时形同虚设。
static void testExhaustedBudget()
{
    CommandDialer dial = makeProxyCommandDialer();
    ProxyTransportPtr transport;
    QString err;
    QElapsedTimer timer;
    timer.start();
    const qintptr fd = dial(QStringLiteral("cat"), 0, nullptr, &transport, &err);
    CHECK(fd < 0);
    CHECK(!err.isEmpty());
    CHECK(timer.elapsed() < 500);        // 立刻失败，不许再起进程
    CHECK(transport == nullptr);
    if (fd >= 0)
        closeFd(fd);
}

// 没人接手载体句柄 = 调用方的编程错误。宁可在这里明确失败，也不要交出一个
// 几毫秒后就变成断管的 fd（那会表现成"连上后随机掉线"，极难定位）。
static void testRefusesOrphanTransport()
{
    CommandDialer dial = makeProxyCommandDialer();
    QString err;
    const qintptr fd = dial(QStringLiteral("cat"), 3000, nullptr, nullptr, &err);
    CHECK(fd < 0);
    CHECK(!err.isEmpty());
    if (fd >= 0)
        closeFd(fd);
}

// 建连途中取消：cancelled 一开始就是 true，拨号必须放弃而不是先把进程起起来。
static void testCancelled()
{
    CommandDialer dial = makeProxyCommandDialer();
    ProxyTransportPtr transport;
    QString err;
    std::atomic<bool> cancelled{true};
    const qintptr fd = dial(QStringLiteral("cat"), 3000, &cancelled, &transport, &err);
    CHECK(fd < 0);
    CHECK(err.contains(QStringLiteral("取消")));
    if (fd >= 0)
        closeFd(fd);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testRoundTrip();
    testMissingBinaryReportedViaDiagnostics();
    testDiagnosticsFromDyingCarrier();
    testExhaustedBudget();
    testRefusesOrphanTransport();
    testCancelled();

    if (failures == 0)
        qInfo() << "proxy_command_test: all checks passed";
    else
        qWarning() << "proxy_command_test:" << failures << "check(s) failed";
    return failures == 0 ? 0 : 1;
}
