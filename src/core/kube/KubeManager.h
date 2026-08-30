#pragma once

// KubeManager.h — kubectl CLI 驱动的 Kubernetes 集群管理（本地 / 远程双后端）。
//
// 对应 docs/Kubernetes功能实现方案.md §4。架构逐行对齐 core/docker/DockerManager：
//   - 本地模式: QProcess 运行本机 kubectl（K8s 主场景：本地 kubectl + kubeconfig
//     指向远地集群）
//   - 远程模式: cubeshell::CommandExecutor 在已连接的 SSH 会话上执行 kubectl
//     （堡垒机场景；非 root 用户同样 sudo -S 包装）
//
// 线程模型（继承 DockerManager 纪律）：
//   - 一切操作异步：schedule() 投全局 QThreadPool，m_futures 析构时 join；
//   - m_executor 读写持 m_executorMutex，任务开始经 executorSnapshot() 取一次
//     快照，同任务全程用同一指针；
//   - 所有信号可能发自工作线程 —— 消费端一律 Qt::QueuedConnection；
//   - refreshAll 并发受限（kMaxConcurrentRefreshes），避免远程模式下瞬间打满
//     SSH exec 通道 / 本地一次起十几个 QProcess。
//
// 交互式操作的分工（方案 §6）：
//   - Pod exec：不在此实现协议直连，由 UI 拼 `kubectl exec -it ...` 命令字符串
//     转发到终端标签页（terminalCommandRequested）；
//   - Pod 日志 / port-forward：走本类的流式通道（远程 = execStream，
//     本地 = 长存 QProcess）。
//
// 注意：execStream 单流约束 —— CommandExecutor 同一时刻只允许一条流。
// 远程模式下日志流与每个 port-forward 各自需要一条流，因此 port-forward /
// 日志并发时经 setExecutorFactory() 由主窗口提供"按需新建 executor"的工厂
// （同一 SshClient 上的新 CommandExecutor 实例）。

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

#include "kube/KubeResourceParser.h"

class QProcess;

namespace cubeshell {

class CommandExecutor;

// 一条活动的 port-forward 的描述（UI 列表用）。
struct KubePortForwardInfo {
    int id = 0;
    QString targetText;    // "pod/web-7d9" / "service/web"
    QString namespace_;
    quint16 localPort = 0;
    quint16 remotePort = 0;
};

class KubeManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 60 * 1000;
    static constexpr int kLongTimeoutMs = 600 * 1000;
    // refreshAll 同时在飞的 kind 数（方案 §7.4）。
    static constexpr int kMaxConcurrentRefreshes = 3;

    explicit KubeManager(QObject *parent = nullptr);
    ~KubeManager() override;

    // --- 后端（语义照搬 DockerManager） ---
    // executor 须比本 manager 存活更久；nullptr 回到本地模式。
    void setRemoteExecutor(CommandExecutor *executor);
    bool isRemote() const;
    void setRemoteUser(const QString &username);
    QString remoteUser() const { return m_remoteUser; }

    // 远程模式下日志流 / port-forward 需要独占流时的 executor 工厂
    // （MainWindow 绑定当前 SSH 会话的 SshClient）。不设置则回退共享 executor，
    // 此时远程日志与 port-forward 互斥（后启动的报"已有流在运行"）。
    void setExecutorFactory(const std::function<CommandExecutor *()> &factory);

    // --- kubeconfig / 上下文 / 命名空间 ---
    void setKubeconfigPath(const QString &path); // 空 = kubectl 默认解析
    QString kubeconfigPath() const { return m_kubeconfigPath; }
    QString currentContext() const { return m_context; }
    QString currentNamespace() const { return m_namespace; }
    void setCurrentContext(const QString &name);   // 仅置内存状态（持久化恢复用）
    void setNamespace(const QString &ns);          // 置状态并发 namespaceChanged

    void checkAvailability();   // `kubectl version --client` → availabilityReady
    void refreshContexts();     // `config get-contexts -o json` → contextsUpdated
    void switchContext(const QString &name); // use-context 后自动全量刷新
    void refreshNamespaces();   // `get namespaces -o json` → namespacesUpdated

    // --- 资源拉取 ---
    void refreshAll();                      // 全量（并发受限队列）
    void refreshKind(const QString &apiPlural); // 单类刷新（操作后回查）

    // --- 对象操作 ---
    void fetchYaml(const KubeObjectRef &ref);       // get -o yaml → yamlReady
    void applyYaml(const QString &yamlText);        // apply -f - → applyFinished
    void deleteResource(const KubeObjectRef &ref);  // delete → operationFinished
    void scaleResource(const KubeObjectRef &ref, int replicas);
    void rolloutRestart(const KubeObjectRef &ref);
    void rolloutStatus(const KubeObjectRef &ref);   // → textReady（阻塞等待结果）
    void fetchDescribe(const KubeObjectRef &ref);   // describe → textReady

    // --- Pod 日志流（对标 DockerManager::startComposeLogs） ---
    void startPodLogs(const KubeObjectRef &pod, const QString &container,
                      int tailLines = 200, bool follow = true);
    void stopPodLogs();
    bool isStreamingLogs() const { return m_logsRunning.load(); }

    // --- port-forward（受管长任务；返回 id，≤0 失败） ---
    int startPortForward(const KubeObjectRef &target, quint16 localPort,
                         quint16 remotePort);
    void stopPortForward(int forwardId);
    QList<KubePortForwardInfo> portForwards() const;

    // --- 命令拼装（纯函数，单测可覆盖） ---
    // 统一拼 --kubeconfig/--context；namespaced 资源另拼 -n。
    QString buildBaseFlags() const;
    QString buildGetCommand(const QString &apiPlural, bool namespaced) const;
    // 单引号 shell 转义。
    static QString shellQuote(const QString &arg);

signals:
    // 全部可能发自工作线程 —— connect 一律 Qt::QueuedConnection。
    void availabilityReady(bool available, const QString &versionText);
    void contextsUpdated(const QList<cubeshell::KubeContextInfo> &contexts);
    void contextChanged(const QString &name);
    void namespacesUpdated(const QStringList &namespaces);
    void namespaceChanged(const QString &ns);
    void resourcesUpdated(const QString &group, const QString &apiPlural,
                          const QList<cubeshell::KubeResourceRow> &rows);
    void refreshFinished(); // 一轮 refreshAll 全部 kind 完成
    void yamlReady(const cubeshell::KubeObjectRef &ref, const QString &yamlText);
    void applyFinished(bool success, const QString &message);
    void operationFinished(bool success, const QString &op,
                           const cubeshell::KubeObjectRef &ref,
                           const QString &message);
    // describe / rollout status 的一次性文本输出。
    void textReady(const QString &title, const QString &text);
    // Pod 日志流。
    void logStarted(const QString &title);
    void logChunk(const QString &text);
    void logsFinished(int exitCode);
    void portForwardsChanged();
    void errorOccurred(const QString &message);

private:
    // 照搬 DockerManager 的私有助手（命令名换成 kubectl）。
    // 注意没有 wrapSudo：kubectl 的凭据在登录用户 $HOME 的 kubeconfig 里，
    // sudo 后读到的是 root 的配置，且堡垒机未必装 sudo —— 与 docker 不同。
    QString runBlocking(CommandExecutor *executor, const QString &cmdLine,
                        int timeoutMs, QString *errorOut);
    CommandExecutor *executorSnapshot() const;
    void schedule(std::function<void()> job);

    // Windows 本地 apply 的 stdin 路径（无 base64 可用）：QProcess 直传 stdin。
    QString runLocalWithStdin(const QStringList &args, const QByteArray &stdinData,
                              int timeoutMs, QString *errorOut);
    // 通用 "kubectl <verb> <ref>" 操作 → operationFinished + 成功后 refreshKind。
    void runObjectOp(const QString &op, const QString &verbLine,
                     const KubeObjectRef &ref);

    // refreshAll 并发受限队列（manager 线程上泵；worker 完成时 Queued 回调）。
    void pumpRefreshQueue();
    void startRefreshWorker(const QString &apiPlural, quint64 generation);

    // 远程日志 / 转发需要的独占 executor：工厂优先，回退共享 executor。
    CommandExecutor *acquireStreamExecutor(bool *ownedOut);

    // m_executor 被 UI 线程写、QtConcurrent 工作线程读，访问必须持锁
    // （QPointer 不能跨线程安全解引用）。
    mutable QMutex m_executorMutex;
    CommandExecutor *m_executor = nullptr; // not owned; guarded by m_executorMutex
    std::function<CommandExecutor *()> m_executorFactory;

    QString m_remoteUser;
    QString m_kubeconfigPath;
    QString m_context;                    // 空 = kubeconfig 的 current-context
    QString m_namespace = QStringLiteral("default");

    // refreshAll 队列状态（仅 manager 线程访问；worker 经 Queued 回调）。
    QStringList m_pendingKinds;
    int m_refreshInFlight = 0;
    // 每次 refreshAll 递增，作废旧周期结果；worker 线程也会读，用 atomic。
    std::atomic<quint64> m_refreshGeneration{0};

    // Pod 日志流状态（对标 DockerManager 的 compose 日志三件套）。
    QProcess *m_logsProcess = nullptr;    // 本地 `kubectl logs -f`
    CommandExecutor *m_logsExecutor = nullptr; // 远程日志独占 executor（owned）
    bool m_logsRemote = false;            // 当前流模式（停止时按模式回收，不重快照）
    QMetaObject::Connection m_logsChunkConn;
    QMetaObject::Connection m_logsFinishedConn;
    std::atomic<bool> m_logsRunning{false};

    // port-forward 受管任务表。
    struct PortForwardEntry {
        int id = 0;
        KubePortForwardInfo info;
        QProcess *process = nullptr;          // 本地模式
        CommandExecutor *executor = nullptr;  // 远程模式（owned）
    };
    QList<PortForwardEntry> m_forwards;
    int m_nextForwardId = 1;

    QVector<QFuture<void>> m_futures;     // joined in destructor
};

} // namespace cubeshell

Q_DECLARE_METATYPE(cubeshell::KubePortForwardInfo)
