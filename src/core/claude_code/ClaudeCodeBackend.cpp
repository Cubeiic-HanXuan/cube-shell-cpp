// ClaudeCodeBackend.cpp — see ClaudeCodeBackend.h for the port map.
// 对应Python: core/claude_code/backend.py

#include "claude_code/ClaudeCodeBackend.h"

#include "ssh/CommandExecutor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>

namespace cubeshell {

// 对应Python: RemoteBackend.run_command 的退出码哨兵
static const QLatin1String kExitSentinel("__EXIT_CODE__");

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

ClaudeCodeBackend::ClaudeCodeBackend(QObject *parent)
    : QObject(parent)
{
}

ClaudeCodeBackend::~ClaudeCodeBackend()
{
    abortChat();
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

void ClaudeCodeBackend::setRemoteExecutor(CommandExecutor *executor)
{
    m_executor = executor;
}

void ClaudeCodeBackend::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

// ---------------------------------------------------------------------------
// binary discovery
// ---------------------------------------------------------------------------

// 对应Python: _get_login_shell_path(GUI 启动 PATH 不完整时拉起登录 shell)
QString ClaudeCodeBackend::loginShellPath() const
{
    if (m_loginPathProbed)
        return m_cachedLoginPath;
    m_loginPathProbed = true;
#ifndef Q_OS_WIN
    const QString shell = qEnvironmentVariable("SHELL",
                                               QStringLiteral("/bin/bash"));
    QProcess proc;
    proc.start(shell, {QStringLiteral("-ilc"),
                       QStringLiteral("printf %s \"$PATH\"")});
    if (proc.waitForFinished(5000))
        m_cachedLoginPath = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    else
        proc.kill();
#endif
    return m_cachedLoginPath;
}

// 对应Python: _find_claude_bin(PATH -> 登录 shell PATH -> 常见安装目录)
QString ClaudeCodeBackend::findClaudeBin() const
{
    if (!m_cachedBin.isEmpty())
        return m_cachedBin;

    // 1. 当前进程 PATH
    QString found = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!found.isEmpty()) {
        m_cachedBin = found;
        return found;
    }

    // 2. 登录 shell 的完整 PATH
    const QString loginPath = loginShellPath();
    if (!loginPath.isEmpty()) {
        found = QStandardPaths::findExecutable(
            QStringLiteral("claude"),
            loginPath.split(QDir::listSeparator(), Qt::SkipEmptyParts));
        if (!found.isEmpty()) {
            m_cachedBin = found;
            return found;
        }
    }

    // 3. 常见安装目录 + node 版本管理器路径
    const QString home = QDir::homePath();
    QStringList candidates = {
        home + QStringLiteral("/.claude/local/claude"), // 官方原生安装器
        home + QStringLiteral("/.claude/bin/claude"),
        QStringLiteral("/usr/local/bin/claude"),
        QStringLiteral("/opt/homebrew/bin/claude"),
        home + QStringLiteral("/.local/bin/claude"),
        home + QStringLiteral("/.npm-global/bin/claude"),
        home + QStringLiteral("/.volta/bin/claude"),
        home + QStringLiteral("/Library/pnpm/claude"),
    };
    // nvm/fnm/n 安装路径,取最新版本优先(逆序)
    const QList<QPair<QString, QString>> globRoots = {
        {home + QStringLiteral("/.nvm/versions/node"),
         QStringLiteral("/bin/claude")},
        {home + QStringLiteral("/.fnm/node-versions"),
         QStringLiteral("/installation/bin/claude")},
        {QStringLiteral("/usr/local/n/versions/node"),
         QStringLiteral("/bin/claude")},
    };
    for (const auto &root : globRoots) {
        QDir dir(root.first);
        QStringList versions = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                             QDir::Name | QDir::Reversed);
        for (const QString &v : versions)
            candidates.append(root.first + QLatin1Char('/') + v + root.second);
    }

    for (const QString &path : candidates) {
        const QFileInfo fi(path);
        if (fi.isFile() && fi.isExecutable()) {
            m_cachedBin = path;
            return path;
        }
    }

    // 都找不到返回裸命令名,让后续报错更明确
    m_cachedBin = QStringLiteral("claude");
    return m_cachedBin;
}

// POSIX 单引号转义,对应Python: shlex.quote
QString ClaudeCodeBackend::shellQuote(const QString &s) const
{
    QString out = s;
    out.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

QString ClaudeCodeBackend::claudeHome() const
{
    return QDir::homePath() + QStringLiteral("/.claude");
}

// 对应Python: backend.py::build_cd_command（行 20-36）
// POSIX 用 cd 'dir' && cmd；Windows PowerShell 5.1 不支持 &&，用
// Set-Location + $? 实现"切目录成功才执行"的等价语义。
QString ClaudeCodeBackend::buildCdCommand(const QString &cwd,
                                          const QString &command)
{
    if (cwd.isEmpty())
        return command;
#ifdef Q_OS_WIN
    // PowerShell 单引号字符串内的单引号写成两个单引号转义
    QString quoted = cwd;
    quoted.replace(QLatin1Char('\''), QLatin1String("''"));
    return QStringLiteral("Set-Location -LiteralPath '%1'; if ($?) { %2 }")
        .arg(quoted, command);
#else
    // shlex.quote 等价：单引号包裹 + 内部单引号转义
    QString quoted = cwd;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QStringLiteral("cd '%1' && %2").arg(quoted, command);
#endif
}

// 对应Python: backend.py::build_install_command（行 39-52）
// 安装在真实终端里交互执行（而非后台子进程），便于用户查看进度。
QString ClaudeCodeBackend::buildInstallCommand()
{
#ifdef Q_OS_WIN
    return QStringLiteral("irm https://claude.ai/install.ps1 | iex");
#else
    return QStringLiteral("curl -fsSL https://claude.ai/install.sh | bash");
#endif
}

// 对应Python: settings_widget.py::_build_settings_dict 约定（行 347-396）：
// settings.json 的 env 字段承载 ANTHROPIC_BASE_URL、ANTHROPIC_AUTH_TOKEN/
// ANTHROPIC_API_KEY 两种鉴权键及 ANTHROPIC_DEFAULT_OPUS/SONNET/HAIKU_MODEL。
// 本地执行 claude 前整体注入，保证第三方中转站配置对 CLI 生效。
// （远程模式不注入：远端 claude 自行读取远端 settings.json。）
void ClaudeCodeBackend::applySettingsEnv(QProcessEnvironment *env)
{
    if (m_executor)
        return;
    const QJsonObject settings = readSettings();
    const QJsonObject settingsEnv =
        settings.value(QStringLiteral("env")).toObject();
    for (auto it = settingsEnv.begin(); it != settingsEnv.end(); ++it) {
        const QJsonValue v = it.value();
        if (v.isString())
            env->insert(it.key(), v.toString());
        else if (v.isDouble())
            env->insert(it.key(), QString::number(v.toDouble()));
        else if (v.isBool())
            env->insert(it.key(), v.toBool() ? QStringLiteral("true")
                                             : QStringLiteral("false"));
    }
}

// ---------------------------------------------------------------------------
// blocking command execution
// ---------------------------------------------------------------------------

// 对应Python: RemoteBackend.run_command 的哨兵解析部分
ClaudeRunResult ClaudeCodeBackend::parseExitCodeSentinel(const QString &output)
{
    ClaudeRunResult result;
    result.exitCode = 0;
    QStringList stdoutLines;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(kExitSentinel)) {
            bool ok = false;
            result.exitCode = line.mid(kExitSentinel.size()).trimmed().toInt(&ok);
            if (!ok)
                result.exitCode = -1;
        } else {
            stdoutLines.append(line);
        }
    }
    // 去掉尾部因 split 产生的空行
    while (!stdoutLines.isEmpty() && stdoutLines.last().isEmpty())
        stdoutLines.removeLast();
    result.stdoutText = stdoutLines.join(QLatin1Char('\n'));
    return result;
}

// 对应Python: LocalBackend.run_command / RemoteBackend.run_command
ClaudeRunResult ClaudeCodeBackend::runCommand(const QStringList &args,
                                              int timeoutMs)
{
    ClaudeRunResult result;
    if (m_executor) {
        // 远程:哨兵取退出码(SSH exec 不直接回传 returncode)
        QStringList quoted;
        for (const QString &a : args)
            quoted.append(shellQuote(a));
        const QString cmd = QStringLiteral("claude %1; echo \"%2$?\"")
                                .arg(quoted.join(QLatin1Char(' ')),
                                     QString(kExitSentinel));
        const ExecResult r = m_executor->exec(cmd, false, timeoutMs);
        if (!r.ok()) {
            result.stderrText = r.errorMessage.isEmpty()
                ? QStringLiteral("Command timed out") : r.errorMessage;
            return result;
        }
        result = parseExitCodeSentinel(r.stdoutText);
        result.stderrText = r.stderrText;
        return result;
    }

    // 本地:QProcess 阻塞执行(在工作线程调用)
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // 对应Python: _build_subprocess_env(合入登录 shell PATH,含 node)
    const QString loginPath = loginShellPath();
    if (!loginPath.isEmpty()) {
        const QString existing = env.value(QStringLiteral("PATH"));
        env.insert(QStringLiteral("PATH"),
                   existing.isEmpty() ? loginPath
                                      : existing + QDir::listSeparator() + loginPath);
    }
    // settings.json 的 env 字段注入（鉴权/模型路由约定，见 applySettingsEnv）
    applySettingsEnv(&env);
    proc.setProcessEnvironment(env);
    proc.start(findClaudeBin(), args);
    if (!proc.waitForStarted(5000)) {
        result.stderrText =
            QStringLiteral("claude binary not found: %1").arg(findClaudeBin());
        return result;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        result.stderrText = QStringLiteral("Command timed out");
        return result;
    }
    result.exitCode = proc.exitCode();
    result.stdoutText = QString::fromUtf8(proc.readAllStandardOutput());
    result.stderrText = QString::fromUtf8(proc.readAllStandardError());
    return result;
}

// ---------------------------------------------------------------------------
// status queries
// ---------------------------------------------------------------------------

// 对应Python: get_version
QString ClaudeCodeBackend::version()
{
    const ClaudeRunResult r = runCommand({QStringLiteral("--version")});
    return r.exitCode == 0 ? r.stdoutText.trimmed() : QString();
}

// 对应Python: _parse_auth_text
QJsonObject ClaudeCodeBackend::parseAuthText(const QString &text)
{
    QJsonObject result;
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        QString key = line.left(colon).trimmed().toLower();
        key.replace(QLatin1Char(' '), QLatin1Char('_'));
        result.insert(key, line.mid(colon + 1).trimmed());
    }
    if (result.isEmpty())
        result.insert(QStringLiteral("raw"), text.trimmed());
    return result;
}

// 对应Python: get_auth_status(--json 优先,--text 回退)
QJsonObject ClaudeCodeBackend::authStatus()
{
    ClaudeRunResult r = runCommand({QStringLiteral("auth"),
                                    QStringLiteral("status"),
                                    QStringLiteral("--json")});
    if (r.exitCode == 0 && !r.stdoutText.trimmed().isEmpty()) {
        const QJsonDocument doc =
            QJsonDocument::fromJson(r.stdoutText.toUtf8());
        if (doc.isObject())
            return doc.object();
    }
    r = runCommand({QStringLiteral("auth"), QStringLiteral("status"),
                    QStringLiteral("--text")});
    if (r.exitCode == 0 && !r.stdoutText.trimmed().isEmpty())
        return parseAuthText(r.stdoutText);
    return QJsonObject();
}

// 对应Python: _parse_daemon_status(不能用 "running" 子串判断)
QJsonObject ClaudeCodeBackend::parseDaemonStatus(int exitCode,
                                                 const QString &stdoutText)
{
    const QString text = stdoutText.trimmed();
    if (!text.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        if (doc.isObject())
            return doc.object();
    }
    QJsonObject result;
    if (text.isEmpty()) {
        result.insert(QStringLiteral("running"), false);
        return result;
    }
    const QString firstLine =
        text.section(QLatin1Char('\n'), 0, 0).trimmed().toLower();
    result.insert(QStringLiteral("raw"), text);
    result.insert(QStringLiteral("running"),
                  exitCode == 0 && !firstLine.startsWith(
                      QLatin1String("not running")));
    return result;
}

// 对应Python: get_daemon_status
QJsonObject ClaudeCodeBackend::daemonStatus()
{
    const ClaudeRunResult r = runCommand({QStringLiteral("daemon"),
                                          QStringLiteral("status")});
    return parseDaemonStatus(r.exitCode, r.stdoutText);
}

// ---------------------------------------------------------------------------
// session listing
// ---------------------------------------------------------------------------

// 对应Python: _iso_to_ms
static qint64 isoToMs(const QString &ts)
{
    if (ts.isEmpty())
        return 0;
    const QDateTime dt = QDateTime::fromString(ts, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

// 对应Python: _parse_transcript_head(只读头部,避免加载大文件)
void ClaudeCodeBackend::parseTranscriptHead(const QString &content,
                                            QString *nameOut, QString *cwdOut,
                                            qint64 *firstTsMsOut, int maxLines)
{
    QString name;
    QString cwd;
    QString firstTs;
    QString firstUser;
    const QStringList lines = content.split(QLatin1Char('\n'));
    const int limit = qMin(maxLines, static_cast<int>(lines.size()));
    for (int i = 0; i < limit; ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject())
            continue;
        const QJsonObject d = doc.object();
        if (cwd.isEmpty())
            cwd = d.value(QStringLiteral("cwd")).toString();
        if (name.isEmpty()
            && d.value(QStringLiteral("type")).toString()
                   == QLatin1String("ai-title"))
            name = d.value(QStringLiteral("aiTitle")).toString();
        if (firstTs.isEmpty())
            firstTs = d.value(QStringLiteral("timestamp")).toString();
        if (firstUser.isEmpty()
            && d.value(QStringLiteral("type")).toString()
                   == QLatin1String("user")) {
            const QJsonValue msg = d.value(QStringLiteral("message"));
            const QJsonValue content2 =
                msg.toObject().value(QStringLiteral("content"));
            if (content2.isArray()) {
                QStringList parts;
                const QJsonArray arr = content2.toArray();
                for (const QJsonValue &x : arr) {
                    const QJsonObject xo = x.toObject();
                    const QString type = xo.value(QStringLiteral("type")).toString();
                    if (type.isEmpty() || type == QLatin1String("text"))
                        parts.append(xo.value(QStringLiteral("text")).toString());
                }
                firstUser = parts.join(QLatin1Char(' ')).trimmed();
            } else if (content2.isString()) {
                firstUser = content2.toString().trimmed();
            }
        }
        if (!name.isEmpty() && !cwd.isEmpty() && !firstTs.isEmpty())
            break;
    }
    QString title = name.isEmpty() ? firstUser : name;
    title = title.simplified(); // 折叠换行/多空格
    if (title.size() > 80)
        title = title.left(80) + QChar(0x2026); // '…'
    if (nameOut)
        *nameOut = title;
    if (cwdOut)
        *cwdOut = cwd;
    if (firstTsMsOut)
        *firstTsMsOut = isoToMs(firstTs);
}

// 对应Python: _live_agent_status(claude agents --json --all)
QMap<QString, QString> ClaudeCodeBackend::liveAgentStatus()
{
    QMap<QString, QString> result;
    const ClaudeRunResult r = runCommand({QStringLiteral("agents"),
                                          QStringLiteral("--json"),
                                          QStringLiteral("--all")});
    if (r.exitCode != 0 || r.stdoutText.trimmed().isEmpty())
        return result;
    const QJsonDocument doc = QJsonDocument::fromJson(r.stdoutText.toUtf8());
    if (!doc.isArray())
        return result;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        const QJsonObject a = v.toObject();
        const QString sid = a.value(QStringLiteral("sessionId")).toString();
        if (!sid.isEmpty())
            result.insert(sid, a.value(QStringLiteral("status"))
                                   .toString(QStringLiteral("running")));
    }
    return result;
}

// 对应Python: LocalBackend._read_transcript_sessions
QList<ClaudeSessionInfo> ClaudeCodeBackend::readLocalTranscripts()
{
    QList<ClaudeSessionInfo> results;
    const QDir projectsDir(claudeHome() + QStringLiteral("/projects"));
    if (!projectsDir.exists())
        return results;
    const QStringList projects =
        projectsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &proj : projects) {
        const QDir projDir(projectsDir.filePath(proj));
        const QStringList files = projDir.entryList(
            {QStringLiteral("*.jsonl")}, QDir::Files);
        for (const QString &fn : files) {
            const QString path = projDir.filePath(fn);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            // 只读头部 32KB,对应Python逐行读 80 行的意图
            const QString head = QString::fromUtf8(file.read(32 * 1024));
            file.close();
            ClaudeSessionInfo info;
            qint64 firstTs = 0;
            parseTranscriptHead(head, &info.name, &info.cwd, &firstTs);
            info.sessionId = fn.chopped(6); // 去掉 ".jsonl"
            info.status = QStringLiteral("saved");
            info.startedAtMs = firstTs > 0
                ? firstTs
                : QFileInfo(path).lastModified().toMSecsSinceEpoch();
            results.append(info);
        }
    }
    return results;
}

// 对应Python: RemoteBackend._read_remote_transcripts(单条 shell 脚本)
QList<ClaudeSessionInfo> ClaudeCodeBackend::readRemoteTranscripts()
{
    QList<ClaudeSessionInfo> results;
    if (!m_executor)
        return results;
    // 兼容 GNU/BSD stat;grep 提取首个 aiTitle 与非空 cwd
    const QString script = QStringLiteral(
        "for f in ~/.claude/projects/*/*.jsonl; do "
        "[ -f \"$f\" ] || continue; "
        "sid=$(basename \"$f\" .jsonl); "
        "mt=$(stat -c %Y \"$f\" 2>/dev/null || stat -f %m \"$f\" 2>/dev/null); "
        "title=$(grep -m1 -oE '\"aiTitle\": *\"[^\"]*\"' \"$f\" 2>/dev/null); "
        "cwd=$(grep -m1 -oE '\"cwd\": *\"[^\"]+\"' \"$f\" 2>/dev/null); "
        "printf '%s\\t%s\\t%s\\t%s\\n' \"$sid\" \"$mt\" \"$title\" \"$cwd\"; "
        "done");
    const ExecResult r = m_executor->exec(script, false,
                                          CommandExecutor::kDefaultTimeoutMs);
    if (!r.ok() || r.stdoutText.isEmpty())
        return results;

    // fragment 形如 "aiTitle": "xxx",取冒号后引号内的值
    const auto fragmentValue = [](const QString &fragment) -> QString {
        const QJsonDocument doc = QJsonDocument::fromJson(
            (QStringLiteral("{") + fragment + QStringLiteral("}")).toUtf8());
        if (!doc.isObject() || doc.object().isEmpty())
            return QString();
        return doc.object().begin().value().toString();
    };

    const QStringList lines = r.stdoutText.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.isEmpty() || parts.at(0).isEmpty())
            continue;
        ClaudeSessionInfo info;
        info.sessionId = parts.at(0);
        const double mt = parts.size() > 1 ? parts.at(1).toDouble() : 0.0;
        info.name = parts.size() > 2 ? fragmentValue(parts.at(2)) : QString();
        info.cwd = parts.size() > 3 ? fragmentValue(parts.at(3)) : QString();
        info.status = QStringLiteral("saved");
        info.startedAtMs = static_cast<qint64>(mt * 1000);
        results.append(info);
    }
    return results;
}

// 对应Python: list_sessions(transcript + 运行状态叠加,按时间降序)
QList<ClaudeSessionInfo> ClaudeCodeBackend::listSessions()
{
    QList<ClaudeSessionInfo> sessions =
        m_executor ? readRemoteTranscripts() : readLocalTranscripts();
    const QMap<QString, QString> live = liveAgentStatus();
    for (ClaudeSessionInfo &s : sessions) {
        if (live.contains(s.sessionId)) {
            s.status = live.value(s.sessionId);
            s.running = true;
        }
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const ClaudeSessionInfo &a, const ClaudeSessionInfo &b) {
                  return a.startedAtMs > b.startedAtMs;
              });
    return sessions;
}

// ---------------------------------------------------------------------------
// settings / MCP config
// ---------------------------------------------------------------------------

// 对应Python: read_settings
QJsonObject ClaudeCodeBackend::readSettings()
{
    QString content;
    if (m_executor) {
        const ExecResult r = m_executor->exec(
            QStringLiteral("cat ~/.claude/settings.json 2>/dev/null"), false,
            CommandExecutor::kDefaultTimeoutMs);
        content = r.stdoutText;
    } else {
        QFile file(claudeHome() + QStringLiteral("/settings.json"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            content = QString::fromUtf8(file.readAll());
    }
    if (content.trimmed().isEmpty())
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// 对应Python: write_settings
bool ClaudeCodeBackend::writeSettings(const QJsonObject &settings,
                                      QString *errorOut)
{
    const QByteArray content =
        QJsonDocument(settings).toJson(QJsonDocument::Indented);
    if (m_executor) {
        // 远程 base64 写入,保证字节精确(比 Python printf 转义更稳)
        const QString cmd = QStringLiteral(
            "mkdir -p ~/.claude && echo %1 | base64 -d > ~/.claude/settings.json")
            .arg(QString::fromLatin1(content.toBase64()));
        const ExecResult r = m_executor->exec(cmd, false,
                                              CommandExecutor::kDefaultTimeoutMs);
        if (!r.ok() || r.exitCode != 0) {
            if (errorOut)
                *errorOut = r.errorMessage.isEmpty() ? r.stderrText
                                                     : r.errorMessage;
            return false;
        }
        return true;
    }
    QDir().mkpath(claudeHome());
    QFile file(claudeHome() + QStringLiteral("/settings.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    file.write(content);
    return true;
}

// 对应Python: _mcp_config_path / _remote_mcp_path
static QString mcpConfigPath(bool remote, const QString &scope,
                             const QString &projectPath)
{
    if (scope == QLatin1String("project")) {
        QString base = projectPath.isEmpty() ? QStringLiteral(".") : projectPath;
        while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
            base.chop(1);
        return base + QStringLiteral("/.mcp.json");
    }
    return remote ? QStringLiteral("~/.claude.json")
                  : QDir::homePath() + QStringLiteral("/.claude.json");
}

// 对应Python: read_mcp_config
QJsonObject ClaudeCodeBackend::readMcpConfig(const QString &scope,
                                             const QString &projectPath)
{
    const QString path = mcpConfigPath(isRemote(), scope, projectPath);
    QString content;
    if (m_executor) {
        const ExecResult r = m_executor->exec(
            QStringLiteral("cat %1 2>/dev/null").arg(path), false,
            CommandExecutor::kDefaultTimeoutMs);
        content = r.stdoutText;
    } else {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            content = QString::fromUtf8(file.readAll());
    }
    QJsonObject result;
    QJsonObject servers;
    if (!content.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
        if (doc.isObject())
            servers = doc.object().value(QStringLiteral("mcpServers")).toObject();
    }
    result.insert(QStringLiteral("mcpServers"), servers);
    return result;
}

// 对应Python: write_mcp_config(读-改-写,只替换 mcpServers 键)
bool ClaudeCodeBackend::writeMcpConfig(const QJsonObject &config,
                                       const QString &scope,
                                       const QString &projectPath,
                                       QString *errorOut)
{
    const QString path = mcpConfigPath(isRemote(), scope, projectPath);
    const QJsonObject servers =
        config.value(QStringLiteral("mcpServers")).toObject();

    // 读取现有配置,保留其它键
    QJsonObject existing;
    {
        QString content;
        if (m_executor) {
            const ExecResult r = m_executor->exec(
                QStringLiteral("cat %1 2>/dev/null").arg(path), false,
                CommandExecutor::kDefaultTimeoutMs);
            content = r.stdoutText;
        } else {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text))
                content = QString::fromUtf8(file.readAll());
        }
        if (!content.trimmed().isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
            if (doc.isObject())
                existing = doc.object();
        }
    }
    existing.insert(QStringLiteral("mcpServers"), servers);
    const QByteArray content =
        QJsonDocument(existing).toJson(QJsonDocument::Indented);

    if (m_executor) {
        const QString dirPart = path.contains(QLatin1Char('/'))
            ? path.left(path.lastIndexOf(QLatin1Char('/')))
            : QStringLiteral(".");
        const QString cmd = QStringLiteral(
            "mkdir -p %1 && echo %2 | base64 -d > %3")
            .arg(dirPart, QString::fromLatin1(content.toBase64()), path);
        const ExecResult r = m_executor->exec(cmd, false,
                                              CommandExecutor::kDefaultTimeoutMs);
        if (!r.ok() || r.exitCode != 0) {
            if (errorOut)
                *errorOut = r.errorMessage.isEmpty() ? r.stderrText
                                                     : r.errorMessage;
            return false;
        }
        return true;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    file.write(content);
    return true;
}

// ---------------------------------------------------------------------------
// async wrappers
// ---------------------------------------------------------------------------

// 对应Python: status_widget.StatusWorker（行 44-69，bin_path 一并回传）
void ClaudeCodeBackend::refreshStatus()
{
    schedule([this]() {
        emit statusLoaded(version(), authStatus(), daemonStatus(), binPath());
    });
}

// 对应Python: session_widget.SessionWorker
void ClaudeCodeBackend::refreshSessions()
{
    schedule([this]() {
        emit sessionsLoaded(listSessions());
    });
}

// 对应Python: status_widget.py::StatusWorker.run 行 57-65
QString ClaudeCodeBackend::binPath()
{
    return m_executor ? QStringLiteral("claude (远程)") : findClaudeBin();
}

// 对应Python: status_widget.py::UpdateWorker.run（行 25-32）：
// run_command(["update"])，output = stdout or stderr，完成后回传日志区。
void ClaudeCodeBackend::refreshUpdate()
{
    schedule([this]() {
        const ClaudeRunResult r = runCommand({QStringLiteral("update")});
        const QString output =
            !r.stdoutText.isEmpty() ? r.stdoutText : r.stderrText;
        emit updateFinished(output.trimmed());
    });
}

// 对应Python: settings_widget.py::SettingsWorker（mode=load）
void ClaudeCodeBackend::refreshSettings()
{
    schedule([this]() {
        emit settingsLoaded(readSettings());
    });
}

// 对应Python: settings_widget.py::SettingsWorker（mode=save）
void ClaudeCodeBackend::saveSettings(const QJsonObject &settings)
{
    schedule([this, settings]() {
        QString error;
        const bool ok = writeSettings(settings, &error);
        emit settingsSaved(ok, error);
    });
}

// 对应Python: mcp_widget.py::McpWorker（mode=load）
void ClaudeCodeBackend::refreshMcpConfig(const QString &scope,
                                         const QString &projectPath)
{
    schedule([this, scope, projectPath]() {
        emit mcpConfigLoaded(readMcpConfig(scope, projectPath));
    });
}

// 对应Python: mcp_widget.py::McpWorker（mode=save）
void ClaudeCodeBackend::saveMcpConfig(const QJsonObject &config,
                                      const QString &scope,
                                      const QString &projectPath)
{
    schedule([this, config, scope, projectPath]() {
        QString error;
        const bool ok = writeMcpConfig(config, scope, projectPath, &error);
        emit mcpConfigSaved(ok, error);
    });
}

// ---------------------------------------------------------------------------
// chat streaming (JSON-line protocol)
// ---------------------------------------------------------------------------

bool ClaudeCodeBackend::isChatting() const
{
    return m_chatProcess != nullptr || m_remoteChatActive;
}

// 增量解析 JSON-line:每个完整行是一个 JSON 对象
void ClaudeCodeBackend::feedChatBuffer(const QByteArray &chunk)
{
    m_chatBuffer.append(chunk);
    int newlinePos;
    while ((newlinePos = m_chatBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_chatBuffer.left(newlinePos);
        m_chatBuffer.remove(0, newlinePos + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            emit chatMessage(doc.object());
    }
}

void ClaudeCodeBackend::flushChatBuffer()
{
    const QByteArray tail = m_chatBuffer.trimmed();
    m_chatBuffer.clear();
    if (tail.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(tail);
    if (doc.isObject())
        emit chatMessage(doc.object());
}

// 对应Python: claude CLI 的 stream-json 协议(claude -p ... --output-format
// stream-json --verbose),远程经 CommandExecutor::execStream
bool ClaudeCodeBackend::startChat(const QString &prompt, const QString &cwd,
                                  const QString &resumeSessionId)
{
    if (isChatting())
        return false;
    m_chatBuffer.clear();

    QStringList args = {QStringLiteral("-p"), prompt,
                        QStringLiteral("--output-format"),
                        QStringLiteral("stream-json"),
                        QStringLiteral("--verbose")};
    if (!resumeSessionId.isEmpty())
        args << QStringLiteral("--resume") << resumeSessionId;

    if (m_executor) {
        // 远程:execStream 自带工作线程,信号跨线程回到本对象
        QStringList quoted;
        for (const QString &a : args)
            quoted.append(shellQuote(a));
        QString cmd = QStringLiteral("claude ") + quoted.join(QLatin1Char(' '));
        if (!cwd.isEmpty())
            cmd = QStringLiteral("cd %1 && %2").arg(shellQuote(cwd), cmd);
        connect(m_executor, &CommandExecutor::outputChunk, this,
                [this](const QByteArray &chunk) { feedChatBuffer(chunk); },
                Qt::QueuedConnection);
        connect(m_executor, &CommandExecutor::streamFinished, this,
                [this](int exitCode, const QString &, const QString &) {
                    flushChatBuffer();
                    m_remoteChatActive = false;
                    if (m_executor)   // executor 可能已随 SSH tab 析构
                        m_executor->disconnect(this);
                    emit chatFinished(exitCode);
                },
                Qt::QueuedConnection);
        connect(m_executor, &CommandExecutor::streamError, this,
                [this](const QString &message) {
                    m_remoteChatActive = false;
                    if (m_executor)   // executor 可能已随 SSH tab 析构
                        m_executor->disconnect(this);
                    emit chatError(message);
                },
                Qt::QueuedConnection);
        if (!m_executor->execStream(cmd, false,
                                    CommandExecutor::kLongRunningTimeoutMs)) {
            m_executor->disconnect(this);
            return false;
        }
        m_remoteChatActive = true;
        return true;
    }

    // 本地:QProcess 事件驱动(信号在本对象线程)
    m_chatProcess = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString loginPath = loginShellPath();
    if (!loginPath.isEmpty()) {
        const QString existing = env.value(QStringLiteral("PATH"));
        env.insert(QStringLiteral("PATH"),
                   existing.isEmpty() ? loginPath
                                      : existing + QDir::listSeparator() + loginPath);
    }
    // settings.json 的 env 字段注入（同 runCommand，见 applySettingsEnv）
    applySettingsEnv(&env);
    m_chatProcess->setProcessEnvironment(env);
    if (!cwd.isEmpty())
        m_chatProcess->setWorkingDirectory(cwd);
    connect(m_chatProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        feedChatBuffer(m_chatProcess->readAllStandardOutput());
    });
    connect(m_chatProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) {
                feedChatBuffer(m_chatProcess->readAllStandardOutput());
                flushChatBuffer();
                m_chatProcess->deleteLater();
                m_chatProcess = nullptr;
                emit chatFinished(exitCode);
            });
    connect(m_chatProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    const QString msg = QStringLiteral(
                        "claude binary not found: %1").arg(findClaudeBin());
                    m_chatProcess->deleteLater();
                    m_chatProcess = nullptr;
                    emit chatError(msg);
                }
            });
    m_chatProcess->start(findClaudeBin(), args);
    return true;
}

void ClaudeCodeBackend::abortChat()
{
    if (m_chatProcess) {
        QProcess *proc = m_chatProcess;
        m_chatProcess = nullptr;
        proc->disconnect(this);
        proc->kill();
        proc->waitForFinished(2000);
        proc->deleteLater();
    }
    if (m_remoteChatActive && m_executor) {
        m_executor->cancel();
        m_remoteChatActive = false;
        m_executor->disconnect(this);
    }
    m_chatBuffer.clear();
}

} // namespace cubeshell
