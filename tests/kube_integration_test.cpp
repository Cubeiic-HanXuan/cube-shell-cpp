// KubeManager 远程后端集成测试：经真实 SSH 会话（tests/docker/kube 的堡垒机）
// 驱动 kubectl 的完整链路 —— 可用性探测 / 上下文 / 命名空间 / 资源拉取 /
// YAML / 扩缩容 / 日志流 / 端口转发。
//
// 环境：先跑 tests/docker/kube/up.sh（堡垒机 127.0.0.1:2403，k8sops/cubeshell）。
// 连不上时 exit(2) → CTest 记 SKIPPED（与 command_executor_test 同一约定）。
//
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS,
//                CUBEKUBE_NS（默认 cube-test）。

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>

#include "kube/KubeManager.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 事件泵等待：信号来自工作线程（QueuedConnection），必须让事件循环转。
template <typename F>
static bool waitFor(int timeoutMs, F &&pred)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2403").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "k8sops");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "cubeshell");
    const QString ns = qEnvironmentVariable("CUBEKUBE_NS", "cube-test");

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "SKIP — connect failed（先跑 tests/docker/kube/up.sh）:" << err.message;
        return 2;
    }
    qInfo() << "connected to" << host << port;

    CommandExecutor executor(&client);
    KubeManager manager;
    manager.setRemoteExecutor(&executor);
    manager.setRemoteUser(user);
    // 独占流工厂：日志/端口转发用（对齐 MainWindow::ensureKubeManager）。
    manager.setExecutorFactory([&client]() -> CommandExecutor * {
        return new CommandExecutor(&client);
    });
    manager.setNamespace(ns);

    // --- 信号捕获 ---
    bool availabilityOk = false;
    QString versionText;
    QObject::connect(&manager, &KubeManager::availabilityReady, &app,
                     [&](bool ok, const QString &text) {
                         availabilityOk = ok;
                         versionText = text;
                     }, Qt::QueuedConnection);

    QList<KubeContextInfo> contexts;
    QObject::connect(&manager, &KubeManager::contextsUpdated, &app,
                     [&](const QList<KubeContextInfo> &c) { contexts = c; },
                     Qt::QueuedConnection);

    QStringList namespaces;
    QObject::connect(&manager, &KubeManager::namespacesUpdated, &app,
                     [&](const QStringList &l) { namespaces = l; }, Qt::QueuedConnection);

    QHash<QString, QList<KubeResourceRow>> rowsByKind;
    QObject::connect(&manager, &KubeManager::resourcesUpdated, &app,
                     [&](const QString &, const QString &apiPlural,
                         const QList<KubeResourceRow> &rows) {
                         rowsByKind.insert(apiPlural, rows);
                     }, Qt::QueuedConnection);

    QString yamlText;
    QObject::connect(&manager, &KubeManager::yamlReady, &app,
                     [&](const KubeObjectRef &, const QString &text) { yamlText = text; },
                     Qt::QueuedConnection);

    bool opSuccess = false;
    QString opMessage;
    QObject::connect(&manager, &KubeManager::operationFinished, &app,
                     [&](bool success, const QString &, const KubeObjectRef &,
                         const QString &message) {
                         opSuccess = success;
                         opMessage = message;
                     }, Qt::QueuedConnection);

    QString describeText;
    QObject::connect(&manager, &KubeManager::textReady, &app,
                     [&](const QString &, const QString &text) { describeText = text; },
                     Qt::QueuedConnection);

    QString logText;
    int logExitCode = -1;
    QObject::connect(&manager, &KubeManager::logChunk, &app,
                     [&](const QString &text) { logText += text; }, Qt::QueuedConnection);
    QObject::connect(&manager, &KubeManager::logsFinished, &app,
                     [&](int exitCode) { logExitCode = exitCode; }, Qt::QueuedConnection);

    // --- 1. 可用性探测 ---
    manager.checkAvailability();
    CHECK(waitFor(30000, [&] { return availabilityOk || !versionText.isEmpty(); }));
    CHECK(availabilityOk);
    qInfo() << "kubectl version:" << versionText;

    // --- 2. 上下文 / 命名空间 ---
    manager.refreshContexts();
    CHECK(waitFor(30000, [&] { return !contexts.isEmpty(); }));
    CHECK(manager.currentContext().isEmpty() == false); // 跟随 kubeconfig current-context

    manager.refreshNamespaces();
    CHECK(waitFor(30000, [&] { return namespaces.contains(ns); }));

    // --- 3. 资源拉取（pods 应含 web 与 crashloop 摘要） ---
    manager.refreshKind(QStringLiteral("pods"));
    CHECK(waitFor(30000, [&] { return rowsByKind.contains(QStringLiteral("pods")); }));
    {
        const QList<KubeResourceRow> pods = rowsByKind.value(QStringLiteral("pods"));
        bool sawCrashLoop = false;
        bool sawWeb = false;
        for (const KubeResourceRow &row : pods) {
            if (row.status.contains(QStringLiteral("CrashLoopBackOff")))
                sawCrashLoop = true;
            if (row.name.startsWith(QStringLiteral("web-"))
                && row.status.contains(QStringLiteral("Running")))
                sawWeb = true;
        }
        CHECK(sawCrashLoop); // waiting.reason 优先于 phase 的远程验证
        CHECK(sawWeb);
    }

    // --- 4. YAML / describe ---
    KubeObjectRef webPod;
    {
        const QList<KubeResourceRow> pods = rowsByKind.value(QStringLiteral("pods"));
        for (const KubeResourceRow &row : pods) {
            if (row.name.startsWith(QStringLiteral("web-"))) {
                webPod.apiPlural = QStringLiteral("pods");
                webPod.name = row.name;
                webPod.namespace_ = ns;
                break;
            }
        }
    }
    CHECK(!webPod.name.isEmpty());

    manager.fetchYaml(webPod);
    CHECK(waitFor(30000, [&] { return yamlText.contains(QStringLiteral("kind: Pod")); }));

    manager.fetchDescribe(webPod);
    CHECK(waitFor(30000, [&] { return describeText.contains(webPod.name); }));

    // --- 5. 扩缩容 2→3→2（operationFinished 后自动回查 deployments） ---
    KubeObjectRef webDeploy{QStringLiteral("deployments"), QStringLiteral("web"), ns};
    rowsByKind.remove(QStringLiteral("deployments"));
    opSuccess = false;
    manager.scaleResource(webDeploy, 3);
    CHECK(waitFor(30000, [&] { return opSuccess; }));
    CHECK(waitFor(30000, [&] { return rowsByKind.contains(QStringLiteral("deployments")); }));

    opSuccess = false;
    manager.scaleResource(webDeploy, 2);
    CHECK(waitFor(30000, [&] { return opSuccess; }));

    // --- 6. 日志流（follow=false，sidecar 每 5 秒一行） ---
    KubeObjectRef multiPod{QStringLiteral("pods"), QStringLiteral("multi-container"), ns};
    manager.startPodLogs(multiPod, QStringLiteral("sidecar"), 3, /*follow=*/false);
    CHECK(waitFor(30000, [&] { return logExitCode >= 0; }));
    CHECK(logText.contains(QStringLiteral("sidecar-log")));

    // --- 7. 端口转发（独占 executor）+ 堡垒机内 curl 验证 + 停止即断 ---
    KubeObjectRef webSvc{QStringLiteral("services"), QStringLiteral("web"), ns};
    const int forwardId = manager.startPortForward(webSvc, 18082, 80);
    CHECK(forwardId > 0);
    CHECK(manager.portForwards().size() == 1);
    if (forwardId > 0) {
        // 远程模式下监听端在堡垒机：用共享 executor 在堡垒机上 curl。
        // port-forward 起来需要一点时间，重试几次。
        QString body;
        for (int i = 0; i < 10 && body.isEmpty(); ++i) {
            QThread::msleep(500);
            const ExecResult res = executor.exec(
                QStringLiteral("curl -s --max-time 3 127.0.0.1:18082 | head -1"));
            body = res.stdoutText.trimmed();
        }
        CHECK(body.contains(QStringLiteral("html"), Qt::CaseInsensitive));

        manager.stopPortForward(forwardId);
        CHECK(waitFor(10000, [&] { return manager.portForwards().isEmpty(); }));
        QThread::msleep(500);
        const ExecResult after = executor.exec(
            QStringLiteral("curl -s --max-time 3 127.0.0.1:18082 | head -1"));
        CHECK(after.stdoutText.trimmed().isEmpty()); // 停止即断
    }

    client.disconnectFromHost();
    qInfo() << (failures == 0 ? "KUBE INTEGRATION ALL PASS" : "KUBE INTEGRATION FAILURES")
            << failures;
    return failures == 0 ? 0 : 1;
}
