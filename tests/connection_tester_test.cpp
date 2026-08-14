// ConnectionTester unit test — 「添加设备」对话框的测试连接后台探测。
//
// 无外部依赖即可跑的部分（始终运行）：
//   - TCP 可达：本地 QTcpServer 监听 → 成功。
//   - TCP 不可达：连接已关闭端口（refused，快速返回）→ 失败。
//   - SSH 不可达：连接已关闭端口（refused，快速返回）→ 失败。
//   - 重入闸门：探测进行中再次 start 返回 false；cancel 后可重新开始。
//
// SSH 成功路径需要可达的 sshd，由环境变量驱动（与 ssh_integration_test 同约定）：
//   CUBESSH_HOST / CUBESSH_PORT / CUBESSH_USER / CUBESSH_PASS
// 未设置时跳过该子项（不影响整体通过），避免把一个可选集成项变成硬失败。

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QTcpServer>
#include <QTimer>

#include "ConnectionTester.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 连接 tester.finished 到本地事件循环，返回是否等到了结果；结果经出参带回。
static bool waitFinished(ConnectionTester &tester, bool *okOut, QString *msgOut, int timeoutMs)
{
    QEventLoop loop;
    bool got = false;
    *okOut = false;
    QObject::connect(&tester, &ConnectionTester::finished, &loop,
                     [&](bool ok, const QString &msg) {
                         got = true;
                         *okOut = ok;
                         *msgOut = msg;
                         loop.quit();
                     });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return got;
}

static DeviceEntry tcpEntry(const QString &host, quint16 port, const QString &proto)
{
    DeviceEntry e;
    e.protocol = proto;
    e.host = formatHostPort(host, port);
    e.port = port;
    return e;
}

// 取一个当前没人监听的本地端口：先listen拿到空端口再立刻关掉。
static quint16 closedLocalPort()
{
    QTcpServer s;
    if (!s.listen(QHostAddress::LocalHost, 0))
        return 9;   // 9 = discard，通常没人监听
    const quint16 p = s.serverPort();
    s.close();
    return p;
}

static void testTcpSuccess()
{
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    if (!server.isListening())
        return;

    ConnectionTester tester;
    const DeviceEntry e = tcpEntry(QStringLiteral("127.0.0.1"), server.serverPort(),
                                   QStringLiteral("tcp"));
    CHECK(tester.testTcp(e));
    bool ok = false;
    QString msg;
    const bool got = waitFinished(tester, &ok, &msg, 8000);
    CHECK(got);
    CHECK(ok);
    qInfo() << "tcp success:" << msg;
}

static void testTcpFailure()
{
    ConnectionTester tester;
    const DeviceEntry e = tcpEntry(QStringLiteral("127.0.0.1"), closedLocalPort(),
                                   QStringLiteral("tcp"));
    CHECK(tester.testTcp(e));
    bool ok = true;
    QString msg;
    const bool got = waitFinished(tester, &ok, &msg, 8000);
    CHECK(got);
    CHECK(!ok);           // refused → 失败
    CHECK(!msg.isEmpty());
    qInfo() << "tcp failure (expected):" << msg;
}

static void testTelnetUsesTcpPath()
{
    // telnet 与 tcp 在可达性层面同一条路：本地起监听即应判定成功。
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    if (!server.isListening())
        return;

    ConnectionTester tester;
    const DeviceEntry e = tcpEntry(QStringLiteral("127.0.0.1"), server.serverPort(),
                                   QStringLiteral("telnet"));
    CHECK(tester.testTcp(e));
    bool ok = false;
    QString msg;
    CHECK(waitFinished(tester, &ok, &msg, 8000));
    CHECK(ok);
}

static void testReentrancyGuard()
{
    // 不可路由地址（TEST-NET-3）：连接会挂起直到超时，足以观察「进行中」态。
    ConnectionTester tester;
    const DeviceEntry e = tcpEntry(QStringLiteral("203.0.113.1"), 65000,
                                   QStringLiteral("tcp"));
    CHECK(tester.testTcp(e));        // 发起成功，进入 Connecting
    CHECK(tester.isRunning());
    CHECK(!tester.testTcp(e));       // 重入被拒
    tester.cancel();
    CHECK(!tester.isRunning());
    // cancel 后可重新开始（状态没有被卡死）。
    const DeviceEntry closed = tcpEntry(QStringLiteral("127.0.0.1"), closedLocalPort(),
                                        QStringLiteral("tcp"));
    CHECK(tester.testTcp(closed));
    bool ok = true;
    QString msg;
    CHECK(waitFinished(tester, &ok, &msg, 8000));
    CHECK(!ok);
}

static void testSshFailureRefused()
{
    // 连已关闭端口 → TCP refused，快速失败（不会触发 15s 兜底）。
    ConnectionTester tester;
    DeviceEntry e;
    e.protocol = QStringLiteral("ssh");
    e.host = formatHostPort(QStringLiteral("127.0.0.1"), closedLocalPort());
    e.username = QStringLiteral("user");
    e.password = QStringLiteral("pass");
    CHECK(tester.testSsh(e));
    bool ok = true;
    QString msg;
    const bool got = waitFinished(tester, &ok, &msg, 15000);
    CHECK(got);
    CHECK(!ok);
    CHECK(!msg.isEmpty());
    qInfo() << "ssh failure (expected):" << msg;
}

// 可选：真实 sshd 的成功路径。未配置环境变量则跳过。
static void testSshSuccessOptional()
{
    const QString host = qEnvironmentVariable("CUBESSH_HOST");
    if (host.isEmpty()) {
        qInfo() << "ssh success: skipped (CUBESSH_HOST not set)";
        return;
    }
    const quint16 port = quint16(qEnvironmentVariableIntValue("CUBESSH_PORT"));
    DeviceEntry e;
    e.protocol = QStringLiteral("ssh");
    e.host = formatHostPort(host, port ? port : 22);
    e.username = qEnvironmentVariable("CUBESSH_USER");
    e.password = qEnvironmentVariable("CUBESSH_PASS");

    ConnectionTester tester;
    CHECK(tester.testSsh(e));
    bool ok = false;
    QString msg;
    CHECK(waitFinished(tester, &ok, &msg, 15000));
    CHECK(ok);
    qInfo() << "ssh success:" << msg;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testTcpSuccess();
    testTcpFailure();
    testTelnetUsesTcpPath();
    testReentrancyGuard();
    testSshFailureRefused();
    testSshSuccessOptional();

    if (failures == 0)
        qInfo() << "connection_tester_test: all checks passed";
    else
        qWarning() << "connection_tester_test:" << failures << "check(s) failed";
    return failures == 0 ? 0 : 1;
}
