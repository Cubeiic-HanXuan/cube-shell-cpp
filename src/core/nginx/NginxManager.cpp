// NginxManager.cpp — see NginxManager.h.
// 见 docs/Nginx可视化管理实现方案.md §4；线程纪律逐行对齐 docker/DockerManager.cpp。

#include "nginx/NginxManager.h"

#include "nginx/NginxConfigParser.h"
#include "ssh/CommandExecutor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QtConcurrent>

namespace cubeshell {

// NginxManager 属 nginx 模块，内部直接用其类型名。
using namespace nginx;

namespace {

// 本地后端子进程环境：macOS 以 GUI 方式启动时 PATH 常缺 /usr/local/bin、
// /opt/homebrew/bin 等 nginx 常见安装目录（同 KubeManager::localProcessEnvironment）。
QProcessEnvironment localProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_UNIX
    const QString pathKey = QStringLiteral("PATH");
    QStringList dirs = env.value(pathKey).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString home = QDir::homePath();
    const QStringList extra = {
        QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/local/sbin"),
        QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/opt/homebrew/sbin"),
        QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
        home + QStringLiteral("/.local/bin"),
    };
    for (const QString &dir : extra) {
        if (!dirs.contains(dir))
            dirs.append(dir);
    }
    env.insert(pathKey, dirs.join(QLatin1Char(':')));
#endif
    return env;
}

QString parentDir(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

} // namespace

NginxManager::NginxManager(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<cubeshell::nginx::SiteView>();
    qRegisterMetaType<QList<cubeshell::nginx::SiteView>>();
}

NginxManager::~NginxManager()
{
    stopLogStream();
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

// ---------------------------------------------------------------------------
// 后端 / 配置
// ---------------------------------------------------------------------------

void NginxManager::setRemoteExecutor(CommandExecutor *executor)
{
    // 工作线程经 executorSnapshot() 并发读，写入必须持锁。
    QMutexLocker locker(&m_executorMutex);
    m_executor = executor;
}

bool NginxManager::isRemote() const
{
    QMutexLocker locker(&m_executorMutex);
    return m_executor != nullptr;
}

void NginxManager::setRemoteUser(const QString &username)
{
    m_remoteUser = username;
}

void NginxManager::setNginxBin(const QString &bin)
{
    m_nginxBin = bin.trimmed().isEmpty() ? QLatin1String(kDefaultNginxBin) : bin.trimmed();
}

void NginxManager::setConfDir(const QString &dir)
{
    QString d = dir.trimmed();
    while (d.size() > 1 && d.endsWith(QLatin1Char('/')))
        d.chop(1);
    m_confDir = d.isEmpty() ? QLatin1String(kDefaultConfDir) : d;
}

void NginxManager::setLogDir(const QString &dir)
{
    m_logDir = dir.trimmed();
}

QString NginxManager::logDir() const
{
    return m_logDir.isEmpty() ? QLatin1String(kDefaultLogDir) : m_logDir;
}

bool NginxManager::needsSudo() const
{
    return !m_remoteUser.isEmpty() && m_remoteUser != QLatin1String("root");
}

// ---------------------------------------------------------------------------
// 线程纪律（照搬 DockerManager）
// ---------------------------------------------------------------------------

CommandExecutor *NginxManager::executorSnapshot() const
{
    QMutexLocker locker(&m_executorMutex);
    return m_executor;
}

void NginxManager::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

QString NginxManager::runBlocking(CommandExecutor *executor, const QString &cmdLine,
                                  int timeoutMs, QString *errorOut, bool useSudo)
{
    // executor 为任务开始时取的一次快照 —— 同任务全程用同一指针。
    if (executor) {
        // sudoExec 只做 `sudo -S ` 前缀：复合命令（shell for 循环、管道、
        // 重定向、if/elif）若直接交给 sudo，会把第一个词当程序名，且管道/重定向
        // 以登录用户身份执行（对 /etc/nginx 无写权限）。故非 root 时整体包一层
        // `sh -c '<script>'`，让远端 root shell 解析脚本。
        ExecResult res = (useSudo && needsSudo())
            ? executor->sudoExec(QStringLiteral("sh -c ") + shellQuote(cmdLine), false, timeoutMs)
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

    // 本地：走默认 shell，使管道/引号语义与远程 exec 通道一致。
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

QString NginxManager::runRead(CommandExecutor *executor, const QString &cmdLine,
                              int timeoutMs, QString *errorOut)
{
    if (!executor || !needsSudo())
        return runBlocking(executor, cmdLine, timeoutMs, errorOut, /*useSudo=*/false);
    QString err1;
    const QString out1 = runBlocking(executor, cmdLine, timeoutMs, &err1, /*useSudo=*/false);
    const bool permDenied = out1.contains(QLatin1String("Permission denied"))
                            || err1.contains(QLatin1String("Permission denied"));
    if (!out1.trimmed().isEmpty() && !permDenied) {
        if (errorOut)
            *errorOut = err1;
        return out1;
    }
    // 读不到（多为权限）→ 回退 sudo。
    return runBlocking(executor, cmdLine, timeoutMs, errorOut, /*useSudo=*/true);
}

// ---------------------------------------------------------------------------
// 纯辅助
// ---------------------------------------------------------------------------

QString NginxManager::shellQuote(const QString &arg)
{
    QString s = arg;
    s.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + s + QLatin1Char('\'');
}

QString NginxManager::siteFileName(const QString &path)
{
    return QFileInfo(path).fileName();
}

QString NginxManager::logFilePath(const QString &kind) const
{
    const QString name = (kind == QLatin1String("access")) ? QStringLiteral("access.log")
                                                           : QStringLiteral("error.log");
    return logDir() + QLatin1Char('/') + name;
}

QString NginxManager::buildListSitesCommand() const
{
    const QString d = shellQuote(m_confDir);
    QString cmd = QStringLiteral("D=%1; ").arg(d);
    cmd += QStringLiteral(
        "for f in \"$D/nginx.conf\" \"$D\"/conf.d/* \"$D\"/sites-enabled/* "
        "\"$D\"/sites-available/*; do "
        "[ -f \"$f\" ] || continue; "
        "echo \"%1$f@@@\"; "
        "cat \"$f\" 2>/dev/null; echo; "
        "done; ").arg(QLatin1String(kFileMarkerPrefix));
    cmd += QStringLiteral("echo \"%1\"; ").arg(QLatin1String(kEnabledMarker));
    cmd += QStringLiteral("ls -1 \"$D/sites-enabled\" 2>/dev/null; true");
    return cmd;
}

QString NginxManager::buildAvailabilityCommand() const
{
    // 在「用户指定路径 + PATH + 常见安装路径」中定位 nginx：非 root 的远程
    // 非交互 shell 的 PATH 通常不含 /usr/sbin，直接 `nginx -v` 会误判未安装。
    // 命中后输出版本行 + kBinMarker 标记的绝对路径。
    static const char *kCandidates =
        "nginx /usr/sbin/nginx /usr/local/sbin/nginx /usr/local/bin/nginx "
        "/usr/bin/nginx /opt/nginx/sbin/nginx /usr/local/nginx/sbin/nginx";
    QString cmd = QStringLiteral("for c in %1 %2; do ")
                      .arg(shellQuote(m_nginxBin), QLatin1String(kCandidates));
    cmd += QStringLiteral(
        "if command -v \"$c\" >/dev/null 2>&1; then p=$(command -v \"$c\"); "
        "\"$p\" -v 2>&1; printf '%1%s\\n' \"$p\"; break; fi; done")
               .arg(QLatin1String(kBinMarker));
    return cmd;
}

QString NginxManager::buildTestCommand() const
{
    QString cmd = shellQuote(m_nginxBin) + QStringLiteral(" -t");
    if (m_confDir != QLatin1String(kDefaultConfDir))
        cmd += QStringLiteral(" -c ") + shellQuote(m_confDir + QStringLiteral("/nginx.conf"))
               + QStringLiteral(" -p ") + shellQuote(m_confDir + QLatin1Char('/'));
    return cmd;
}

QString NginxManager::buildReloadCommand() const
{
    return shellQuote(m_nginxBin) + QStringLiteral(" -s reload");
}

QString NginxManager::buildServiceStatusCommand() const
{
    return QStringLiteral(
        "if systemctl is-active nginx >/dev/null 2>&1; then systemctl is-active nginx; "
        "elif pgrep -x nginx >/dev/null 2>&1; then echo active; "
        "else echo inactive; fi");
}

QString NginxManager::buildStartCommand() const
{
    return QStringLiteral("systemctl start nginx >/dev/null 2>&1 || %1")
        .arg(shellQuote(m_nginxBin));
}

QString NginxManager::buildStopCommand() const
{
    return QStringLiteral("systemctl stop nginx >/dev/null 2>&1 || %1 -s stop")
        .arg(shellQuote(m_nginxBin));
}

QString NginxManager::buildRestartCommand() const
{
    return QStringLiteral(
               "systemctl restart nginx >/dev/null 2>&1 || { %1 -s stop; sleep 1; %1; }")
        .arg(shellQuote(m_nginxBin));
}

QList<SiteView> NginxManager::parseListing(const QString &output)
{
    const QString filePrefix = QLatin1String(kFileMarkerPrefix);
    const QString enabledMarker = QLatin1String(kEnabledMarker);

    QList<QPair<QString, QString>> files; // path → content
    QStringList enabledNames;
    enum class Mode { None, File, Enabled } mode = Mode::None;
    QString curPath;
    QStringList curLines;

    auto flushFile = [&]() {
        if (!curPath.isEmpty()) {
            files.append({curPath, curLines.join(QLatin1Char('\n'))});
            curPath.clear();
            curLines.clear();
        }
    };

    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &ln : lines) {
        if (ln.startsWith(filePrefix) && ln.endsWith(QLatin1String("@@@"))) {
            flushFile();
            curPath = ln.mid(filePrefix.size(), ln.size() - filePrefix.size() - 3);
            mode = Mode::File;
            continue;
        }
        if (ln == enabledMarker) {
            flushFile();
            mode = Mode::Enabled;
            continue;
        }
        if (mode == Mode::File)
            curLines.append(ln);
        else if (mode == Mode::Enabled && !ln.trimmed().isEmpty())
            enabledNames.append(ln.trimmed());
    }
    flushFile();

    const QSet<QString> enabledSet(enabledNames.begin(), enabledNames.end());
    QSet<QString> availableNames;
    for (const auto &f : files) {
        if (f.first.contains(QLatin1String("/sites-available/")))
            availableNames.insert(QFileInfo(f.first).fileName());
    }

    QList<SiteView> out;
    for (const auto &f : files) {
        SiteView s = NginxConfigParser::parseSite(f.second, f.first);
        if (s.dirKind == QLatin1String("sites-enabled")) {
            // sites-enabled 里的条目通常是指向 sites-available 的符号链接：
            // 由 sites-available 条目代表它，避免重复显示。
            if (availableNames.contains(s.fileName))
                continue;
            s.enabled = true;
        } else if (s.dirKind == QLatin1String("sites-available")) {
            s.enabled = enabledSet.contains(s.fileName);
        } else if (s.dirKind == QLatin1String("conf.d")
                   || s.dirKind == QLatin1String("nginx.conf")) {
            s.enabled = true;
        }
        out.append(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 探测 / 状态
// ---------------------------------------------------------------------------

void NginxManager::checkAvailability()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        // 探测不提高权限（nginx -v 无需 root），避免 sudo 需要密码时误判未安装。
        const QString out = runBlocking(executor, buildAvailabilityCommand(),
                                        kDefaultTimeoutMs, &err, /*useSudo=*/false);
        const bool available = out.contains(QLatin1String("nginx version"))
                               || out.contains(QLatin1String("nginx/"));
        QString versionText;
        QString bin;
        for (const QString &ln : out.split(QLatin1Char('\n'))) {
            const QString t = ln.trimmed();
            if (t.startsWith(QLatin1String(kBinMarker)))
                bin = t.mid(QLatin1String(kBinMarker).size());
            else if (versionText.isEmpty() && t.contains(QLatin1String("nginx/")))
                versionText = t;
        }
        if (!bin.isEmpty())
            m_nginxBin = bin; // 后续 test/reload/启停用解析出的绝对路径
        if (versionText.isEmpty() && available)
            versionText = out.trimmed();
        // 未检测到时回传诊断信息（供 UI 提示），而非空串。
        emit availabilityReady(available, available
                                               ? versionText
                                               : (err.isEmpty() ? out.trimmed() : err));
    });
}

void NginxManager::refreshServiceStatus()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        const QString out = runBlocking(executor, buildServiceStatusCommand(),
                                        kDefaultTimeoutMs, &err, /*useSudo=*/false);
        const QString detail = out.trimmed();
        const bool running = detail.endsWith(QLatin1String("active"))
                             && !detail.endsWith(QLatin1String("inactive"));
        emit serviceStatusReady(running, detail.isEmpty() ? QStringLiteral("未知") : detail);
    });
}

// ---------------------------------------------------------------------------
// 站点：列举 / 读写 / 启停 / 删除
// ---------------------------------------------------------------------------

void NginxManager::refreshSites()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        const QString out = runRead(executor, buildListSitesCommand(), kLongTimeoutMs, &err);
        if (!err.isEmpty() && out.trimmed().isEmpty()) {
            emit errorOccurred(err);
            emit sitesUpdated({});
            return;
        }
        emit sitesUpdated(parseListing(out));
    });
}

void NginxManager::readFile(const QString &path)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, path]() {
        if (executor) {
            QString err;
            const QString out = runRead(executor, QStringLiteral("cat %1").arg(shellQuote(path)),
                                        kDefaultTimeoutMs, &err);
            if (!err.isEmpty()) {
                emit errorOccurred(QStringLiteral("读取 %1 失败: %2").arg(path, err));
                return;
            }
            emit fileContentReady(path, out);
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            emit errorOccurred(QStringLiteral("读取 %1 失败: %2").arg(path, f.errorString()));
            return;
        }
        emit fileContentReady(path, QString::fromUtf8(f.readAll()));
    });
}

void NginxManager::writeFile(const QString &path, const QString &text)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, path, text]() {
        if (executor) {
            // base64 round-trip：内容逐字节穿过 shell，不受引号/换行影响。
            const QByteArray b64 = text.toUtf8().toBase64();
            QString err;
            runBlocking(executor, QStringLiteral("mkdir -p %1").arg(shellQuote(parentDir(path))),
                        kDefaultTimeoutMs, nullptr);
            runBlocking(executor,
                        QStringLiteral("echo %1 | base64 -d > %2")
                            .arg(shellQuote(QString::fromLatin1(b64)), shellQuote(path)),
                        kDefaultTimeoutMs, &err);
            if (!err.isEmpty()) {
                emit errorOccurred(QStringLiteral("写入 %1 失败: %2").arg(path, err));
                return;
            }
            emit fileSaved(path);
            return;
        }
        QDir().mkpath(parentDir(path));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit errorOccurred(QStringLiteral("写入 %1 失败: %2").arg(path, f.errorString()));
            return;
        }
        f.write(text.toUtf8());
        f.close();
        emit fileSaved(path);
    });
}

void NginxManager::createSite(const QString &fileName, const QString &text)
{
    const QString path = m_confDir + QStringLiteral("/conf.d/") + fileName;
    writeFile(path, text);
}

void NginxManager::deleteSite(const QString &path)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, path]() {
        if (executor) {
            QString err;
            runBlocking(executor, QStringLiteral("rm -f %1").arg(shellQuote(path)),
                        kDefaultTimeoutMs, &err);
            emit operationFinished(err.isEmpty(), QStringLiteral("delete"),
                                   err.isEmpty() ? QString() : err);
            return;
        }
        const bool ok = QFile::remove(path);
        emit operationFinished(ok, QStringLiteral("delete"),
                               ok ? QString() : QStringLiteral("删除失败"));
    });
}

void NginxManager::moveFile(const QString &from, const QString &to)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, from, to]() {
        if (executor) {
            QString err;
            runBlocking(executor, QStringLiteral("mv -f %1 %2").arg(shellQuote(from), shellQuote(to)),
                        kDefaultTimeoutMs, &err);
            emit operationFinished(err.isEmpty(), QStringLiteral("move"),
                                   err.isEmpty() ? QString() : err);
            return;
        }
        const bool ok = QFile::rename(from, to);
        emit operationFinished(ok, QStringLiteral("move"),
                               ok ? QString() : QStringLiteral("重命名失败"));
    });
}

void NginxManager::enableSite(const QString &fileName)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, fileName]() {
        const QString avail = m_confDir + QStringLiteral("/sites-available/") + fileName;
        const QString link = m_confDir + QStringLiteral("/sites-enabled/") + fileName;
        const QString cmd = QStringLiteral("mkdir -p %1 && ln -sf %2 %3")
                                .arg(shellQuote(m_confDir + QStringLiteral("/sites-enabled")),
                                     shellQuote(avail), shellQuote(link));
        if (executor) {
            QString err;
            runBlocking(executor, cmd, kDefaultTimeoutMs, &err);
            emit operationFinished(err.isEmpty(), QStringLiteral("enable"),
                                   err.isEmpty() ? QString() : err);
            return;
        }
        QString err;
        runBlocking(nullptr, cmd, kDefaultTimeoutMs, &err);
        emit operationFinished(err.isEmpty(), QStringLiteral("enable"),
                               err.isEmpty() ? QString() : err);
    });
}

void NginxManager::disableSite(const QString &fileName)
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor, fileName]() {
        const QString link = m_confDir + QStringLiteral("/sites-enabled/") + fileName;
        if (executor) {
            QString err;
            runBlocking(executor, QStringLiteral("rm -f %1").arg(shellQuote(link)),
                        kDefaultTimeoutMs, &err);
            emit operationFinished(err.isEmpty(), QStringLiteral("disable"),
                                   err.isEmpty() ? QString() : err);
            return;
        }
        QFile::remove(link);
        emit operationFinished(true, QStringLiteral("disable"), QString());
    });
}

// ---------------------------------------------------------------------------
// 校验 / 重载
// ---------------------------------------------------------------------------

void NginxManager::testConfig()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        const QString out = runBlocking(executor, buildTestCommand(), kDefaultTimeoutMs, &err);
        const bool ok = err.isEmpty()
                        && (out.contains(QLatin1String("syntax is ok"))
                            || out.contains(QLatin1String("test is successful")));
        const QString message = out.trimmed().isEmpty() ? err : out.trimmed();
        emit testResultReady(ok, message);
    });
}

void NginxManager::startService()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        runBlocking(executor, buildStartCommand(), kDefaultTimeoutMs, &err);
        emit operationFinished(err.isEmpty(), QStringLiteral("start"), err);
    });
}

void NginxManager::stopService()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        runBlocking(executor, buildStopCommand(), kDefaultTimeoutMs, &err);
        emit operationFinished(err.isEmpty(), QStringLiteral("stop"), err);
    });
}

void NginxManager::restartService()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        runBlocking(executor, buildRestartCommand(), kDefaultTimeoutMs, &err);
        emit operationFinished(err.isEmpty(), QStringLiteral("restart"), err);
    });
}

void NginxManager::reload()
{
    CommandExecutor *executor = executorSnapshot();
    schedule([this, executor]() {
        QString err;
        QString out = runBlocking(executor, buildReloadCommand(), kDefaultTimeoutMs, &err);
        if (!err.isEmpty() && out.trimmed().isEmpty()) {
            // 回退：systemctl reload nginx（有些系统 nginx -s reload 需要 pid 路径）。
            out = runBlocking(executor, QStringLiteral("systemctl reload nginx"),
                              kDefaultTimeoutMs, &err);
        }
        const bool ok = err.isEmpty();
        emit reloadFinished(ok, ok ? QString() : (out.trimmed().isEmpty() ? err : out.trimmed()));
    });
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

void NginxManager::fetchLog(const QString &kind, int tailLines)
{
    CommandExecutor *executor = executorSnapshot();
    const QString path = logFilePath(kind);
    const QString title = (kind == QLatin1String("access")) ? QStringLiteral("access.log")
                                                            : QStringLiteral("error.log");
    schedule([this, executor, path, title, tailLines]() {
        QString err;
        const QString out = runRead(
            executor, QStringLiteral("tail -n %1 %2 2>&1").arg(tailLines).arg(shellQuote(path)),
            kDefaultTimeoutMs, &err);
        emit logTextReady(title, out.trimmed().isEmpty() ? err : out);
    });
}

void NginxManager::startLogStream(const QString &kind)
{
    if (m_logsRunning.load())
        return;
    const QString path = logFilePath(kind);
    const QString title = (kind == QLatin1String("access")) ? QStringLiteral("access.log")
                                                            : QStringLiteral("error.log");
    const QString cmdLine =
        QStringLiteral("tail -n 100 -f %1 2>&1").arg(shellQuote(path));
    m_logsRunning.store(true);

    CommandExecutor *executor = executorSnapshot();
    m_logsRemote = (executor != nullptr);
    if (executor) {
        disconnect(m_logsChunkConn);
        disconnect(m_logsFinishedConn);
        m_logsChunkConn = connect(executor, &CommandExecutor::outputChunk, this,
                                  [this](const QByteArray &chunk) {
                                      if (m_logsRunning.load())
                                          emit logChunk(QString::fromUtf8(chunk));
                                  }, Qt::QueuedConnection);
        m_logsFinishedConn = connect(
            executor, &CommandExecutor::streamFinished, this,
            [this](int, const QString &, const QString &) {
                if (m_logsRunning.load()) {
                    m_logsRunning.store(false);
                    emit logsFinished(0);
                }
            }, Qt::QueuedConnection);
        const QString cmd = needsSudo() ? (QStringLiteral("sudo -S ") + cmdLine) : cmdLine;
        emit logStarted(title);
        if (!executor->execStream(cmd)) {
            m_logsRunning.store(false);
            disconnect(m_logsChunkConn);
            disconnect(m_logsFinishedConn);
            emit errorOccurred(QStringLiteral("已有命令流在运行，无法启动日志查看"));
        }
        return;
    }

    m_logsProcess = new QProcess(this);
    m_logsProcess->setProcessEnvironment(localProcessEnvironment());
    connect(m_logsProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        emit logChunk(QString::fromUtf8(m_logsProcess->readAllStandardOutput()));
    });
    connect(m_logsProcess, &QProcess::readyReadStandardError, this, [this]() {
        emit logChunk(QString::fromUtf8(m_logsProcess->readAllStandardError()));
    });
    connect(m_logsProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                if (m_logsRunning.load()) {
                    m_logsRunning.store(false);
                    emit logsFinished(code);
                }
                m_logsProcess->deleteLater();
                m_logsProcess = nullptr;
            });
#ifdef Q_OS_WIN
    m_logsProcess->start(QStringLiteral("cmd"), {QStringLiteral("/c"), cmdLine});
#else
    m_logsProcess->start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), cmdLine});
#endif
    emit logStarted(title);
}

void NginxManager::stopLogStream()
{
    if (!m_logsRunning.load())
        return;
    m_logsRunning.store(false);
    if (m_logsRemote) {
        CommandExecutor *executor = executorSnapshot();
        if (executor) {
            disconnect(m_logsChunkConn);
            disconnect(m_logsFinishedConn);
            executor->cancel();
        }
        m_logsRemote = false;
        return;
    }
    if (m_logsProcess) {
        m_logsProcess->kill(); // finished() 处理器负责 deleteLater
    }
}

} // namespace cubeshell
