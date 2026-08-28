// ConnectionTester.cpp — 「测试连接」后台探测。见 ConnectionTester.h。

#include "ConnectionTester.h"

#include <QTimer>
#include <QThread>

#include "ssh/SshClient.h"

namespace cubeshell {

namespace {
// 兜底超时：改之前 SSH 的阻塞 connect() 自身不带超时（内核 SYN 重传可达一两
// 分钟），这张表是唯一的超时。现在 SshClient 自带预算，本表退化为"预算失效时
// 的最后一道闸"，取值见 sshGuardMsFor()。
//
// 兜底表比预算多留的余量：预算只管 TCP+握手+认证，之后还有 disconnectFromHost
// 与线程回切。余量太小会让正常收尾被判成超时。
constexpr int kSshGuardMarginMs = 3000;
constexpr int kTcpGuardMs  = 12000;  // 兜底；TcpClient 自身 connectTimeoutMs 更短
constexpr int kTcpConnectMs = 8000;  // 写给 TcpSettings.connectTimeoutMs
} // namespace

// SSH 探测结果 + 代数令牌。gen 用来识别「上一个已超时/被取代的 worker」迟到
// 的结果，避免它误收新一次测试的果子（见 onSshDone）。
struct ConnectionTester::SshResult {
    quint64 gen = 0;
    bool ok = false;
    bool authFailed = false;
    QString msg;
};

ConnectionTester::ConnectionTester(QObject *parent)
    : QObject(parent)
{
    m_guard = new QTimer(this);
    m_guard->setSingleShot(true);
    connect(m_guard, &QTimer::timeout, this, [this] {
        // 先按失败回报（finish 会停表、清 m_running、发 finished），再尽力打断
        // 底层连接。顺序不能反：cancel() 也会清 m_running，先 cancel 会让 finish
        // 的闸门把这条超时结果吞掉，UI 就卡在「测试中」了。对黑 hole 主机的阻塞
        // connect 无能为力，那个靠 detach 收场（见 testSsh 注释）。
        finish(false, tr("连接超时"));
        cancel();
    });
}

ConnectionTester::~ConnectionTester()
{
    cancel();      // best-effort：能打断 handshake/auth/read 阶段的阻塞
    cleanupTcp();  // TCP 对象随本对象回收
    // SSH worker 刻意不 join：它可能仍阻塞在内核 connect()（黑 hole 主机），
    // join 会让 UI 死等。finished→deleteLater 让它结束后自我回收；发向本对象
    // 的 queued 结果在析构时被 Qt 自动丢弃（context 已销毁）。
}

void ConnectionTester::cancel()
{
    // 递增代数：任何迟到的 SSH 结果都会因 gen 不匹配被丢弃。
    ++m_generation;
    if (m_sshClient)
        m_sshClient->shutdownSocket();
    if (m_tcp)
        m_tcp->disconnectFromHost();
    // Serial 是同步 open，不存在「进行中」可取消。
    // 清态但不发 finished()：取消的反馈由调用方（对话框）自己给，
    // 析构路径也不该再发信号。
    m_running = false;
    m_guard->stop();
}

// ---------------------------------------------------------------- SSH ----

bool ConnectionTester::testSsh(const DeviceEntry &entry)
{
    if (m_running)
        return false;
    m_running = true;
    const quint64 gen = ++m_generation;

    const HostPort hp = entry.hostPort();
    auto client = std::make_shared<SshClient>();
    client->setHost(hp.host, hp.port);
    client->setUsername(entry.username);
    if (entry.usesKey())
        client->setPrivateKey(entry.keyType, entry.keyFile);
    else
        client->setPassword(entry.password);
    // 这里不设 setConnectTimeoutMs：不设时 SshClient 自己会去「设置 → 通用」
    // 取那个「SSH 连接超时」（见 effectiveConnectTimeoutMs），跟终端建连路径
    // 走同一份预算——「测试连接」通了而实连超时，或者反过来，都会很难解释。
    //
    // 代理必须一起测：不带代理去测，内网设备会显示"连接超时"，而真正连接时
    // 走代理明明是通的——「测试连接」给出的结论与实际相反，比没有这个按钮更糟。
    client->setProxyConfig(entry.proxy);
    m_sshClient = client;   // cancel() 靠它 shutdownSocket

    auto result = std::make_shared<SshResult>();
    result->gen = gen;

    // MFA / keyboard-interactive 的测试不做交互式应答（传 nullptr）：
    // 「测试连接」只验证密码 / 私钥这类可无人值守的凭据。
    //
    // worker 不 join（见析构注释）。结果经 finished 信号 + queued 回切到本
    // 对象所在线程；shared_ptr 让 client/result 的生命期不依赖本对象。
    QThread *worker = QThread::create([client, result]() {
        SshError err;
        result->ok = client->connectToHost(nullptr, err);
        result->authFailed = err.authFailed;
        result->msg = err.message;
        if (result->ok)
            client->disconnectFromHost();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, result]() {
                onSshDone(result);
            }, Qt::QueuedConnection);
    worker->start();

    startGuardTimer(sshGuardMsFor(*client));
    return true;
}

// 兜底表要盖住 SshClient 自己那份建连预算，不能是个写死的 15 秒。
//
// 改之前 SSH 的三段（TCP/握手/认证）一个超时都没有，这张表就是**唯一**的超时，
// 15 秒够用。现在预算由设置页决定：用户设成 30 秒时，写死的 15 秒会先到点，
// 于是明明还在正常握手的连接被稳定误报成"连接超时"。
//
// 用 effectiveConnectTimeoutMs() 而不是 connectTimeoutMs()：后者在"没显式设过"
// 时返回 0，那正是本函数最需要知道真实预算的情形（本类就从不显式设它）。
int ConnectionTester::sshGuardMsFor(const SshClient &client)
{
    return client.effectiveConnectTimeoutMs() + kSshGuardMarginMs;
}

void ConnectionTester::onSshDone(const std::shared_ptr<SshResult> &result)
{
    // 迟到结果（上一个已超时/被取代的 worker）：直接丢弃，不碰当前状态。
    if (result->gen != m_generation)
        return;
    // 本代 worker 已结束，放开引用让 shared_ptr 回收（worker 自己也持有一份，
    // 此处 reset 不会提前析构）。QThread 对象由 finished→deleteLater 自我回收。
    m_sshClient.reset();

    if (!m_running)   // 已超时/取消，finish 已报过
        return;

    if (result->ok) {
        finish(true, tr("连接成功，认证通过"));
    } else if (result->authFailed) {
        finish(false, result->msg.isEmpty()
                          ? tr("认证失败：用户名或密码/密钥错误")
                          : tr("认证失败：%1").arg(result->msg));
    } else {
        finish(false, result->msg.isEmpty() ? tr("连接失败") : result->msg);
    }
}

// -------------------------------------------------------------- TCP 等 ----

bool ConnectionTester::testTcp(const DeviceEntry &entry)
{
    if (m_running)
        return false;
    m_running = true;
    ++m_generation;   // 让任何迟到的 SSH 结果失效

    const HostPort hp = entry.hostPort();

    cleanupTcp();
    m_tcp = new TcpClient(this);
    connect(m_tcp, &TcpClient::connected, this, [this] {
                cleanupTcp();
                finish(true, tr("连接成功"));
            });
    connect(m_tcp, &TcpClient::errorOccurred, this, [this](const QString &msg) {
                cleanupTcp();
                finish(false, msg.isEmpty() ? tr("连接失败") : msg);
            });

    TcpSettings s;
    // telnet 与裸 tcp 在「可达性」层面完全一样；rdp 也借这条路只测 host:port。
    s.mode = entry.isTelnet() ? QStringLiteral("telnet") : QStringLiteral("tcp");
    s.host = hp.host;
    s.port = hp.port;
    s.connectTimeoutMs = kTcpConnectMs;

    if (s.host.isEmpty() || !m_tcp->connectToHost(s)) {
        // 参数非法 / 立即失败。errorOccurred 可能已同步发过一次（finish 的闸门
        // 会挡掉重复），这里再以 queued 方式兜一条，保持与其它路径一致的异步语义。
        cleanupTcp();
        const QString target = hp.host;
        QTimer::singleShot(0, this, [this, target] {
            finish(false, tr("无法连接 %1").arg(target));
        });
        return true;
    }

    startGuardTimer(kTcpGuardMs);
    return true;
}

void ConnectionTester::cleanupTcp()
{
    if (!m_tcp)
        return;
    disconnect(m_tcp, nullptr, this, nullptr);  // 防 deleteLater 前再发信号
    m_tcp->disconnectFromHost();
    m_tcp->deleteLater();
    m_tcp = nullptr;
}

// ------------------------------------------------------------- Serial ----

#ifdef CUBESHELL_WITH_SERIAL
bool ConnectionTester::testSerial(const SerialSettings &settings)
{
    if (m_running)
        return false;
    m_running = true;
    ++m_generation;

    if (settings.portName.isEmpty()) {
        QTimer::singleShot(0, this, [this] { finish(false, tr("未指定串口设备")); });
        return true;
    }

    // open() 是同步的，直接在 UI 线程跑；串口不存在/被占用会立刻返回错误。
    SerialClient client;
    QString errMsg;
    connect(&client, &SerialClient::errorOccurred, this,
            [&errMsg](const QString &m) { errMsg = m; });
    const bool ok = client.open(settings);
    if (ok)
        client.close();
    finish(ok, ok ? tr("连接成功")
                  : (errMsg.isEmpty() ? tr("无法打开串口") : errMsg));
    return true;
}
#endif // CUBESHELL_WITH_SERIAL

// ------------------------------------------------------------- 内部 ----

void ConnectionTester::startGuardTimer(int ms)
{
    m_guard->start(ms);
}

void ConnectionTester::finish(bool ok, const QString &message)
{
    if (!m_running)
        return;
    m_running = false;
    m_guard->stop();
    emit finished(ok, message);
}

} // namespace cubeshell
