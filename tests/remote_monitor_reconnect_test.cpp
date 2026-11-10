// 默认离屏、无网络：监控/心跳换连接、UI 重订阅、旧帧隔离、延迟线程回收。
// --integration：真实终端连接 → 断线 → 点击重新连接 → 勾选远程监控 → 状态栏恢复。
#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

#include "main_window.h"
#include "device_list_widget.h"
#include "ssh_session_tab.h"
#include "ssh_terminal_widget.h"
#include "status_box_item.h"
#include "config/GlobalState.h"
#include "ssh/RemoteMonitor.h"
#include "ssh/SshClient.h"
#include "ssh/SshKeepaliveTimer.h"

using namespace cubeshell;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

namespace cubeshell {
struct RemoteMonitorReconnectTestAccess {
    static void services(SshSessionTab &tab, const std::shared_ptr<SshClient> &client) {
        tab.bindConnectionServices(client);
    }
    static void bind(MainWindow &window, SshSessionTab *tab) { window.bindMonitorToTab(tab); }
    static RemoteMonitor *source(MainWindow &window) { return window.m_monitorSource; }
    static SshKeepaliveTimer *keepalive(SshSessionTab &tab) { return tab.m_keepalive; }
    static SshClient *client(SshSessionTab &tab) { return tab.m_servicesClient.lock().get(); }
    static StatusBoxItem *memory(MainWindow &window) { return window.m_statusMem; }
    static StatusBoxItem *cpu(MainWindow &window) { return window.m_statusCpu; }
    static bool unbound(MainWindow &window) { return !window.m_monitorTab && !window.m_monitorSource; }
    static void running(RemoteMonitor &monitor) { monitor.m_running = true; }
    static int receivers(RemoteMonitor &monitor) {
        return monitor.receivers(SIGNAL(statsUpdated(cubeshell::RemoteStats)));
    }
    static void stall(RemoteMonitor &monitor, QSemaphore &gate) {
        monitor.m_running = true;
        monitor.m_thread = QThread::create([&gate]() { gate.acquire(); });
        monitor.m_thread->start();
    }
};
}
using Access = RemoteMonitorReconnectTestAccess;

static void events()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static bool waitUntil(const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        events();
        QThread::msleep(5);
    }
    return done();
}

static QCheckBox *checkbox(MainWindow &window)
{
    for (auto *box : window.findChildren<QCheckBox *>())
        if (box->text() == QStringLiteral("远程监控")) return box;
    return nullptr;
}

static void publish(RemoteMonitor &monitor, double memoryUsage)
{
    RemoteStats stats;
    stats.memory.usagePercent = memoryUsage;
    Access::running(monitor); // 离线模拟已在采集，不创建网络线程
    emit monitor.statsUpdated(stats);
}

static bool hasMemory(MainWindow &window, double memoryUsage)
{
    return Access::memory(window)->text() == QStringLiteral("MEM: %1%").arg(memoryUsage, 0, 'f', 2)
        && !Access::memory(window)->isHidden();
}

static void testReconnectAndSubscriptions()
{
    MainWindow window;
    auto *box = checkbox(window);
    CHECK(box);
    if (!box) return;
    auto *tab = new SshSessionTab(DeviceEntry{});
    tab->setMonitoringEnabled(false);
    auto first = std::make_shared<SshClient>();
    std::weak_ptr<SshClient> oldClient = first;
    Access::services(*tab, first);
    QPointer<RemoteMonitor> oldMonitor = tab->monitor();
    QPointer<SshKeepaliveTimer> oldKeepalive = Access::keepalive(*tab);
    CHECK(oldMonitor && oldKeepalive);
    CHECK(!oldMonitor->isRunning());

    // 首次绑定时 monitor 已经存在，仍必须订阅以后重连产生的 monitorReady。
    Access::bind(window, tab);
    box->setChecked(true);
    publish(*oldMonitor, 12.5);
    events();
    CHECK(hasMemory(window, 12.5));
    Access::services(*tab, first); // 同一连接重复通知不重建
    CHECK(tab->monitor() == oldMonitor);

    // 旧帧已入队，紧接着用新的 SshClient 重建；旧数据不得出现在新连接的状态栏。
    publish(*oldMonitor, 99.0);
    auto second = std::make_shared<SshClient>();
    Access::services(*tab, second);
    first.reset();
    CHECK(tab->monitor() != oldMonitor);
    CHECK(oldKeepalive.isNull());
    CHECK(Access::source(window) == tab->monitor());
    CHECK(Access::client(*tab) == second.get());
    events();
    CHECK(oldMonitor.isNull());
    CHECK(oldClient.expired());
    CHECK(Access::memory(window)->isHidden());
    publish(*tab->monitor(), 23.0);
    events();
    CHECK(hasMemory(window, 23.0));

    // monitorReady 不应叠加多个 statsUpdated 接收器。
    const int receivers = Access::receivers(*tab->monitor());
    CHECK(receivers == 1);
    emit tab->monitorReady();
    emit tab->monitorReady();
    CHECK(Access::receivers(*tab->monitor()) == receivers);

    // 关闭后立刻打开，旧帧的源对象仍是同一个，必须靠订阅代次过滤。
    publish(*tab->monitor(), 88.0);
    box->setChecked(false);
    CHECK(!tab->monitor()->isRunning());
    CHECK(Access::memory(window)->isHidden());
    box->setChecked(true);
    Access::running(*tab->monitor());
    events();
    CHECK(Access::memory(window)->isHidden());
    publish(*tab->monitor(), 24.0);
    events();
    CHECK(hasMemory(window, 24.0));

    // 快速切换标签时，旧标签及其迟到的 monitorReady 都不能抢回当前订阅。
    auto *other = new SshSessionTab(DeviceEntry{});
    other->setMonitoringEnabled(false);
    Access::services(*other, std::make_shared<SshClient>());
    publish(*tab->monitor(), 77.0);
    Access::bind(window, other);
    emit tab->monitorReady();
    events();
    CHECK(Access::source(window) == other->monitor());
    CHECK(Access::memory(window)->isHidden());
    publish(*other->monitor(), 34.0);
    events();
    CHECK(hasMemory(window, 34.0));
    emit other->disconnected();
    CHECK(Access::memory(window)->isHidden());
    CHECK(!Access::source(window));

    // 删除当前标签会清空 QPointer 和 UI；已投递的数据不能访问已销毁的标签。
    emit other->monitorReady();
    publish(*other->monitor(), 66.0);
    delete other;
    events();
    CHECK(Access::unbound(window));
    CHECK(Access::memory(window)->isHidden());
    delete tab;
    events();
}

static void testRetireStalledWorker()
{
    auto client = std::make_shared<SshClient>();
    std::weak_ptr<SshClient> weak = client;
    auto *parent = new QObject;
    QPointer<RemoteMonitor> monitor = new RemoteMonitor(client, parent);
    client.reset();
    QSemaphore gate;
    Access::stall(*monitor, gate);
    monitor->retire(); // 3 秒有界等待后，仍要保持对象/连接存活
    CHECK(monitor && !monitor->parent());
    CHECK(!weak.expired());
    delete parent;
    CHECK(monitor);
    monitor->start(); // 退休对象不能重新启动
    CHECK(!monitor->isRunning());
    gate.release();
    CHECK(waitUntil([&]() { return monitor.isNull(); }));
    CHECK(weak.expired());
}

static DeviceEntry testDevice()
{
    DeviceEntry device;
    device.host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1") + QLatin1Char(':')
        + qEnvironmentVariable("CUBESSH_PORT", "2222");
    device.username = qEnvironmentVariable("CUBESSH_USER", "testuser");
    device.password = qEnvironmentVariable("CUBESSH_PASS", "testpass123");
    return device;
}

static bool clickReconnect(SshSessionTab &tab)
{
    for (auto *button : tab.terminal()->findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("重新连接")) {
            button->click();
            return true;
        }
    }
    return false;
}

static bool liveStatsReady(MainWindow &window)
{
    // 不仅收到第一帧，还要完成第二次采样，确认 CPU 差值与持续刷新确实恢复。
    return !Access::memory(window)->isHidden()
        && Access::memory(window)->text() != QStringLiteral("MEM: —")
        && Access::cpu(window)->text() != QStringLiteral("CPU: —");
}

static int testLiveReconnect()
{
    const DeviceEntry device = testDevice();
    // 快速探测测试服务，避免缺服务时停在终端交互认证/提示上。
    SshClient probe;
    probe.setHost(device.hostPort().host, device.hostPort().port);
    probe.setUsername(device.username);
    probe.setPassword(device.password);
    probe.setConnectTimeoutMs(3000);
    SshError error;
    if (!probe.connectToHost(nullptr, error)) {
        qWarning() << "SKIP: test sshd unavailable:" << error.message;
        return 2;
    }
    probe.disconnectFromHost();

    // 仅对隔离的测试进程关闭首次主机确认弹窗，不写入配置或真实 known_hosts。
    GlobalState::instance().setHostKeyVerification(int(HostKeyVerification::Off));
    GlobalState::instance().setSshConnectTimeoutSeconds(3);
    GlobalState::instance().setSshKeepaliveEnabled(true);
    MainWindow window;
    SshSessionTab tab(device);
    Access::bind(window, &tab);
    auto *box = checkbox(window);
    CHECK(box);
    if (!box) return 1;
    int connected = 0;
    int ready = 0;
    QObject::connect(&tab, &SshSessionTab::connected, &window, [&]() { ++connected; });
    QObject::connect(&tab, &SshSessionTab::monitorReady, &window, [&]() {
        ++ready;
        tab.monitor()->setIntervalMs(200);
    });
    tab.connectToHost();
    CHECK(waitUntil([&]() { return connected == 1; }));
    if (connected != 1) return 1;
    CHECK(tab.monitor() && !tab.monitor()->isRunning());
    CHECK(Access::keepalive(tab)->isRunning());
    box->setChecked(true);
    CHECK(waitUntil([&]() { return liveStatsReady(window); }));
    CHECK(tab.monitor()->isRunning());
    box->setChecked(false);
    CHECK(!tab.monitor()->isRunning());
    CHECK(Access::memory(window)->isHidden());

    QPointer<RemoteMonitor> firstMonitor = tab.monitor();
    QPointer<SshKeepaliveTimer> firstKeepalive = Access::keepalive(tab);
    tab.terminal()->notifyConnectionLost(QStringLiteral("test reconnect while unchecked"));
    CHECK(!Access::source(window));
    CHECK(clickReconnect(tab));
    CHECK(waitUntil([&]() { return connected == 2; }));
    if (connected != 2) return 1;
    CHECK(ready == 2);
    CHECK(firstKeepalive.isNull());
    CHECK(waitUntil([&]() { return firstMonitor.isNull(); }));
    CHECK(Access::client(tab) == tab.terminal()->sshClient().get());
    CHECK(Access::keepalive(tab)->isRunning());
    CHECK(!tab.monitor()->isRunning());
    CHECK(Access::memory(window)->isHidden());
    // 用户报告的路径：重连后才勾选，必须恢复真正的底部指标。
    box->setChecked(true);
    CHECK(waitUntil([&]() { return liveStatsReady(window); }));
    CHECK(Access::memory(window)->text() != QStringLiteral("MEM: —"));

    // 另一条路径：保持勾选重连，不需反复取消/勾选就应自动恢复。
    tab.terminal()->notifyConnectionLost(QStringLiteral("test reconnect while checked"));
    CHECK(Access::memory(window)->isHidden());
    CHECK(clickReconnect(tab));
    CHECK(waitUntil([&]() { return connected == 3 && liveStatsReady(window); }));
    CHECK(ready == 3);
    CHECK(box->isChecked() && tab.monitor()->isRunning());
    CHECK(Access::keepalive(tab)->isRunning());
    box->setChecked(false);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", home.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", home.filePath("data").toUtf8());
    QDir::setCurrent(home.path()); // 禁止 loadDevices 的开发目录回落读取仓库配置
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--integration")))
        return testLiveReconnect();
    testReconnectAndSubscriptions();
    testRetireStalledWorker();
    qInfo() << "remote_monitor_reconnect_test:" << failures << "failure(s)";
    return failures ? 1 : 0;
}
