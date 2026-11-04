#pragma once

// NginxManager.h — nginx 站点管理（本地 / 远程双后端）。
//
// 见 docs/Nginx可视化管理实现方案.md §4。架构逐行对齐 docker/DockerManager：
//   - 本地模式: QProcess 运行本机 nginx（开发机 / 本机部署）
//   - 远程模式: cubeshell::CommandExecutor 在已连接的 SSH 会话上执行（生产主场景；
//     非 root 用户 sudo -S 包装，因为改 /etc/nginx 与 reload 都要 root）
//
// 线程模型（继承 DockerManager 纪律）：
//   - 一切操作异步：schedule() 投全局 QThreadPool，m_futures 析构 join；
//   - m_executor 读写持 m_executorMutex，任务开始经 executorSnapshot() 取一次快照；
//   - 所有信号可能发自工作线程 —— 消费端一律 Qt::QueuedConnection。

#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

#include "nginx/NginxConfig.h"

class QProcess;

namespace cubeshell {

class CommandExecutor;

class NginxManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 60 * 1000;
    static constexpr int kLongTimeoutMs = 600 * 1000;
    static constexpr const char *kDefaultConfDir = "/etc/nginx";
    static constexpr const char *kDefaultLogDir = "/var/log/nginx";
    static constexpr const char *kDefaultNginxBin = "nginx";
    // 列表命令输出的分隔标记（纯函数 parseListing 与脚本共用）。
    static constexpr const char *kFileMarkerPrefix = "@@@CUBESHELL_FILE:";
    static constexpr const char *kEnabledMarker = "@@@CUBESHELL_ENABLED@@@";
    // 可用性探测输出中标记解析到的 nginx 绝对路径。
    static constexpr const char *kBinMarker = "@@@NGINX_BIN@@@";

    explicit NginxManager(QObject *parent = nullptr);
    ~NginxManager() override;

    // --- 后端 ---
    // executor 须比本 manager 存活更久；nullptr 回到本地模式。
    void setRemoteExecutor(CommandExecutor *executor);
    bool isRemote() const;
    void setRemoteUser(const QString &username);
    QString remoteUser() const { return m_remoteUser; }

    // --- 配置 ---
    void setNginxBin(const QString &bin);
    QString nginxBin() const { return m_nginxBin; }
    void setConfDir(const QString &dir);
    QString confDir() const { return m_confDir; }
    void setLogDir(const QString &dir);
    QString logDir() const;

    // --- 探测 / 状态 ---
    void checkAvailability();     // nginx -v → availabilityReady
    void refreshServiceStatus();  // systemctl/pgrep → serviceStatusReady
    void startService();          // 启动 nginx → operationFinished
    void stopService();           // 停止 nginx → operationFinished
    void restartService();        // 重启 nginx → operationFinished

    // --- 站点 ---
    void refreshSites();          // 列举+读取+解析 → sitesUpdated
    void readFile(const QString &path);                              // → fileContentReady
    void writeFile(const QString &path, const QString &text);        // → fileSaved
    void createSite(const QString &fileName, const QString &text);   // 写 conf.d/<file>
    void deleteSite(const QString &path);                            // → operationFinished
    void moveFile(const QString &from, const QString &to);
    void enableSite(const QString &fileName);                        // sites-available → enabled
    void disableSite(const QString &fileName);                       // 删 sites-enabled 链接

    // --- 校验 / 重载 ---
    void testConfig();            // nginx -t → testResultReady
    void reload();                // nginx -s reload → reloadFinished

    // --- 日志 ---
    void fetchLog(const QString &kind, int tailLines = 300); // → logTextReady
    void startLogStream(const QString &kind);               // tail -f
    void stopLogStream();
    bool isStreamingLogs() const { return m_logsRunning.load(); }

    // --- 纯辅助（单测可覆盖） ---
    QString buildListSitesCommand() const;
    QString buildAvailabilityCommand() const;
    QString buildTestCommand() const;
    QString buildStartCommand() const;
    QString buildStopCommand() const;
    QString buildRestartCommand() const;
    QString buildReloadCommand() const;
    QString buildServiceStatusCommand() const;
    QString logFilePath(const QString &kind) const;
    static QString shellQuote(const QString &arg);
    // 解析列表命令输出 → 站点摘要（含 sites-available/enabled 去重与启用态判定）。
    static QList<nginx::SiteView> parseListing(const QString &output);
    static QString siteFileName(const QString &path);

signals:
    // 全部可能发自工作线程 —— connect 一律 Qt::QueuedConnection。
    void availabilityReady(bool available, const QString &versionText);
    void serviceStatusReady(bool running, const QString &detail);
    void sitesUpdated(const QList<cubeshell::nginx::SiteView> &sites);
    void fileContentReady(const QString &path, const QString &text);
    void fileSaved(const QString &path);
    void testResultReady(bool ok, const QString &output);
    void reloadFinished(bool ok, const QString &message);
    void operationFinished(bool ok, const QString &op, const QString &message);
    void logTextReady(const QString &title, const QString &text);
    void logStarted(const QString &title);
    void logChunk(const QString &text);
    void logsFinished(int exitCode);
    void errorOccurred(const QString &message);

private:
    QString runBlocking(CommandExecutor *executor, const QString &cmdLine, int timeoutMs,
                        QString *errorOut, bool useSudo = true);
    // 读类命令（列目录 / cat / tail）：先不提权，遇 Permission denied 或空输出
    // 再回退 sudo —— 避免「sudo 需要密码但登录无密码」时把可读操作一起拖垮。
    QString runRead(CommandExecutor *executor, const QString &cmdLine, int timeoutMs,
                    QString *errorOut);
    CommandExecutor *executorSnapshot() const;
    void schedule(std::function<void()> job);
    bool needsSudo() const;

    mutable QMutex m_executorMutex;
    CommandExecutor *m_executor = nullptr; // not owned; guarded by m_executorMutex
    QString m_remoteUser;
    QString m_nginxBin = QLatin1String(kDefaultNginxBin);
    QString m_confDir = QLatin1String(kDefaultConfDir);
    QString m_logDir;

    QProcess *m_logsProcess = nullptr;         // 本地 `tail -f`
    CommandExecutor *m_logsExecutor = nullptr; // 远程日志用的共享 executor（不 owned）
    bool m_logsRemote = false;
    QMetaObject::Connection m_logsChunkConn;
    QMetaObject::Connection m_logsFinishedConn;
    std::atomic<bool> m_logsRunning{false};

    QVector<QFuture<void>> m_futures; // joined in destructor
};

} // namespace cubeshell

Q_DECLARE_METATYPE(cubeshell::nginx::SiteView)
