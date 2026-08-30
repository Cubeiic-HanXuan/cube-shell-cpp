// KubeManager.cpp — see KubeManager.h.
// 对应 docs/Kubernetes功能实现方案.md §4；线程纪律逐行对齐 docker/DockerManager.cpp。

#include "kube/KubeManager.h"

#include "ssh/CommandExecutor.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtConcurrent>

namespace cubeshell {

namespace {

// 本地后端子进程环境：macOS 以 GUI 方式启动（Finder/Dock/IDE 之外的
// launchd 会话）时 PATH 只有 /usr/bin:/bin…，不含 /usr/local/bin、
// /opt/homebrew/bin 等 kubectl 常见安装目录，导致"本机未检测到 kubectl"。
// 在系统环境基础上补齐这些目录（已存在不重复），与本机 shell 行为对齐。
QProcessEnvironment localProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_UNIX
    const QString pathKey = QStringLiteral("PATH");
    QStringList dirs = env.value(pathKey).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString home = QDir::homePath();
    const QStringList extra = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/usr/local/sbin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/opt/homebrew/sbin"),
        home + QStringLiteral("/.local/bin"),
        home + QStringLiteral("/bin"),
    };
    for (const QString &dir : extra) {
        if (!dirs.contains(dir))
            dirs.append(dir);
    }
    env.insert(pathKey, dirs.join(QLatin1Char(':')));
#endif
    return env;
}

} // namespace


KubeManager::KubeManager(QObject *parent)
    : QObject(parent)
{
    // 跨线程队列信号的值类型注册（同 DockerManager 构造）。
    qRegisterMetaType<QList<KubeContextInfo>>();
    qRegisterMetaType<QList<KubeResourceRow>>();
    qRegisterMetaType<KubeObjectRef>();
    qRegisterMetaType<KubePortForwardInfo>();
}

KubeManager::~KubeManager()
{
    stopPodLogs();
    // 独占日志 executor 还活着（流未自然结束）时直接销毁：析构即 cancel+join。
    if (m_logsExecutor) {
        delete m_logsExecutor;
        m_logsExecutor = nullptr;
    }
    // port-forward 回收：QProcess 是子对象随析构杀掉；远程 executor 删除即
    // cancel+join（CommandExecutor 析构语义）。
    for (PortForwardEntry &entry : m_forwards) {
        if (entry.executor)
            delete entry.executor;
    }
    m_forwards.clear();
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

// ---------------------------------------------------------------------------
// 后端与命令拼装
// ---------------------------------------------------------------------------

void KubeManager::setRemoteExecutor(CommandExecutor *executor)
{
    // 工作线程经 executorSnapshot() 并发读，写入必须持锁。
    QMutexLocker locker(&m_executorMutex);
    m_executor = executor;
}

bool KubeManager::isRemote() const
{
    QMutexLocker locker(&m_executorMutex);
    return m_executor != nullptr;
}

void KubeManager::setRemoteUser(const QString &username)
{
    m_remoteUser = username;
}

void KubeManager::setExecutorFactory(const std::function<CommandExecutor *()> &factory)
{
    QMutexLocker locker(&m_executorMutex);
    m_executorFactory = factory;
}

CommandExecutor *KubeManager::executorSnapshot() const
{
    QMutexLocker locker(&m_executorMutex);
    return m_executor;
}

CommandExecutor *KubeManager::acquireStreamExecutor(bool *ownedOut)
{
    QMutexLocker locker(&m_executorMutex);
    if (m_executorFactory) {
        if (ownedOut)
            *ownedOut = true;
        return m_executorFactory();
    }
    if (ownedOut)
        *ownedOut = false;
    return m_executor;
}

void KubeManager::setKubeconfigPath(const QString &path)
{
    m_kubeconfigPath = path;
}

void KubeManager::setCurrentContext(const QString &name)
{
    m_context = name;
}

void KubeManager::setNamespace(const QString &ns)
{
    if (ns.isEmpty() || ns == m_namespace)
        return;
    m_namespace = ns;
    emit namespaceChanged(m_namespace);
}

QString KubeManager::shellQuote(const QString &arg)
{
    QString escaped = arg;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString KubeManager::buildBaseFlags() const
{
    QString flags;
    if (!m_kubeconfigPath.isEmpty())
        flags += QStringLiteral(" --kubeconfig %1").arg(shellQuote(m_kubeconfigPath));
    if (!m_context.isEmpty())
        flags += QStringLiteral(" --context %1").arg(shellQuote(m_context));
    return flags;
}

QString KubeManager::buildGetCommand(const QString &apiPlural, bool namespaced) const
{
    QString cmd = QStringLiteral("kubectl get %1 -o json").arg(apiPlural) + buildBaseFlags();
    if (namespaced)
        cmd += QStringLiteral(" -n %1").arg(shellQuote(m_namespace));
    return cmd;
}

// 单对象命令的命名空间：ref 自带（行模型从对象 metadata 取到），空则回退当前。
static QString nsFlagFor(const QString &currentNs, const KubeObjectRef &ref)
{
    const QString ns = ref.namespace_.isEmpty() ? currentNs : ref.namespace_;
    if (ns.isEmpty())
        return QString();
    return QStringLiteral(" -n %1").arg(KubeManager::shellQuote(ns));
}

// ---------------------------------------------------------------------------
// 线程助手（照搬 DockerManager）
// ---------------------------------------------------------------------------

void KubeManager::schedule(std::function<void()> job)
{
    // 修剪已完成的 future，防容器无界增长。
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

QString KubeManager::runBlocking(CommandExecutor *executor, const QString &cmdLine,
                                 int timeoutMs, QString *errorOut)
{
    // executor 为调用方在任务开始时取的一次快照 —— 同一次任务全程用同一指针，
    // 不重读 m_executor，避免会话断开瞬间退回本地分支执行远程命令。
    if (executor) {
        // 注意：kubectl 一律不用 sudo 包装（与 docker 不同）——kubeconfig 与
        // 凭据都在登录用户的 $HOME 下，sudo 后读到的是 root 的 kubeconfig，
        // 且堡垒机未必装 sudo。这是与 DockerManager 的语义差异点。
        ExecResult res = executor->exec(cmdLine, false, timeoutMs);
        if (!res.ok() && errorOut) {
            *errorOut = res.errorMessage.isEmpty()
                ? (res.timedOut ? QStringLiteral("命令执行超时") : res.stderrText)
                : res.errorMessage;
        } else if (res.exitCode != 0 && errorOut && !res.stderrText.isEmpty()) {
            // 传输正常但命令本身失败（如 kubectl 报错/找不到）——与本地分支
            // 的 exitCode!=0 处理对齐，否则远端失败会被误判为成功。
            *errorOut = res.stderrText.trimmed();
        }
        QString out = res.stdoutText;
        if (!res.stderrText.isEmpty())
            out += (out.isEmpty() ? QString() : QStringLiteral("\n")) + res.stderrText;
        return out;
    }

    // Local: run through the default shell so pipelines/quoting work the
    // same way as the remote exec channel.
    QProcess proc;
    proc.setProcessEnvironment(localProcessEnvironment());
#ifdef Q_OS_WIN
    proc.start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmdLine});
#else
    proc.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmdLine});
#endif
    if (!proc.waitForStarted(5000)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法启动本地进程: %1").arg(cmdLine);
        return QString();
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        if (errorOut)
            *errorOut = QStringLiteral("命令执行超时: %1").arg(cmdLine);
        return QString();
    }
    const QString stdoutText = QString::fromUtf8(proc.readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(proc.readAllStandardError());
    if (proc.exitCode() != 0 && errorOut && !stderrText.isEmpty())
        *errorOut = stderrText.trimmed();
    QString out = stdoutText;
    if (!stderrText.isEmpty())
        out += (out.isEmpty() ? QString() : QStringLiteral("\n")) + stderrText;
    return out;
}

// ---------------------------------------------------------------------------
// 可用性探测 / 上下文 / 命名空间
// ---------------------------------------------------------------------------

void KubeManager::checkAvailability()
{
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    const QString kubeconfigFlag = m_kubeconfigPath.isEmpty()
        ? QString()
        : QStringLiteral(" --kubeconfig %1").arg(shellQuote(m_kubeconfigPath));
    schedule([this, executor, kubeconfigFlag]() {
        QString error;
        const QString out = runBlocking(
            executor,
            QStringLiteral("kubectl version --client -o json 2>/dev/null") + kubeconfigFlag,
            kDefaultTimeoutMs, &error);
        // 从 clientVersion.gitVersion 提取版本号；失败回退原文首行。
        QString versionText;
        const int jsonStart = out.indexOf(QLatin1Char('{'));
        if (jsonStart >= 0) {
            const QJsonDocument doc = QJsonDocument::fromJson(out.mid(jsonStart).toUtf8());
            versionText = doc.object()
                              .value(QStringLiteral("clientVersion")).toObject()
                              .value(QStringLiteral("gitVersion")).toString();
        }
        if (versionText.isEmpty())
            versionText = out.trimmed().section(QLatin1Char('\n'), 0, 0);
        const bool available = !versionText.isEmpty();
        emit availabilityReady(available,
                               available ? versionText
                                         : (error.isEmpty() ? QStringLiteral("kubectl 不可用")
                                                            : error.trimmed()));
    });
}

void KubeManager::refreshContexts()
{
    CommandExecutor *executor = executorSnapshot();
    const QString kubeconfigFlag = m_kubeconfigPath.isEmpty()
        ? QString()
        : QStringLiteral(" --kubeconfig %1").arg(shellQuote(m_kubeconfigPath));
    schedule([this, executor, kubeconfigFlag]() {
        QString error;
        // 注意：`kubectl config get-contexts` 不支持 -o json（会报错并退回
        // 表格输出）；`config view -o json` 输出完整 kubeconfig，其 contexts /
        // current-context 字段结构与 parseContextsJson 的期望一致。
        const QString out = runBlocking(
            executor,
            QStringLiteral("kubectl config view -o json 2>/dev/null") + kubeconfigFlag,
            kDefaultTimeoutMs, &error);
        const QList<KubeContextInfo> contexts =
            KubeResourceParser::parseContextsJson(out.toUtf8());
        // 状态写回走 manager 线程（worker 直接写 m_context 会与 UI 读竞争）。
        QMetaObject::invokeMethod(this, [this, contexts]() {
            // 未显式选过上下文时，跟随 kubeconfig 的 current-context。
            if (m_context.isEmpty()) {
                for (const KubeContextInfo &info : contexts) {
                    if (info.isCurrent) {
                        m_context = info.name;
                        break;
                    }
                }
            }
            emit contextsUpdated(contexts);
        }, Qt::QueuedConnection);
    });
}

void KubeManager::switchContext(const QString &name)
{
    if (name.isEmpty() || name == m_context)
        return;
    CommandExecutor *executor = executorSnapshot();
    const QString kubeconfigFlag = m_kubeconfigPath.isEmpty()
        ? QString()
        : QStringLiteral(" --kubeconfig %1").arg(shellQuote(m_kubeconfigPath));
    schedule([this, executor, name, kubeconfigFlag]() {
        QString error;
        runBlocking(executor,
                    QStringLiteral("kubectl config use-context %1 2>/dev/null")
                        .arg(shellQuote(name)) + kubeconfigFlag,
                    kDefaultTimeoutMs, &error);
        // 状态切换 + 连带刷新全部回到 manager 线程，保证顺序。
        QMetaObject::invokeMethod(this, [this, name, error]() {
            if (!error.isEmpty()) {
                emit errorOccurred(QStringLiteral("切换上下文失败: %1").arg(error.trimmed()));
                return;
            }
            m_context = name;
            emit contextChanged(name);
            refreshNamespaces();
            refreshAll();
        }, Qt::QueuedConnection);
    });
}

void KubeManager::refreshNamespaces()
{
    CommandExecutor *executor = executorSnapshot();
    const QString cmd = QStringLiteral("kubectl get namespaces -o json 2>/dev/null")
                        + buildBaseFlags();
    schedule([this, executor, cmd]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kDefaultTimeoutMs, &error);
        emit namespacesUpdated(KubeResourceParser::parseNamespacesJson(out.toUtf8()));
    });
}

// ---------------------------------------------------------------------------
// 资源拉取（refreshAll 并发受限队列）
// ---------------------------------------------------------------------------

void KubeManager::refreshAll()
{
    // 仅 manager 线程调用。递增代际作废旧周期的在飞结果，然后重建队列。
    ++m_refreshGeneration;
    m_pendingKinds.clear();
    const QList<KubeResourceKind> kinds = KubeResourceParser::allKinds();
    for (const KubeResourceKind &kind : kinds)
        m_pendingKinds.append(kind.apiPlural);
    pumpRefreshQueue();
}

void KubeManager::pumpRefreshQueue()
{
    // 仅 manager 线程调用（worker 完成时 Queued 回调进来）。
    while (m_refreshInFlight < kMaxConcurrentRefreshes && !m_pendingKinds.isEmpty()) {
        const QString apiPlural = m_pendingKinds.takeFirst();
        ++m_refreshInFlight;
        startRefreshWorker(apiPlural, m_refreshGeneration);
    }
    if (m_pendingKinds.isEmpty() && m_refreshInFlight == 0)
        emit refreshFinished();
}

void KubeManager::startRefreshWorker(const QString &apiPlural, quint64 generation)
{
    const KubeResourceKind *kind = KubeResourceParser::findKind(apiPlural);
    if (!kind) { // 注册表外的名字不应出现；防御性跳过并保持队列推进。
        QMetaObject::invokeMethod(this, [this]() {
            --m_refreshInFlight;
            pumpRefreshQueue();
        }, Qt::QueuedConnection);
        return;
    }
    const QString group = kind->group;
    const QString cmd = buildGetCommand(apiPlural, kind->namespaced);
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, cmd, group, apiPlural, generation]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kDefaultTimeoutMs, &error);
        // RBAC 无权限 / CRD 不存在的类别静默显示空（很常见，不发 errorOccurred
        // 轰炸）；连接性问题由 availability 探测覆盖。
        const QList<KubeResourceRow> rows =
            KubeResourceParser::parseResourceListJson(apiPlural, out.toUtf8(),
                                                      QDateTime::currentDateTimeUtc());
        const bool stale = (generation != m_refreshGeneration);
        QMetaObject::invokeMethod(this, [this, group, apiPlural, rows, stale]() {
            if (!stale)
                emit resourcesUpdated(group, apiPlural, rows);
            --m_refreshInFlight;
            pumpRefreshQueue();
        }, Qt::QueuedConnection);
    });
}

void KubeManager::refreshKind(const QString &apiPlural)
{
    const KubeResourceKind *kind = KubeResourceParser::findKind(apiPlural);
    if (!kind)
        return;
    const QString group = kind->group;
    const QString cmd = buildGetCommand(apiPlural, kind->namespaced);
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, cmd, group, apiPlural]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kDefaultTimeoutMs, &error);
        const QList<KubeResourceRow> rows =
            KubeResourceParser::parseResourceListJson(apiPlural, out.toUtf8(),
                                                      QDateTime::currentDateTimeUtc());
        emit resourcesUpdated(group, apiPlural, rows);
    });
}

// ---------------------------------------------------------------------------
// 对象操作
// ---------------------------------------------------------------------------

void KubeManager::fetchYaml(const KubeObjectRef &ref)
{
    const QString cmd = QStringLiteral("kubectl get %1 %2 -o yaml")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, cmd, ref]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kDefaultTimeoutMs, &error);
        if (!error.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(QStringLiteral("获取 YAML 失败: %1").arg(error.trimmed()));
            return;
        }
        emit yamlReady(ref, out);
    });
}

void KubeManager::applyYaml(const QString &yamlText)
{
    const QString baseFlags = buildBaseFlags();
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, yamlText, baseFlags]() {
        QString error;
        QString out;
#ifdef Q_OS_WIN
        if (!executor) {
            // Windows 本地无 base64：QProcess 直传 stdin。
            QStringList args{QStringLiteral("apply"), QStringLiteral("-f"),
                             QStringLiteral("-")};
            if (!m_kubeconfigPath.isEmpty())
                args << QStringLiteral("--kubeconfig") << m_kubeconfigPath;
            if (!m_context.isEmpty())
                args << QStringLiteral("--context") << m_context;
            out = runLocalWithStdin(args, yamlText.toUtf8(), kDefaultTimeoutMs, &error);
        } else
#endif
        {
            // base64 管道保持 YAML 字节级不变（与 DockerManager 传 compose 文件同级技巧）。
            const QByteArray b64 = yamlText.toUtf8().toBase64();
            out = runBlocking(executor,
                              QStringLiteral("echo %1 | base64 -d | kubectl apply -f -")
                                  .arg(shellQuote(QString::fromLatin1(b64)))
                                  + baseFlags,
                              kDefaultTimeoutMs, &error);
        }
        const bool success = error.isEmpty();
        emit applyFinished(success, success ? out.trimmed() : error.trimmed());
    });
}

QString KubeManager::runLocalWithStdin(const QStringList &args,
                                       const QByteArray &stdinData,
                                       int timeoutMs, QString *errorOut)
{
    QProcess proc;
    proc.setProcessEnvironment(localProcessEnvironment());
    proc.start(QStringLiteral("kubectl"), args);
    if (!proc.waitForStarted(5000)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法启动 kubectl 进程");
        return QString();
    }
    proc.write(stdinData);
    proc.closeWriteChannel();
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        if (errorOut)
            *errorOut = QStringLiteral("命令执行超时: kubectl %1").arg(args.join(QLatin1Char(' ')));
        return QString();
    }
    const QString stdoutText = QString::fromUtf8(proc.readAllStandardOutput());
    const QString stderrText = QString::fromUtf8(proc.readAllStandardError());
    if (proc.exitCode() != 0 && errorOut && !stderrText.isEmpty())
        *errorOut = stderrText.trimmed();
    QString out = stdoutText;
    if (!stderrText.isEmpty())
        out += (out.isEmpty() ? QString() : QStringLiteral("\n")) + stderrText;
    return out;
}

void KubeManager::runObjectOp(const QString &op, const QString &verbLine,
                              const KubeObjectRef &ref)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, op, verbLine, ref]() {
        QString error;
        const QString out = runBlocking(executor, verbLine, kDefaultTimeoutMs, &error);
        const bool success = error.isEmpty();
        emit operationFinished(success, op, ref,
                               success ? out.trimmed() : error.trimmed());
        if (success) {
            // 回查回到 manager 线程触发（schedule 的 m_futures 不是线程安全的）。
            const QString plural = ref.apiPlural;
            QMetaObject::invokeMethod(this, [this, plural]() {
                refreshKind(plural);
            }, Qt::QueuedConnection);
        }
    });
}

void KubeManager::deleteResource(const KubeObjectRef &ref)
{
    const QString cmd = QStringLiteral("kubectl delete %1 %2")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    runObjectOp(QStringLiteral("delete"), cmd, ref);
}

void KubeManager::scaleResource(const KubeObjectRef &ref, int replicas)
{
    const QString cmd = QStringLiteral("kubectl scale %1 %2 --replicas=%3")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                            .arg(replicas)
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    runObjectOp(QStringLiteral("scale"), cmd, ref);
}

void KubeManager::rolloutRestart(const KubeObjectRef &ref)
{
    const QString cmd = QStringLiteral("kubectl rollout restart %1 %2")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    runObjectOp(QStringLiteral("restart"), cmd, ref);
}

void KubeManager::rolloutStatus(const KubeObjectRef &ref)
{
    const QString cmd = QStringLiteral("kubectl rollout status %1 %2 --timeout=300s")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    const QString title = QStringLiteral("rollout status %1/%2")
                              .arg(ref.apiPlural, ref.name);
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, cmd, title]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kLongTimeoutMs, &error);
        emit textReady(title, error.isEmpty() ? out.trimmed()
                                              : QStringLiteral("失败: %1").arg(error.trimmed()));
    });
}

void KubeManager::fetchDescribe(const KubeObjectRef &ref)
{
    const QString cmd = QStringLiteral("kubectl describe %1 %2")
                            .arg(ref.apiPlural, shellQuote(ref.name))
                        + buildBaseFlags() + nsFlagFor(m_namespace, ref);
    const QString title = QStringLiteral("describe %1/%2").arg(ref.apiPlural, ref.name);
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, cmd, title]() {
        QString error;
        const QString out = runBlocking(executor, cmd, kDefaultTimeoutMs, &error);
        if (!error.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(QStringLiteral("describe 失败: %1").arg(error.trimmed()));
            return;
        }
        emit textReady(title, out);
    });
}

// ---------------------------------------------------------------------------
// Pod 日志流（对标 DockerManager::startComposeLogs/stopComposeLogs）
// ---------------------------------------------------------------------------

void KubeManager::startPodLogs(const KubeObjectRef &pod, const QString &container,
                               int tailLines, bool follow)
{
    if (m_logsRunning.load())
        return;
    QString cmd = QStringLiteral("kubectl logs %1 --tail=%2")
                      .arg(shellQuote(pod.name))
                      .arg(tailLines);
    if (follow)
        cmd += QStringLiteral(" -f");
    if (!container.isEmpty())
        cmd += QStringLiteral(" -c %1").arg(shellQuote(container));
    cmd += buildBaseFlags() + nsFlagFor(m_namespace, pod);
    m_logsRunning.store(true);

    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    if (executor) {
        // 远程：工厂给的独占 executor 优先（避免与转发/其他流互斥）。
        bool owned = false;
        CommandExecutor *streamExecutor = acquireStreamExecutor(&owned);
        if (!streamExecutor) {
            m_logsRunning.store(false);
            return;
        }
        m_logsRemote = true;
        // 先断开上一轮遗留连接，防重复 connect 输出成倍重复（DockerManager 同款）。
        disconnect(m_logsChunkConn);
        disconnect(m_logsFinishedConn);
        if (owned)
            m_logsExecutor = streamExecutor;
        m_logsChunkConn = connect(streamExecutor, &CommandExecutor::outputChunk, this,
                [this](const QByteArray &chunk) {
                    if (m_logsRunning.load())
                        emit logChunk(QString::fromUtf8(chunk));
                }, Qt::QueuedConnection);
        m_logsFinishedConn = connect(streamExecutor, &CommandExecutor::streamFinished, this,
                [this, owned](int exitCode, const QString &, const QString &) {
                    m_logsRunning.store(false);
                    disconnect(m_logsChunkConn);
                    disconnect(m_logsFinishedConn);
                    if (owned && m_logsExecutor) {
                        m_logsExecutor->deleteLater();
                        m_logsExecutor = nullptr;
                    }
                    emit logsFinished(exitCode);
                }, Qt::QueuedConnection);
        if (!streamExecutor->execStream(cmd)) {
            m_logsRunning.store(false);
            disconnect(m_logsChunkConn);
            disconnect(m_logsFinishedConn);
            if (owned && m_logsExecutor) {
                m_logsExecutor->deleteLater();
                m_logsExecutor = nullptr;
            }
            emit errorOccurred(QStringLiteral("已有命令流在运行，无法启动日志查看"));
            return;
        }
        emit logStarted(QStringLiteral("logs %1/%2").arg(pod.namespace_, pod.name));
        return;
    }

    // 本地：长存 QProcess，chunk 到达即转发。
    m_logsRemote = false;
    m_logsProcess = new QProcess(this);
    m_logsProcess->setProcessEnvironment(localProcessEnvironment());
    connect(m_logsProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        emit logChunk(QString::fromUtf8(m_logsProcess->readAllStandardOutput()));
    });
    connect(m_logsProcess, &QProcess::readyReadStandardError, this, [this]() {
        emit logChunk(QString::fromUtf8(m_logsProcess->readAllStandardError()));
    });
    connect(m_logsProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                m_logsRunning.store(false);
                m_logsProcess->deleteLater();
                m_logsProcess = nullptr;
                emit logsFinished(exitCode);
            });
#ifdef Q_OS_WIN
    m_logsProcess->start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmd});
#else
    m_logsProcess->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
    emit logStarted(QStringLiteral("logs %1/%2").arg(pod.namespace_, pod.name));
}

void KubeManager::stopPodLogs()
{
    if (!m_logsRunning.load())
        return;
    m_logsRunning.store(false);
    // 按流的启动模式回收，不重快照 m_executor —— 否则流式日志进行中切换
    // 后端（远程→本机）会漏杀旧流。
    if (m_logsRemote) {
        // 同步断开本轮日志连接，与 startPodLogs 的 disconnect 配对。
        disconnect(m_logsChunkConn);
        disconnect(m_logsFinishedConn);
        // 独占 executor 的回收在 streamFinished 里做（deleteLater）；
        // 共享 executor 只取消流，生命周期归会话。
        CommandExecutor *target = m_logsExecutor ? m_logsExecutor : executorSnapshot();
        if (target)
            target->cancel();
        return;
    }
    if (m_logsProcess) {
        m_logsProcess->kill();
        // finished() 处理器删除进程对象。
    }
}

// ---------------------------------------------------------------------------
// port-forward（受管长任务）
// ---------------------------------------------------------------------------

int KubeManager::startPortForward(const KubeObjectRef &target, quint16 localPort,
                                  quint16 remotePort)
{
    // 仅 UI 线程调用（m_forwards 无线程保护）。
    if (localPort == 0 || remotePort == 0)
        return -1;
    const QString resource = QStringLiteral("%1/%2").arg(target.apiPlural, target.name);
    const QString cmd = QStringLiteral("kubectl port-forward %1 %2:%3")
                            .arg(shellQuote(resource))
                            .arg(localPort)
                            .arg(remotePort)
                        + buildBaseFlags() + nsFlagFor(m_namespace, target);

    PortForwardEntry entry;
    entry.id = m_nextForwardId++;
    entry.info.id = entry.id;
    entry.info.targetText = resource;
    entry.info.namespace_ = target.namespace_.isEmpty() ? m_namespace : target.namespace_;
    entry.info.localPort = localPort;
    entry.info.remotePort = remotePort;

    CommandExecutor *executor = executorSnapshot();
    if (executor) {
        bool owned = false;
        CommandExecutor *streamExecutor = acquireStreamExecutor(&owned);
        if (!streamExecutor)
            return -1;
        if (!owned && streamExecutor->isStreaming()) {
            // 共享 executor 单流约束：已有日志流/其他流在跑。
            emit errorOccurred(QStringLiteral("远程模式已有命令流在运行，无法新建端口转发"));
            return -1;
        }
        entry.executor = streamExecutor;
        const int id = entry.id;
        connect(streamExecutor, &CommandExecutor::streamFinished, this,
                [this, id, streamExecutor, owned](int, const QString &, const QString &) {
                    for (int i = 0; i < m_forwards.size(); ++i) {
                        if (m_forwards.at(i).id == id) {
                            m_forwards.removeAt(i);
                            break;
                        }
                    }
                    if (owned)
                        streamExecutor->deleteLater();
                    emit portForwardsChanged();
                }, Qt::QueuedConnection);
        if (!streamExecutor->execStream(cmd)) {
            disconnect(streamExecutor, nullptr, this, nullptr);
            if (owned)
                streamExecutor->deleteLater();
            emit errorOccurred(QStringLiteral("端口转发启动失败：已有命令流在运行"));
            return -1;
        }
    } else {
        auto *proc = new QProcess(this);
        proc->setProcessEnvironment(localProcessEnvironment());
        entry.process = proc;
        const int id = entry.id;
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, id](int, QProcess::ExitStatus) {
                    for (int i = 0; i < m_forwards.size(); ++i) {
                        if (m_forwards.at(i).id == id) {
                            QProcess *p = m_forwards.at(i).process;
                            m_forwards.removeAt(i);
                            if (p)
                                p->deleteLater();
                            break;
                        }
                    }
                    emit portForwardsChanged();
                });
#ifdef Q_OS_WIN
        proc->start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmd});
#else
        proc->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmd});
#endif
    }

    m_forwards.append(entry);
    emit portForwardsChanged();
    return entry.id;
}

void KubeManager::stopPortForward(int forwardId)
{
    for (int i = 0; i < m_forwards.size(); ++i) {
        if (m_forwards.at(i).id != forwardId)
            continue;
        if (m_forwards.at(i).process) {
            m_forwards.at(i).process->kill(); // finished() 处理器移除条目
        } else if (m_forwards.at(i).executor) {
            m_forwards.at(i).executor->cancel(); // streamFinished 处理器移除条目
        }
        return;
    }
}

QList<KubePortForwardInfo> KubeManager::portForwards() const
{
    QList<KubePortForwardInfo> result;
    result.reserve(m_forwards.size());
    for (const PortForwardEntry &entry : m_forwards)
        result.append(entry.info);
    return result;
}

} // namespace cubeshell
