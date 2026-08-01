// DockerManager.cpp — see DockerManager.h for the port map.
// 对应Python: core/docker/docker_compose_editor.py（非 UI 逻辑部分）

#include "docker/DockerManager.h"

#include "docker/ComposeYaml.h"
#include "ssh/CommandExecutor.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QtConcurrent>

namespace cubeshell {

// ---------------------------------------------------------------------------
// ContainerInfo
// ---------------------------------------------------------------------------

ContainerInfo ContainerInfo::fromPsJson(const QJsonObject &obj)
{
    ContainerInfo info;
    info.id        = obj.value(QStringLiteral("ID")).toString();
    info.image     = obj.value(QStringLiteral("Image")).toString();
    info.names     = obj.value(QStringLiteral("Names")).toString();
    info.command   = obj.value(QStringLiteral("Command")).toString();
    info.status    = obj.value(QStringLiteral("Status")).toString();
    info.state     = obj.value(QStringLiteral("State")).toString();
    info.ports     = obj.value(QStringLiteral("Ports")).toString();
    info.createdAt = obj.value(QStringLiteral("CreatedAt")).toString();
    return info;
}

bool ContainerInfo::isRunning() const
{
    // docker >= 20 fills "State"; older versions only have "Status" ("Up …").
    if (!state.isEmpty())
        return state.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0;
    return status.startsWith(QStringLiteral("Up"), Qt::CaseInsensitive);
}

// ---------------------------------------------------------------------------
// DockerManager
// ---------------------------------------------------------------------------

DockerManager::DockerManager(QObject *parent)
    : QObject(parent)
{
    // Register the value types carried by the worker-thread signals so
    // queued cross-thread delivery works.
    qRegisterMetaType<QList<DockerComposeGroup>>();
    qRegisterMetaType<QList<CommonServiceInfo>>();
    qRegisterMetaType<DockerStatePortsMap>();
}

DockerManager::~DockerManager()
{
    stopComposeLogs();
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

void DockerManager::setRemoteExecutor(CommandExecutor *executor)
{
    // 工作线程经 executorSnapshot() 并发读，写入必须持锁。
    QMutexLocker locker(&m_executorMutex);
    m_executor = executor;
}

CommandExecutor *DockerManager::executorSnapshot() const
{
    QMutexLocker locker(&m_executorMutex);
    return m_executor;
}

void DockerManager::setRemoteUser(const QString &username)
{
    m_remoteUser = username;
}

QString DockerManager::composeFilePath() const
{
    return composeDir() + QLatin1String(kComposeFileName);
}

void DockerManager::setComposeDir(const QString &dir)
{
    m_composeDir = dir;
    if (!m_composeDir.isEmpty() && !m_composeDir.endsWith(QLatin1Char('/')))
        m_composeDir += QLatin1Char('/');
}

// 对应Python: DockerComposeEditor.__init__ 的 dirs 计算
// （root -> /home/app/，其余 -> /home/<user>/app/）
QString DockerManager::composeDir() const
{
    if (!m_composeDir.isEmpty())
        return m_composeDir;
    if (m_remoteUser.isEmpty() || m_remoteUser == QLatin1String("root"))
        return QStringLiteral("/home/app/");
    return QStringLiteral("/home/%1/app/").arg(m_remoteUser);
}

void DockerManager::schedule(std::function<void()> job)
{
    // Prune finished futures so the vector does not grow unboundedly.
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

QString DockerManager::wrapSudo(const QString &cmdLine) const
{
    // 对应Python: execute_command 中非 root 用户的 `sudo -S <cmd>` 分支；
    // CommandExecutor::sudoExec 负责写入密码，这里仅在本函数用于组装文本时用。
    if (m_remoteUser.isEmpty() || m_remoteUser == QLatin1String("root"))
        return cmdLine;
    return QStringLiteral("sudo -S ") + cmdLine;
}

QString DockerManager::runBlocking(CommandExecutor *executor, const QString &cmdLine,
                                   int timeoutMs, QString *errorOut)
{
    // executor 为调用方在任务开始时取的一次快照 —— 同一次任务全程用同一指针，
    // 不重读 m_executor，避免会话断开瞬间退回本地分支执行远程命令。
    if (executor) {
        const bool needsSudo = !m_remoteUser.isEmpty()
                               && m_remoteUser != QLatin1String("root");
        ExecResult res = needsSudo
            ? executor->sudoExec(cmdLine, false, timeoutMs)
            : executor->exec(cmdLine, false, timeoutMs);
        if (!res.ok() && errorOut) {
            *errorOut = res.errorMessage.isEmpty()
                ? (res.timedOut ? QStringLiteral("命令执行超时") : res.stderrText)
                : res.errorMessage;
        }
        QString out = res.stdoutText;
        if (!res.stderrText.isEmpty())
            out += (out.isEmpty() ? QString() : QStringLiteral("\n")) + res.stderrText;
        return out;
    }

    // Local: run through the default shell so pipelines/quoting work the
    // same way as the remote exec channel.
    QProcess proc;
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
// containers
// ---------------------------------------------------------------------------

QList<ContainerInfo> DockerManager::parsePsJsonLines(const QString &output)
{
    QList<ContainerInfo> result;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1Char('{')))
            continue; // skip sudo prompts / warnings interleaved in output
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        result.append(ContainerInfo::fromPsJson(doc.object()));
    }
    return result;
}

void DockerManager::refreshContainers()
{
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor]() {
        QString error;
        const QString out = runBlocking(
            executor,
            QStringLiteral("docker ps -a --format \"{{json .}}\""),
            kDefaultTimeoutMs, &error);
        if (!error.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(QStringLiteral("获取容器列表失败: %1").arg(error));
            return;
        }
        emit containersUpdated(parsePsJsonLines(out));
    });
}

// ---------------------------------------------------------------------------
// grouped containers / operations / common containers
// 对应Python: cube-shell.py 的 DockerInfoThread / DockerOperationThread /
//             CommonContainersThread
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py:122-126 FIELD_SEPARATOR / DOCKER_PS_FORMAT
static const QLatin1String kFieldSeparator("|||");
static const char *kDockerPsPipeFormat =
    "{{.ID}}|||{{.Names}}|||{{.Image}}|||{{.State}}|||{{.CreatedAt}}|||{{.Ports}}";

// 对应Python: cube-shell.py:132-144 DockerInfoThread._parse_container_line
QList<DockerContainerRow> DockerManager::parsePipeLines(const QString &output)
{
    QList<DockerContainerRow> result;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1); // 远端输出可能是 \r\n（Python splitlines 已吞掉）
        if (line.trimmed().isEmpty())
            continue;
        const QStringList parts = line.split(kFieldSeparator);
        if (parts.size() < 6)
            continue; // 对应Python: len(parts) >= 6 才有效
        DockerContainerRow row;
        row.id        = parts.at(0);
        row.name      = parts.at(1);
        row.image     = parts.at(2);
        row.state     = parts.at(3);
        row.createdAt = parts.at(4);
        row.ports     = parts.at(5);
        result.append(row);
    }
    return result;
}

// 对应Python: cube-shell.py:157-166 compose_projects = json.loads(ls.strip())
QList<QPair<QString, QString>> DockerManager::parseComposeLsJson(const QString &output)
{
    QList<QPair<QString, QString>> result;
    QString trimmed = output.trimmed();
    if (trimmed.isEmpty())
        return result;
    // 容忍 sudo 提示等前缀行：从第一个 '[' 开始解析
    const int start = trimmed.indexOf(QLatin1Char('['));
    if (start > 0)
        trimmed = trimmed.mid(start);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        // 对应Python: cube-shell.py:192-194 旧版 compose 回退，仅告警不中断
        qWarning() << "docker compose ls JSON parse failed, falling back to table parsing";
        return result;
    }
    const QJsonArray projects = doc.array();
    for (const QJsonValue &value : projects) {
        const QJsonObject project = value.toObject();
        const QString name   = project.value(QStringLiteral("Name")).toString();
        const QString config = project.value(QStringLiteral("ConfigFiles")).toString();
        if (name.isEmpty() || config.isEmpty())
            continue; // 对应Python: if not config or not project_name: continue
        result.append(qMakePair(name, config));
    }
    return result;
}

// 对应Python: cube-shell.py:172-191（每行一个 JSON 或 JSON 数组两种输出都容忍）
QList<DockerContainerRow> DockerManager::parseComposePsJson(const QString &output,
                                                            const QString &projectName)
{
    QList<DockerContainerRow> result;
    const QString trimmed = output.trimmed();
    if (trimmed.isEmpty())
        return result;

    QList<QJsonObject> objects;
    if (trimmed.startsWith(QLatin1Char('['))) {
        // 整体是 JSON 数组（compose v2 早期版本）
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isArray()) {
            const QJsonArray arr = doc.array();
            for (const QJsonValue &value : arr) {
                if (value.isObject())
                    objects.append(value.toObject());
            }
        }
    }
    if (objects.isEmpty()) {
        // 每行一个 JSON 对象（compose v2.21+），坏行/sudo 提示行直接跳过
        const QStringList lines = trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString cleaned = line.trimmed();
            if (!cleaned.startsWith(QLatin1Char('{')))
                continue;
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(cleaned.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                continue; // 对应Python: except json.JSONDecodeError: pass
            objects.append(doc.object());
        }
    }

    for (const QJsonObject &obj : objects) {
        DockerContainerRow row;
        row.id        = obj.value(QStringLiteral("ID")).toString();
        row.name      = obj.value(QStringLiteral("Name")).toString();
        row.image     = obj.value(QStringLiteral("Image")).toString();
        row.state     = obj.value(QStringLiteral("State")).toString();
        row.createdAt = obj.value(QStringLiteral("CreatedAt")).toString();
        row.ports     = obj.value(QStringLiteral("Ports")).toString();
        // 对应Python: container.get('Project', project_name)——键缺失才回退
        row.project = obj.contains(QStringLiteral("Project"))
            ? obj.value(QStringLiteral("Project")).toString()
            : projectName;
        if (!row.id.isEmpty())
            result.append(row); // 对应Python: if data['ID']:
    }
    return result;
}

// 对应Python: cube-shell.py:146-228 DockerInfoThread.run
void DockerManager::refreshGroupedContainers()
{
    // 快照读取，守卫与任务全程使用同一指针（线程安全）。
    CommandExecutor *executor = executorSnapshot();
    // 对应Python: if not self.ssh_conn or not self.ssh_conn.active
    if (!executor) {
        emit groupedContainersUpdated(QList<DockerComposeGroup>());
        return;
    }
    schedule([this, executor]() {
        QStringList groupOrder;
        QHash<QString, QList<DockerContainerRow>> byProject;
        QSet<QString> composeContainerIds;

        // 1. compose 项目列表（JSON 数组） 对应Python: cube-shell.py:157
        const QString ls = runBlocking(
            executor,
            QStringLiteral("docker compose ls -a --format json 2>/dev/null"),
            kDefaultTimeoutMs, nullptr);
        const QList<QPair<QString, QString>> projects = parseComposeLsJson(ls);

        // 2. 每个项目的容器列表 对应Python: cube-shell.py:169-189
        for (const QPair<QString, QString> &project : projects) {
            const QString psOut = runBlocking(
                executor,
                QStringLiteral("docker compose --file %1 ps -a --format json 2>/dev/null")
                    .arg(project.second),
                kDefaultTimeoutMs, nullptr);
            const QList<DockerContainerRow> rows = parseComposePsJson(psOut, project.first);
            for (const DockerContainerRow &row : rows) {
                composeContainerIds.insert(row.id);
                // 对应Python: groups[data['Project']].append(data)，保持插入顺序
                if (!byProject.contains(row.project))
                    groupOrder.append(row.project);
                byProject[row.project].append(row);
            }
        }

        // 3. 独立容器两步查询（运行中 + 已停止） 对应Python: cube-shell.py:196-218
        QList<DockerContainerRow> standalone;
        const auto collectStandalone = [&](const QString &cmdLine) {
            const QString out = runBlocking(executor, cmdLine, kDefaultTimeoutMs, nullptr);
            const QList<DockerContainerRow> rows = parsePipeLines(out);
            for (const DockerContainerRow &row : rows) {
                if (!row.id.isEmpty() && !composeContainerIds.contains(row.id))
                    standalone.append(row);
            }
        };
        collectStandalone(QStringLiteral("docker ps --format '%1' 2>/dev/null")
                              .arg(QLatin1String(kDockerPsPipeFormat)));
        collectStandalone(QStringLiteral(
            "docker ps -f 'status=exited' -f 'status=created' -f 'status=dead' "
            "--format '%1' 2>/dev/null").arg(QLatin1String(kDockerPsPipeFormat)));

        // 对应Python: cube-shell.py:221-222 groups['default'] = standalone_containers
        // （覆盖语义：若已有同名 compose 组则替换其内容，位置保持首次插入处）
        if (!standalone.isEmpty()) {
            const QString defaultName = QStringLiteral("default");
            if (!groupOrder.contains(defaultName))
                groupOrder.append(defaultName);
            byProject[defaultName] = standalone;
        }

        QList<DockerComposeGroup> groups;
        groups.reserve(groupOrder.size());
        for (const QString &name : groupOrder)
            groups.append(DockerComposeGroup{name, byProject.value(name)});
        emit groupedContainersUpdated(groups);
    });
}

// 对应Python: cube-shell.py:287-325 DockerOperationThread.run
void DockerManager::containerOperation(const QString &op, const QStringList &ids)
{
    // 快照读取，守卫与任务全程使用同一指针（线程安全）。
    CommandExecutor *executor = executorSnapshot();
    // 对应Python: if not self.ssh_conn or not self.ssh_conn.active
    if (!executor) {
        emit containerOperationFinished(false, op, DockerStatePortsMap());
        return;
    }
    schedule([this, executor, op, ids]() {
        // 对应Python: cube-shell.py:293-296 顺序执行 docker <op> <id>
        for (const QString &containerId : ids) {
            QString error;
            runBlocking(executor, QStringLiteral("docker %1 %2").arg(op, containerId),
                        kDefaultTimeoutMs, &error);
            if (!error.isEmpty()) {
                // 对应Python: except Exception → emit(False, op, {})
                emit containerOperationFinished(false, op, DockerStatePortsMap());
                return;
            }
        }

        DockerStatePortsMap containerInfo;
        if (op != QLatin1String("rm")) {
            // 对应Python: cube-shell.py:300-316 逐个回查最新 State/Ports
            for (const QString &containerId : ids) {
                QString error;
                const QString result = runBlocking(
                    executor,
                    QStringLiteral("docker ps -a --filter 'id=%1' "
                                   "--format '{{.State}}|||{{.Ports}}' 2>/dev/null")
                        .arg(containerId),
                    kDefaultTimeoutMs, &error);
                QString state;
                QString ports;
                if (error.isEmpty() && !result.trimmed().isEmpty()) {
                    const QStringList parts = result.trimmed().split(kFieldSeparator);
                    state = parts.value(0);
                    ports = parts.value(1);
                }
                containerInfo.insert(containerId, qMakePair(state, ports));
            }
        } else {
            // 删除操作无需回查（Python 标记 'removed'，此处按约定置空对，
            // 消费端以 op=="rm" 判定删除语义）
            for (const QString &containerId : ids)
                containerInfo.insert(containerId, qMakePair(QString(), QString()));
        }
        emit containerOperationFinished(true, op, containerInfo);
    });
}

// 对应Python: cube-shell.py:240-267 CommonContainersThread.run
void DockerManager::checkCommonContainers(const QString &composeFullPath)
{
    // 快照读取，守卫与任务全程使用同一指针（线程安全）。
    CommandExecutor *executor = executorSnapshot();
    // 对应Python: if not self.ssh_conn or not self.ssh_conn.active
    if (!executor) {
        emit commonContainersReady(QList<CommonServiceInfo>(), false);
        return;
    }
    schedule([this, executor, composeFullPath]() {
        // 1. docker 是否安装 对应Python: cube-shell.py:246-250
        const QString version = runBlocking(executor, QStringLiteral("docker --version"),
                                            kDefaultTimeoutMs, nullptr);
        if (version.trimmed().isEmpty()) {
            emit commonContainersReady(QList<CommonServiceInfo>(), false);
            return;
        }

        // 2. 现有容器名称列表 对应Python: cube-shell.py:254-257
        const QString namesOut = runBlocking(
            executor,
            QStringLiteral("docker ps -a --format '{{.Names}}' 2>/dev/null"),
            kDefaultTimeoutMs, nullptr);
        QStringList containerNames;
        const QStringList nameLines = namesOut.trimmed().split(QLatin1Char('\n'),
                                                               Qt::SkipEmptyParts);
        for (const QString &line : nameLines) {
            const QString name = line.trimmed();
            if (!name.isEmpty())
                containerNames.append(name);
        }

        // 3. 预定义服务清单 对应Python: cube-shell.py:259 util.get_compose_service
        const QList<ComposeYaml::ComposeServiceInfo> services =
            ComposeYaml::loadComposeServices(composeFullPath);

        // 4. 子串匹配 对应Python: cube-shell.py:269-273 any(service_key in name)
        QList<CommonServiceInfo> result;
        result.reserve(services.size());
        for (const ComposeYaml::ComposeServiceInfo &service : services) {
            CommonServiceInfo info;
            info.key = service.name;
            info.description = service.description;
            info.has = false;
            for (const QString &name : containerNames) {
                if (name.contains(service.name)) {
                    info.has = true;
                    break;
                }
            }
            result.append(info);
        }
        emit commonContainersReady(result, true);
    });
}

void DockerManager::runDocker(const QStringList &args, const char *what)
{
    const QString cmdLine = QStringLiteral("docker ") + args.join(QLatin1Char(' '));
    const QString label = QString::fromUtf8(what);
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor, cmdLine, label]() {
        QString error;
        const QString out = runBlocking(executor, cmdLine, kDefaultTimeoutMs, &error);
        if (!error.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(QStringLiteral("%1 失败: %2").arg(label, error));
            return;
        }
        emit commandOutput(out.trimmed().isEmpty()
                           ? QStringLiteral("[%1] 完成").arg(label)
                           : out);
    });
}

void DockerManager::startContainer(const QString &idOrName)
{
    runDocker({QStringLiteral("start"), idOrName}, "启动容器");
}

void DockerManager::stopContainer(const QString &idOrName)
{
    runDocker({QStringLiteral("stop"), idOrName}, "停止容器");
}

void DockerManager::restartContainer(const QString &idOrName)
{
    runDocker({QStringLiteral("restart"), idOrName}, "重启容器");
}

void DockerManager::removeContainer(const QString &idOrName, bool force)
{
    QStringList args{QStringLiteral("rm")};
    if (force)
        args << QStringLiteral("-f");
    args << idOrName;
    runDocker(args, "删除容器");
}

void DockerManager::fetchLogs(const QString &idOrName, int tailLines)
{
    runDocker({QStringLiteral("logs"),
               QStringLiteral("--tail"), QString::number(tailLines),
               idOrName}, "容器日志");
}

void DockerManager::execInContainer(const QString &idOrName, const QString &cmd)
{
    QString escaped = cmd;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    runDocker({QStringLiteral("exec"), idOrName,
               QStringLiteral("sh"), QStringLiteral("-c"),
               QStringLiteral("'%1'").arg(escaped)}, "容器内执行");
}

// ---------------------------------------------------------------------------
// docker compose
// ---------------------------------------------------------------------------

// 对应Python: DockerComposeEditor.execute_command
void DockerManager::composeCommand(const QString &subcommand)
{
    const QString cmdLine = QStringLiteral("docker compose -f %1 %2")
                                .arg(composeFilePath(), subcommand);
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor, cmdLine]() {
        QString error;
        const QString out = runBlocking(executor, cmdLine, kDefaultTimeoutMs, &error);
        if (!error.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(QStringLiteral("执行命令时出错: %1").arg(error));
            return;
        }
        emit commandOutput(out);
    });
}

// 对应Python: DockerComposeEditor.start_logs
void DockerManager::startComposeLogs()
{
    if (m_logsRunning.load())
        return;
    const QString cmdLine = QStringLiteral("docker compose -f %1 logs -f --tail 100")
                                .arg(composeFilePath());
    m_logsRunning.store(true);

    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    if (executor) {
        // 先断开上一轮遗留的连接（stop 只 cancel 不 disconnect 的历史路径），
        // 防止多次开关日志后 outputChunk 重复投递、输出成倍重复。
        disconnect(m_logsChunkConn);
        disconnect(m_logsFinishedConn);
        // Remote: stream through CommandExecutor's dedicated QThread; its
        // outputChunk is emitted from that thread (queued at the consumer).
        m_logsChunkConn = connect(executor, &CommandExecutor::outputChunk, this,
                [this](const QByteArray &chunk) {
                    if (m_logsRunning.load())
                        emit commandOutput(QString::fromUtf8(chunk));
                }, Qt::QueuedConnection);
        m_logsFinishedConn = connect(executor, &CommandExecutor::streamFinished, this,
                [this](int, const QString &, const QString &) {
                    m_logsRunning.store(false);
                }, Qt::QueuedConnection);
        const bool needsSudo = !m_remoteUser.isEmpty()
                               && m_remoteUser != QLatin1String("root");
        if (!executor->execStream(needsSudo ? wrapSudo(cmdLine) : cmdLine)) {
            m_logsRunning.store(false);
            disconnect(m_logsChunkConn);
            disconnect(m_logsFinishedConn);
            emit errorOccurred(QStringLiteral("已有命令流在运行，无法启动日志查看"));
        }
        return;
    }

    // Local: long-lived QProcess, chunks forwarded as they arrive.
    m_logsProcess = new QProcess(this);
    connect(m_logsProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        emit commandOutput(QString::fromUtf8(m_logsProcess->readAllStandardOutput()));
    });
    connect(m_logsProcess, &QProcess::readyReadStandardError, this, [this]() {
        emit commandOutput(QString::fromUtf8(m_logsProcess->readAllStandardError()));
    });
    connect(m_logsProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
                m_logsRunning.store(false);
                m_logsProcess->deleteLater();
                m_logsProcess = nullptr;
            });
#ifdef Q_OS_WIN
    m_logsProcess->start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmdLine});
#else
    m_logsProcess->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmdLine});
#endif
}

// 对应Python: DockerComposeEditor.stop_logs
void DockerManager::stopComposeLogs()
{
    if (!m_logsRunning.load())
        return;
    m_logsRunning.store(false);
    CommandExecutor *executor = executorSnapshot(); // 快照读取（线程安全）
    if (executor) {
        // 同步断开本轮日志连接，与 startComposeLogs 的 disconnect 配对。
        disconnect(m_logsChunkConn);
        disconnect(m_logsFinishedConn);
        executor->cancel();
        return;
    }
    if (m_logsProcess) {
        m_logsProcess->kill();
        // finished() handler deletes the process object.
    }
}

// ---------------------------------------------------------------------------
// compose file load / save（文本级，YAML 原样传递）
// ---------------------------------------------------------------------------

// 对应Python: DockerComposeEditor.load_config（文件不存在时创建默认内容）
void DockerManager::loadComposeFile()
{
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor]() {
        const QString path = composeFilePath();
        static const char *kDefaultCompose =
            "version: '3.8'\nservices: {}\nvolumes: {}\nnetworks: {}\n";
        QString error;
        if (executor) {
            const QString out = runBlocking(
                executor,
                QStringLiteral("cat %1 2>/dev/null").arg(path),
                kDefaultTimeoutMs, &error);
            if (!out.trimmed().isEmpty()) {
                emit composeLoaded(out);
                return;
            }
            // File missing: create directory + default file, like Python.
            runBlocking(executor, QStringLiteral("mkdir -p %1").arg(composeDir()),
                        kDefaultTimeoutMs, nullptr);
            const QByteArray b64 = QByteArray(kDefaultCompose).toBase64();
            runBlocking(executor, QStringLiteral("echo %1 | base64 -d > %2")
                            .arg(QString::fromLatin1(b64), path),
                        kDefaultTimeoutMs, nullptr);
            emit composeLoaded(QString::fromUtf8(kDefaultCompose));
            return;
        }
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            emit composeLoaded(QString::fromUtf8(f.readAll()));
            return;
        }
        QDir().mkpath(composeDir());
        QFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(kDefaultCompose);
            out.close();
        }
        emit composeLoaded(QString::fromUtf8(kDefaultCompose));
    });
}

// 对应Python: DockerComposeEditor.save_config
void DockerManager::saveComposeFile(const QString &yamlText)
{
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor, yamlText]() {
        const QString path = composeFilePath();
        if (executor) {
            // base64 round-trip keeps the YAML byte-exact through the shell.
            const QByteArray b64 = yamlText.toUtf8().toBase64();
            QString error;
            runBlocking(executor, QStringLiteral("mkdir -p %1").arg(composeDir()),
                        kDefaultTimeoutMs, nullptr);
            runBlocking(executor, QStringLiteral("echo %1 | base64 -d > %2")
                            .arg(QString::fromLatin1(b64), path),
                        kDefaultTimeoutMs, &error);
            if (!error.isEmpty()) {
                emit errorOccurred(QStringLiteral("保存配置时出错: %1").arg(error));
                return;
            }
            emit composeSaved();
            return;
        }
        QDir().mkpath(composeDir());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            emit errorOccurred(QStringLiteral("保存配置时出错: 无法写入 %1").arg(path));
            return;
        }
        f.write(yamlText.toUtf8());
        f.close();
        emit composeSaved();
    });
}

// ---------------------------------------------------------------------------
// daemon.json
// ---------------------------------------------------------------------------

// 对应Python: DockerDaemonConfigDialog.validate_config
bool DockerManager::validateDaemonJson(const QString &text, QString *errorOut)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (errorOut)
            *errorOut = err.errorString();
        return false;
    }
    if (!doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("daemon.json 必须是 JSON 对象");
        return false;
    }
    return true;
}

// 对应Python: DockerDaemonConfigDialog 的默认配置
QString DockerManager::defaultDaemonJson()
{
    return QStringLiteral(
        "{\n"
        "  \"registry-mirrors\": [\n"
        "    \"https://mirror.ccs.tencentyun.com\"\n"
        "  ]\n"
        "}");
}

// 对应Python: DockerComposeEditor.on_config_docker_clicked 的 apply_docker_config
void DockerManager::applyDaemonConfig(const QString &daemonJsonText)
{
    QString validationError;
    if (!validateDaemonJson(daemonJsonText, &validationError)) {
        emit errorOccurred(QStringLiteral("配置内容为空或格式错误: %1").arg(validationError));
        return;
    }
    CommandExecutor *executor = executorSnapshot(); // 任务开始取一次快照
    schedule([this, executor, daemonJsonText]() {
        QString error;

        emit commandOutput(QStringLiteral("[配置Docker] 创建Docker配置目录..."));
        runBlocking(executor, QStringLiteral("mkdir -p /etc/docker"), kDefaultTimeoutMs, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(QStringLiteral("[配置Docker] 创建配置目录失败: %1").arg(error));
            return;
        }

        emit commandOutput(QStringLiteral("[配置Docker] 备份现有配置..."));
        runBlocking(executor, QStringLiteral(
            "cp /etc/docker/daemon.json /etc/docker/daemon.json.bak 2>/dev/null || true"),
            kDefaultTimeoutMs, nullptr);

        emit commandOutput(QStringLiteral("[配置Docker] 写入新配置..."));
        const QByteArray b64 = daemonJsonText.toUtf8().toBase64();
        error.clear();
        runBlocking(executor,
                    QStringLiteral("echo %1 | base64 -d | tee /etc/docker/daemon.json >/dev/null")
                        .arg(QString::fromLatin1(b64)),
                    kDefaultTimeoutMs, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(QStringLiteral("[配置Docker] 写入配置失败: %1").arg(error));
            return;
        }

        emit commandOutput(QStringLiteral("[配置Docker] 重启Docker服务..."));
        error.clear();
        runBlocking(executor, QStringLiteral("systemctl restart docker"), kDefaultTimeoutMs, &error);
        if (!error.isEmpty()) {
            emit errorOccurred(QStringLiteral("[配置Docker] 重启Docker服务失败: %1").arg(error));
            return;
        }

        emit commandOutput(QStringLiteral("[配置Docker] Docker守护进程配置完成！"));
    });
}

} // namespace cubeshell
