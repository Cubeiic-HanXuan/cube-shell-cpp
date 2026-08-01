#pragma once

// DockerManager.h — docker CLI / docker compose management for local and
// remote (SSH) hosts.
//
// C++ port of core/docker/docker_compose_editor.py's non-UI logic:
//   - compose file location  对应Python: DockerComposeEditor.__init__ (dirs/file_name)
//   - compose commands       对应Python: DockerComposeEditor.execute_command
//   - compose log streaming  对应Python: DockerComposeEditor.start_logs/stop_logs
//   - compose file load/save 对应Python: DockerComposeEditor.load_config/save_config
//     (kept text-level: the YAML content travels as an opaque QString, no
//     YAML parser is involved — the visual per-service editing of the Python
//     version collapses into plain-text editing in DockerPanel)
//   - daemon.json config     对应Python: DockerComposeEditor.on_config_docker_clicked
// plus a container list built on `docker ps --format {{json .}}` parsed with
// QJsonDocument (JSON-lines output; works with every docker >= 17.x).
//
// Execution backends:
//   - Local:  QProcess running the docker CLI
//   - Remote: cubeshell::CommandExecutor over an existing SSH session
//     (sudo -S wrapping for non-root users, mirroring the Python behaviour)
//
// Threading model: every operation is asynchronous. Remote commands run on
// the global QThreadPool (QtConcurrent), local commands use QProcess signal
// delivery. All signals may therefore be emitted from a worker thread —
// consumers on the UI thread MUST connect with Qt::QueuedConnection.

#include <QFuture>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class QProcess;

namespace cubeshell {

class CommandExecutor;

// One row of `docker ps` output.
// 对应Python: docker ps 表格行（Python 版通过 compose ps 文本展示，这里结构化）
struct ContainerInfo {
    QString id;        // "ID"
    QString image;     // "Image"
    QString names;     // "Names"
    QString command;   // "Command"
    QString status;    // "Status"  e.g. "Up 3 hours"
    QString state;     // "State"   e.g. "running" / "exited"
    QString ports;     // "Ports"
    QString createdAt; // "CreatedAt"

    // Parse one JSON object produced by `docker ps --format "{{json .}}"`.
    static ContainerInfo fromPsJson(const QJsonObject &obj);
    bool isRunning() const;
};

// One container row of the grouped (compose project / standalone) list.
// 对应Python: cube-shell.py:132-144 DockerInfoThread._parse_container_line /
//             cube-shell.py:178-186 compose ps JSON 行的 dict 字段
struct DockerContainerRow {
    QString id;        // "ID"
    QString name;      // "Name"（compose）/ "Names"（独立容器）
    QString image;     // "Image"
    QString state;     // "State"
    QString createdAt; // "CreatedAt"
    QString ports;     // "Ports"
    QString project;   // "Project"（独立容器为空）
};

// One compose project group (or the "default" standalone group).
// 对应Python: cube-shell.py:151 DockerInfoThread.run 的 groups(defaultdict)
struct DockerComposeGroup {
    QString name;
    QList<DockerContainerRow> containers;
};

// One predefined compose service with its installed flag.
// 对应Python: cube-shell.py:269-273 CommonContainersThread._update_has_attribute
struct CommonServiceInfo {
    QString key;         // services 下的键名（service_key）
    QString description; // labels.description
    bool has = false;    // 是否已有同名（子串匹配）容器
};

// 容器操作后的状态回查结果 {id: (state, ports)}。
// 对应Python: cube-shell.py:299-320 DockerOperationThread 的 container_info dict
using DockerStatePortsMap = QHash<QString, QPair<QString, QString>>;

class DockerManager : public QObject {
    Q_OBJECT
public:
    // 对应Python: DockerComposeEditor.file_name = "docker-compose.yml"
    static constexpr const char *kComposeFileName = "docker-compose.yml";
    // 对应Python: exec_cli 默认 timeout（秒→毫秒）
    static constexpr int kDefaultTimeoutMs = 60 * 1000;

    // Local-mode manager (docker CLI via QProcess).
    explicit DockerManager(QObject *parent = nullptr);
    ~DockerManager() override;

    // Switch to remote mode: all docker commands go through the executor.
    // The executor must outlive this manager; nullptr returns to local mode.
    void setRemoteExecutor(CommandExecutor *executor);
    bool isRemote() const
    {
        QMutexLocker locker(&m_executorMutex);
        return m_executor != nullptr;
    }

    // Remote credentials context: non-root users get `sudo -S` wrapping.
    // 对应Python: DockerComposeEditor.execute_command 的 root/sudo 分支
    void setRemoteUser(const QString &username);
    QString remoteUser() const { return m_remoteUser; }

    // Compose working file. Default mirrors the Python rule:
    //   root  -> /home/app/docker-compose.yml
    //   other -> /home/<user>/app/docker-compose.yml
    // 对应Python: DockerComposeEditor.__init__ 的 dirs 计算
    QString composeFilePath() const;
    void setComposeDir(const QString &dir);
    QString composeDir() const; // 未设置时按 remoteUser 推导默认目录

    // --- containers ---

    // Run `docker ps -a --format "{{json .}}"`; emits containersUpdated().
    void refreshContainers();

    // Grouped container query: compose projects (via `docker compose ls` +
    // per-project `compose ps`) plus a trailing "default" group holding every
    // standalone container. Emits groupedContainersUpdated() from a worker
    // thread; remote executor required（无 executor 时直接发空列表）.
    // 对应Python: cube-shell.py:146-228 DockerInfoThread.run
    void refreshGroupedContainers();

    // Sequential `docker <op> <id>` over ids (op ∈ start/stop/restart/rm),
    // then a per-id state/ports re-query (skipped for rm). Emits
    // containerOperationFinished(). 对应Python: cube-shell.py:287-325
    // DockerOperationThread.run
    void containerOperation(const QString &op, const QStringList &ids);

    // Detect which predefined compose services already run as containers
    // (substring match on container names). Emits commonContainersReady().
    // 对应Python: cube-shell.py:240-267 CommonContainersThread.run
    void checkCommonContainers(const QString &composeFullPath);

    // Single-container lifecycle; each emits commandOutput()/errorOccurred().
    void startContainer(const QString &idOrName);
    void stopContainer(const QString &idOrName);
    void restartContainer(const QString &idOrName);
    void removeContainer(const QString &idOrName, bool force = false);

    // `docker logs --tail <n>`; output arrives via commandOutput().
    void fetchLogs(const QString &idOrName, int tailLines = 200);

    // `docker exec <id> sh -c "<cmd>"`.
    void execInContainer(const QString &idOrName, const QString &cmd);

    // --- docker compose ---

    // Run `docker compose -f <composeFilePath> <subcommand>` (e.g. "up -d",
    // "stop", "restart", "ps"). 对应Python: DockerComposeEditor.execute_command
    void composeCommand(const QString &subcommand);

    // Streaming `docker compose logs -f` (remote only uses the streaming
    // channel of CommandExecutor; local uses a long-lived QProcess).
    // 对应Python: DockerComposeEditor.start_logs / stop_logs
    void startComposeLogs();
    void stopComposeLogs();
    bool isStreamingLogs() const { return m_logsRunning.load(); }

    // Load / save the compose file as plain text (no YAML parsing).
    // Remote transfers go through base64 to stay byte-exact over exec
    // channels. 对应Python: DockerComposeEditor.load_config / save_config
    void loadComposeFile();
    void saveComposeFile(const QString &yamlText);

    // --- daemon.json ---

    // Validate that the text is a JSON object (daemon.json content).
    // 对应Python: DockerDaemonConfigDialog.validate_config
    static bool validateDaemonJson(const QString &text, QString *errorOut = nullptr);

    // Write /etc/docker/daemon.json (backup + tee + systemctl restart docker),
    // remote-only, needs sudo. 对应Python: on_config_docker_clicked 线程体
    void applyDaemonConfig(const QString &daemonJsonText);

    // Default daemon.json proposed by the UI.
    // 对应Python: DockerDaemonConfigDialog 默认 registry-mirrors 配置
    static QString defaultDaemonJson();

    // Parse JSON-lines `docker ps` output (pure helper, unit-testable).
    static QList<ContainerInfo> parsePsJsonLines(const QString &output);

    // --- pure helpers for the grouped query (unit-testable, no side effect) ---

    // Parse `docker ps --format '<ID>|||<Names>|||...'` pipe-separated lines;
    // lines with fewer than 6 fields are dropped.
    // 对应Python: cube-shell.py:132-144 DockerInfoThread._parse_container_line
    static QList<DockerContainerRow> parsePipeLines(const QString &output);

    // Parse the JSON array printed by `docker compose ls -a --format json`
    // into (Name, ConfigFiles) pairs, keeping order; entries with an empty
    // name or config are skipped. On parse failure a qWarning is logged and
    // an empty list returned (旧版 compose 回退).
    // 对应Python: cube-shell.py:157-166 + 192-194
    static QList<QPair<QString, QString>> parseComposeLsJson(const QString &output);

    // Parse `docker compose ps -a --format json` output — either one JSON
    // object per line or a whole JSON array（两种格式都容忍）。Rows without an
    // ID are dropped; a missing "Project" key falls back to projectName.
    // 对应Python: cube-shell.py:172-191
    static QList<DockerContainerRow> parseComposePsJson(const QString &output,
                                                        const QString &projectName);

signals:
    // Emitted from worker threads — connect with Qt::QueuedConnection.
    void containersUpdated(const QList<cubeshell::ContainerInfo> &containers);
    void commandOutput(const QString &text);
    void errorOccurred(const QString &message);
    void composeLoaded(const QString &yamlText);
    void composeSaved();

    // 对应Python: cube-shell.py:118 DockerInfoThread.data_ready(dict, list)
    // （Python 第二个参数 container_list 恒为空列表，此处省略）
    void groupedContainersUpdated(const QList<cubeshell::DockerComposeGroup> &groups);
    // 对应Python: cube-shell.py:279 DockerOperationThread.operation_finished
    void containerOperationFinished(bool success, const QString &op,
                                    const cubeshell::DockerStatePortsMap &idToStatePorts);
    // 对应Python: cube-shell.py:233 CommonContainersThread.data_ready(dict, bool)
    void commonContainersReady(const QList<cubeshell::CommonServiceInfo> &services,
                               bool hasDocker);

private:
    // Dispatch a one-shot docker command; emits commandOutput/errorOccurred.
    void runDocker(const QStringList &args, const char *what);
    // Run a raw shell command line, returning stdout (blocking; worker only).
    // executor 由调用方在任务开始时经 executorSnapshot() 取一次快照传入，
    // 保证同一次任务全程使用同一指针（避免中途被 setRemoteExecutor 清空后
    // 误退回本地 QProcess 分支）。
    QString runBlocking(CommandExecutor *executor, const QString &cmdLine,
                        int timeoutMs, QString *errorOut);
    // Thread-safe snapshot of m_executor (mutex-guarded read).
    CommandExecutor *executorSnapshot() const;
    // Wrap with sudo -S for non-root remote users.
    QString wrapSudo(const QString &cmdLine) const;
    void schedule(std::function<void()> job);

    // m_executor 被 UI 线程写（setRemoteExecutor）、QtConcurrent 工作线程读，
    // 所有访问必须持 m_executorMutex（QPointer 不能跨线程安全解引用）。
    mutable QMutex m_executorMutex;
    CommandExecutor *m_executor = nullptr; // not owned; guarded by m_executorMutex
    QString m_remoteUser;
    QString m_composeDir;                  // empty -> derived from remoteUser
    QProcess *m_logsProcess = nullptr;     // local `compose logs -f`
    // 远程日志流连接句柄：start 前 / stop 时断开，防止重复 connect 导致输出成倍重复。
    QMetaObject::Connection m_logsChunkConn;
    QMetaObject::Connection m_logsFinishedConn;
    std::atomic<bool> m_logsRunning{false};
    QVector<QFuture<void>> m_futures;      // joined in destructor
};

} // namespace cubeshell

// Metatype declarations so the value types can cross thread boundaries in
// queued signal emissions (registered in the DockerManager constructor).
Q_DECLARE_METATYPE(cubeshell::DockerContainerRow)
Q_DECLARE_METATYPE(cubeshell::DockerComposeGroup)
Q_DECLARE_METATYPE(cubeshell::CommonServiceInfo)
