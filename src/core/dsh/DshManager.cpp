// DshManager.cpp — DeepSeek Harness 本地管理器。见 DshManager.h 的设计说明。

#include "dsh/DshManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>
#include <memory>

Q_LOGGING_CATEGORY(dshLog, "cubeshell.dsh")

namespace cubeshell {

namespace {
const QString kPackage = QStringLiteral("@deepseek-ai/dsh");
// 健康检查：Starting 后每 800ms 探一次端口，最多 ~40s（首次要 compose 插件层）。
constexpr int kHealthIntervalMs = 800;
constexpr int kHealthMaxAttempts = 50;
} // namespace

DshManager::DshManager(QObject *parent)
    : QObject(parent)
{
    dshDir(); // 目录懒创建

    // 异步环境检测结果回到 UI 线程后转发为信号（QFutureWatcher 的 finished
    // 在它所属线程投递，本对象在 UI 线程，故 result() 取用是安全的）。
    connect(&m_envWatcher, &QFutureWatcherBase::finished, this, [this]() {
        if (m_envWatcher.future().isResultReadyAt(0))
            emit environmentDetected(m_envWatcher.result());
    });

    m_healthTimer = new QTimer(this);
    m_healthTimer->setInterval(kHealthIntervalMs);
    connect(m_healthTimer, &QTimer::timeout, this, [this]() {
        if (m_status != Status::Starting) {
            stopHealthCheck();
            return;
        }
        if (++m_healthAttempts > kHealthMaxAttempts) {
            stopHealthCheck();
            // 超时不算致命：进程仍在跑，可能只是端口非默认。提示看日志。
            emit errorOccurred(QStringLiteral(
                "等待 web 服务就绪超时（%1:%2）。dsh 首次启动需下载/compose "
                "插件层，可稍候用浏览器直连，或查看下方日志确认实际监听端口。")
                                   .arg(m_host)
                                   .arg(m_port));
            return;
        }
        // 用一次性 QTcpSocket 探端口连通性；连得上即认为 web 服务就绪。
        auto *probe = new QTcpSocket(this);
        connect(probe, &QTcpSocket::connected, this, [this, probe]() {
            probe->disconnectFromHost();
            probe->deleteLater();
            stopHealthCheck();
            setStatus(Status::Running);
            emit webReady(webUrl());
        });
        connect(probe, &QTcpSocket::errorOccurred, this, [probe](QAbstractSocket::SocketError) {
            probe->deleteLater(); // 未就绪，下一拍再试
        });
        probe->connectToHost(m_host, quint16(m_port > 0 ? m_port : kDefaultPort));
    });
}

DshManager::~DshManager()
{
    // 析构时收掉托管的子进程，避免留下孤儿 dsh/node。
    stop(1000);
    if (m_installer) {
        m_installer->kill();
        m_installer->waitForFinished(1000);
    }
    if (m_versionChecker) {
        m_versionChecker->kill();
        m_versionChecker->waitForFinished(1000);
    }
    if (m_pluginProc) {
        m_pluginProc->kill();
        m_pluginProc->waitForFinished(1000);
    }
}

// ---------------------------------------------------------------------------
// 路径
// ---------------------------------------------------------------------------

QString DshManager::dshDir()
{
#ifdef Q_OS_DARWIN
    QString dir = QDir::home().filePath(
        QStringLiteral("Library/Application Support/cube-shell/dsh"));
#elif defined(Q_OS_WIN)
    const QString appData =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    QString dir = QDir(appData).filePath(QStringLiteral("cube-shell/dsh"));
#else
    QString dir = QDir::home().filePath(QStringLiteral(".cube-shell/dsh"));
#endif
    QDir().mkpath(dir);
    return QDir::cleanPath(dir);
}

QString DshManager::logPath()
{
    return QDir(dshDir()).filePath(QStringLiteral("dsh.log"));
}

// 用户的 dsh home：$DSH_HOME 优先，否则 ~/.dsh（dsh 自身的默认）。
QString DshManager::dshHome()
{
    const QString fromEnv = qEnvironmentVariable("DSH_HOME");
    if (!fromEnv.trimmed().isEmpty())
        return QDir::cleanPath(fromEnv.trimmed());
    return QDir::cleanPath(QDir::home().filePath(QStringLiteral(".dsh")));
}

QString DshManager::settingsPath()
{
    return QDir(dshHome()).filePath(QStringLiteral("settings.yaml"));
}

QString DshManager::profilesDir()
{
    return QDir(dshHome()).filePath(QStringLiteral("profiles"));
}

QString DshManager::sessionsDir()
{
    return QDir(dshHome()).filePath(QStringLiteral("sessions"));
}

QString DshManager::storagesDir()
{
    return QDir(dshHome()).filePath(QStringLiteral("storages"));
}

// ---------------------------------------------------------------------------
// profile / 插件 / 会话（本地扫描，无子进程）
// ---------------------------------------------------------------------------

QStringList DshManager::listProfiles()
{
    QDir dir(profilesDir());
    if (!dir.exists())
        return {};
    QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    // profiles 目录下的 node_modules 是 pnpm 的存储，不是 profile。
    names.removeAll(QStringLiteral("node_modules"));
    return names;
}

// 读 <profiles>/<profile>/package.json。
static QJsonObject readProfilePackageJson(const QString &profile)
{
    if (profile.isEmpty())
        return {};
    const QString path = QDir(DshManager::profilesDir())
                             .filePath(profile + QStringLiteral("/package.json"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// 显式 dependencies 即该 profile 装的插件（不含 pnpm 拉进来的传递依赖）。
QList<DshManager::PluginInfo> DshManager::listPlugins(const QString &profile)
{
    const QJsonObject root = readProfilePackageJson(profile);
    const QJsonObject deps = root.value(QStringLiteral("dependencies")).toObject();
    QList<PluginInfo> out;
    for (auto it = deps.constBegin(); it != deps.constEnd(); ++it) {
        PluginInfo info;
        info.name = it.key();
        // package.json 里是版本范围（"^0.3.0"）；去掉范围前缀更贴近实际展示。
        QString v = it.value().toString();
        while (!v.isEmpty() && (v.front() == QLatin1Char('^') || v.front() == QLatin1Char('~')
                                || v.front() == QLatin1Char('=') || v.front() == QLatin1Char('v')))
            v.remove(0, 1);
        info.version = v;
        out.append(info);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo &a, const PluginInfo &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return out;
}

QStringList DshManager::profileBundles(const QString &profile)
{
    const QJsonObject root = readProfilePackageJson(profile);
    const QJsonArray arr = root.value(QStringLiteral("dsh")).toObject()
                               .value(QStringLiteral("profile")).toObject()
                               .value(QStringLiteral("bundles")).toArray();
    QStringList out;
    for (const QJsonValue &v : arr)
        out.append(v.toString());
    return out;
}

// ---------------------------------------------------------------------------
// 会话元信息缓存（<home>/storages/*.json）
// ---------------------------------------------------------------------------
// dsh 把会话的可读元信息单独缓存在 storages 下，不在压缩正文里，所以这里能
// 免解压拿到标题和工作目录：
//   session_projcache.json  tables.sessions.<session-id>.identity.cwd    ← 权威 cwd
//                           tables.sessions.<session-id>.rows.title.val  （可为 null）
//                           tables.sessions.<session-id>.rows.sessionStats.val.turns
//   workspace.json          tables.workspaces.<uuid>.{path, sessionIds}  ← 兜底 cwd
// 两者都可能缺条目（如手工拷进来的会话目录），故 listSessions 还有第三层兜底。

namespace {

// session_projcache.json 里一条会话的元信息。
struct SessionMetaCache {
    QString cwd;
    QString title;
    int turns = 0;
};

// 读 <storages>/<fileName> 的 tables.<table> 对象；读不到返回空对象。
QJsonObject readStorageTable(const QString &fileName, const QString &table)
{
    QFile f(QDir(DshManager::storagesDir()).filePath(fileName));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.object().value(QStringLiteral("tables")).toObject()
        .value(table).toObject();
}

// dsh 的行式存储：rows.<key> = {ver, seq, val}。val 可能是 null。
QJsonValue rowValue(const QJsonObject &rows, const QString &key)
{
    return rows.value(key).toObject().value(QStringLiteral("val"));
}

// session-<uuid> → 元信息。
QHash<QString, SessionMetaCache> readSessionMetaCache()
{
    QHash<QString, SessionMetaCache> out;
    const QJsonObject sessions = readStorageTable(
        QStringLiteral("session_projcache.json"), QStringLiteral("sessions"));
    for (auto it = sessions.constBegin(); it != sessions.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        const QJsonObject rows = o.value(QStringLiteral("rows")).toObject();
        SessionMetaCache m;
        m.cwd = o.value(QStringLiteral("identity")).toObject()
                    .value(QStringLiteral("cwd")).toString();
        // title 为 null 时 toString() 得空串，正好表示“未命名”。
        m.title = rowValue(rows, QStringLiteral("title")).toString();
        m.turns = rowValue(rows, QStringLiteral("sessionStats")).toObject()
                      .value(QStringLiteral("turns")).toInt(0);
        out.insert(it.key(), m);
    }
    return out;
}

// session-<uuid> → 工作目录（projcache 里没有该会话时的第二来源）。
QHash<QString, QString> readWorkspaceCwdMap()
{
    QHash<QString, QString> out;
    const QJsonObject workspaces = readStorageTable(
        QStringLiteral("workspace.json"), QStringLiteral("workspaces"));
    for (auto it = workspaces.constBegin(); it != workspaces.constEnd(); ++it) {
        const QJsonObject o = it.value().toObject();
        const QString path = o.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        const QJsonArray ids = o.value(QStringLiteral("sessionIds")).toArray();
        for (const QJsonValue &v : ids)
            out.insert(v.toString(), path);
    }
    return out;
}

} // namespace

QString DshManager::decodeWorkspaceDir(const QString &encoded)
{
    QString s = encoded;
    while (s.startsWith(QLatin1Char('-')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')))
        s.chop(1);
    // 文件系统根目录被编码成字面量 "root"（不是 /root，实测同名工作区下的会话
    // identity.cwd 都是 "/"）。
    if (s.isEmpty() || s == QLatin1String("root"))
        return QStringLiteral("/");
    return QLatin1Char('/') + s.replace(QLatin1Char('-'), QLatin1Char('/'));
}

QList<DshManager::SessionInfo> DshManager::listSessions()
{
    QList<SessionInfo> out;
    QDir root(sessionsDir());
    if (!root.exists())
        return out;
    // 两份缓存各读一次，供下面所有会话查表。
    const QHash<QString, SessionMetaCache> metaByDir = readSessionMetaCache();
    const QHash<QString, QString> cwdByDir = readWorkspaceCwdMap();
    // 工作区目录名 → 已确证的真实 cwd。同一工作区目录下的会话 cwd 必然相同
    // （目录名就是 cwd 的编码），所以任一条会话的精确值可供同目录其他会话复用。
    QHash<QString, QString> exactCwdByWs;

    // <sessions>/<workspace>/session-<uuid>/session.jsonl.zstd
    const QStringList workspaces =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &ws : workspaces) {
        QDir wsDir(root.filePath(ws));
        const QFileInfoList entries =
            wsDir.entryInfoList({QStringLiteral("session-*")},
                                QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries) {
            SessionInfo info;
            // 目录名就是 dsh 的会话 id（含 "session-" 前缀），也是 storages 的
            // 键与 --resume 的实参，原样保留，别剥前缀。
            info.id = fi.fileName();
            info.workspace = ws;
            info.modified = fi.lastModified();

            // 工作目录第一/二来源：projcache → workspace.json（都是精确值）。
            const auto it = metaByDir.constFind(info.id);
            if (it != metaByDir.constEnd()) {
                info.cwd = it->cwd;
                info.title = it->title;
                info.turns = it->turns;
            }
            if (info.cwd.isEmpty())
                info.cwd = cwdByDir.value(info.id);
            info.cwdExact = !info.cwd.isEmpty();
            if (info.cwdExact)
                exactCwdByWs.insert(ws, info.cwd);

            // 体积取会话正文文件（zstd 压缩，内容不可在此解析）。
            const QFileInfo body(fi.absoluteFilePath()
                                 + QStringLiteral("/session.jsonl.zstd"));
            info.sizeBytes = body.exists() ? body.size() : 0;
            if (info.sizeBytes == 0) {
                // 兼容未来可能的非压缩/改名：取目录下最大文件。
                const QFileInfoList files =
                    QDir(fi.absoluteFilePath()).entryInfoList(QDir::Files);
                for (const QFileInfo &f : files)
                    info.sizeBytes = qMax(info.sizeBytes, f.size());
            }
            out.append(info);
        }
    }

    // 缓存里没有的会话（手工拷入等）：先借同工作区已确证的 cwd，再退到目录名反解。
    // 反解不可逆（见 decodeWorkspaceDir），故只有它标 cwdExact=false。
    for (SessionInfo &info : out) {
        if (info.cwdExact)
            continue;
        const auto borrowed = exactCwdByWs.constFind(info.workspace);
        if (borrowed != exactCwdByWs.constEnd()) {
            info.cwd = *borrowed;
            info.cwdExact = true;
        } else {
            info.cwd = decodeWorkspaceDir(info.workspace);
        }
    }

    // 新 → 旧
    std::sort(out.begin(), out.end(), [](const SessionInfo &a, const SessionInfo &b) {
        return a.modified > b.modified;
    });
    return out;
}

bool DshManager::deleteSession(const QString &workspace, const QString &id,
                              QString *errorOut)
{
    if (workspace.isEmpty() || id.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("会话标识不完整。");
        return false;
    }
    // 防目录穿越：两段都必须是单层名字。
    if (workspace.contains(QLatin1Char('/')) || id.contains(QLatin1Char('/'))) {
        if (errorOut)
            *errorOut = QStringLiteral("非法的会话路径。");
        return false;
    }
    // id 是 SessionInfo::id，本身含 "session-" 前缀（= 目录名），这里不再拼。
    // 校验前缀既是防呆，也把删除范围锁在会话目录上。
    if (!id.startsWith(QStringLiteral("session-"))) {
        if (errorOut)
            *errorOut = QStringLiteral("不是会话目录名：%1").arg(id);
        return false;
    }
    const QString path = QDir(sessionsDir())
                             .filePath(workspace + QLatin1Char('/') + id);
    QDir dir(path);
    if (!dir.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("会话目录不存在：%1").arg(path);
        return false;
    }
    if (!dir.removeRecursively()) {
        if (errorOut)
            *errorOut = QStringLiteral("删除失败：%1").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// settings.yaml 读写（纯文本，显式 UTF-8）
// ---------------------------------------------------------------------------

QString DshManager::readSettings(QString *errorOut)
{
    QFile f(settingsPath());
    if (!f.exists())
        return QString(); // 尚未生成，视为空内容
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法读取 %1").arg(settingsPath());
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

bool DshManager::writeSettings(const QString &content, QString *errorOut)
{
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入 %1").arg(path);
        return false;
    }
    const QByteArray utf8 = content.toUtf8();
    const qint64 n = f.write(utf8);
    f.close();
    if (n != utf8.size()) {
        if (errorOut)
            *errorOut = QStringLiteral("写入不完整：%1").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 环境检测
// ---------------------------------------------------------------------------

// 登录 shell 的完整 PATH（GUI 启动 PATH 不含 nvm/fnm 时的兜底）。
// 与 ClaudeCodeBackend::loginShellPath 同源。
// 只探一次并缓存（实测 ~0.9s，zsh 要加载 nvm 等初始化脚本，是环境检测里最贵的
// 一步）。用 C++11 的 "magic static" 而不是 probed+cached 两个静态量：本函数
// 现在会被工作线程（detectEnvironmentAsync）和 UI 线程（start/恢复会话等）同时
// 调到，函数局部静态的初始化才有线程安全保证——并发到达的调用会阻塞到初始化
// 完成，不会各探一次或读到半初始化的值。
QString DshManager::loginShellPath()
{
    static const QString cached = []() -> QString {
#ifndef Q_OS_WIN
        const QString shell = qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash"));
        QProcess proc;
        proc.start(shell, {QStringLiteral("-ilc"), QStringLiteral("printf %s \"$PATH\"")});
        if (proc.waitForFinished(5000))
            return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        proc.kill();
        proc.waitForFinished(1000);
#endif
        return QString();
    }();
    return cached;
}

// 依次查 进程 PATH -> 登录 shell PATH -> 常见版本管理器目录（nvm/fnm/n 取最新）。
QString DshManager::findNodeTool(const QString &name)
{
#ifdef Q_OS_WIN
    const QString exe = name + QStringLiteral(".exe");
    const QString cmd = name + QStringLiteral(".cmd"); // npm/npx 在 Windows 是 .cmd
#else
    const QString exe = name;
#endif

    // 1. 当前进程 PATH
    QString found = QStandardPaths::findExecutable(exe);
#ifdef Q_OS_WIN
    if (found.isEmpty())
        found = QStandardPaths::findExecutable(cmd);
#endif
    if (!found.isEmpty())
        return found;

    // 2. 登录 shell PATH
    const QString loginPath = loginShellPath();
    if (!loginPath.isEmpty()) {
        const QStringList dirs =
            loginPath.split(QDir::listSeparator(), Qt::SkipEmptyParts);
        found = QStandardPaths::findExecutable(exe, dirs);
#ifdef Q_OS_WIN
        if (found.isEmpty())
            found = QStandardPaths::findExecutable(cmd, dirs);
#endif
        if (!found.isEmpty())
            return found;
    }

    // 3. 常见安装目录 + 版本管理器路径（逆序取最新版本）
    const QString home = QDir::homePath();
    QStringList candidates = {
        QStringLiteral("/usr/local/bin/") + exe,
        QStringLiteral("/opt/homebrew/bin/") + exe,
        QStringLiteral("/usr/bin/") + exe,
        home + QStringLiteral("/.volta/bin/") + exe,
    };
    const QList<QPair<QString, QString>> globRoots = {
        {home + QStringLiteral("/.nvm/versions/node"), QStringLiteral("/bin/") + exe},
        {home + QStringLiteral("/.fnm/node-versions"), QStringLiteral("/installation/bin/") + exe},
        {QStringLiteral("/usr/local/n/versions/node"), QStringLiteral("/bin/") + exe},
    };
    for (const auto &root : globRoots) {
        QDir dir(root.first);
        const QStringList versions = dir.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const QString &v : versions)
            candidates.append(root.first + QLatin1Char('/') + v + root.second);
    }
    for (const QString &path : candidates) {
        const QFileInfo fi(path);
        if (fi.isFile() && fi.isExecutable())
            return QDir::cleanPath(path);
    }
    return QString();
}

// 从一段文本中提取首个 semver（容忍前导 v、预发布/构建元数据）。
// npm 常把 "npm warn ..." 一类警告写到 stderr；即便混入也能凭正则捞出真版本号。
static QString extractVersion(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(
        "v?\\d+\\.\\d+\\.\\d+(?:-[0-9A-Za-z.\\-]+)?(?:\\+[0-9A-Za-z.\\-]+)?"));
    const QRegularExpressionMatch m = re.match(text);
    if (m.hasMatch())
        return m.captured(0);
    return text.trimmed().section(QLatin1Char('\n'), 0, 0);
}

// 运行一个短命令并取其 stdout 里的版本号（用于 node/npm/dsh --version）。
// 用分离通道只读 stdout：npm 的 "Unknown user config" 等警告在 stderr，
// 合并通道会把它夹在版本号前面污染解析。
static QString probeVersion(const QString &program, const QStringList &args,
                            const QProcessEnvironment &env)
{
    if (program.isEmpty())
        return QString();
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(program, args);
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return QString();
    }
    return extractVersion(QString::fromUtf8(proc.readAllStandardOutput()));
}

DshManager::Environment DshManager::detectEnvironment()
{
    Environment env;
    env.npxPath = findNodeTool(QStringLiteral("npx"));
    env.npmPath = findNodeTool(QStringLiteral("npm"));
    const QString nodePath = findNodeTool(QStringLiteral("node"));

    // 版本探测需要 node 在 PATH 上：用 childEnvironment 注入 bin 目录。
    const QProcessEnvironment procEnv = childEnvironment(
        !env.npxPath.isEmpty() ? env.npxPath
                               : (!env.npmPath.isEmpty() ? env.npmPath : nodePath));
    env.nodeVersion = probeVersion(nodePath, {QStringLiteral("--version")}, procEnv);
    env.npmVersion = probeVersion(env.npmPath, {QStringLiteral("--version")}, procEnv);

    // 全局是否已装 dsh：能解析到 dsh 可执行文件即视为已全局安装；
    // 同时取其版本号（`dsh --version`）。
    const QString dshPath = findNodeTool(QStringLiteral("dsh"));
    env.dshGlobalInstalled = !dshPath.isEmpty();
    if (env.dshGlobalInstalled)
        env.dshVersion = probeVersion(dshPath, {QStringLiteral("--version")}, procEnv);
    return env;
}

bool DshManager::isDetectingEnvironment() const
{
    return m_envWatcher.isRunning();
}

// 把阻塞检测丢到全局线程池。detectEnvironment 是静态纯函数，lambda 不捕获
// this，所以即使本对象在检测完成前被销毁，工作线程里也没有悬垂引用
//（watcher 随之销毁，finished 不会再投递）。
void DshManager::detectEnvironmentAsync()
{
    if (m_envWatcher.isRunning())
        return; // 已有一次在跑，别重复起子进程
    m_envWatcher.setFuture(QtConcurrent::run([]() { return detectEnvironment(); }));
}

// ---------------------------------------------------------------------------
// 监听配置
// ---------------------------------------------------------------------------

void DshManager::setListen(const QString &host, int port)
{
    m_host = host.trimmed().isEmpty() ? QLatin1String(kDefaultHost) : host.trimmed();
    m_port = (port < 0 || port > 65535) ? kDefaultPort : port;
}

QString DshManager::webUrl() const
{
    const int p = (m_port > 0) ? m_port : kDefaultPort;
    return QStringLiteral("http://%1:%2").arg(m_host).arg(p);
}

// 构造子进程环境：不覆盖 DSH_HOME —— 让面板托管的 web 进程与终端里的
// dsh CLI 共用用户默认的 ~/.dsh（同一份 profile/会话/插件，才是“管理你的 dsh”）。
// 仅把工具所在 bin 目录前置到 PATH（npx 靠 shebang 找 node）。
QProcessEnvironment DshManager::childEnvironment(const QString &toolPath)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString pathKey =
#ifdef Q_OS_WIN
        QStringLiteral("Path"); // Windows 环境变量名大小写不敏感，但约定为 Path
#else
        QStringLiteral("PATH");
#endif

    // 子进程 PATH = 工具自身 bin 目录 + 本进程 PATH + 登录 shell PATH（去重保序）。
    //
    // 只前置 node 的 bin 目录是不够的：dsh 会再去 PATH 上找**别的**工具，最典型
    // 的是 pnpm——`dsh plugin add/remove` 只是把活转发给 profile 目录里的 pnpm，
    // 找不到就 code=127 退出（"pnpm not found on PATH"）。而 node 与 pnpm 常常
    // 装在不同地方（实测本机 node 在 ~/.nvm/versions/node/*/bin，pnpm 在
    // Homebrew 的 /opt/homebrew/bin），macOS GUI 应用的进程 PATH 又是最小集、
    // 两者都不含，于是只补 node 目录就会漏掉 pnpm。
    // 登录 shell PATH 是用户终端里真实可用的那份，并进来才能覆盖这类工具。
    // loginShellPath() 有缓存，且面板构造时的异步环境检测已在工作线程里预热过。
    QStringList dirs;
    const auto appendDirs = [&dirs](const QString &pathList) {
        const QStringList parts =
            pathList.split(QDir::listSeparator(), Qt::SkipEmptyParts);
        for (const QString &d : parts) {
            if (!dirs.contains(d))
                dirs.append(d);
        }
    };
    if (!toolPath.isEmpty())
        appendDirs(QFileInfo(toolPath).absolutePath());
    appendDirs(env.value(pathKey));
    appendDirs(loginShellPath());
    env.insert(pathKey, dirs.join(QDir::listSeparator()));
    return env;
}

// ---------------------------------------------------------------------------
// 进程启停
// ---------------------------------------------------------------------------

void DshManager::setStatus(Status status)
{
    if (m_status == status)
        return;
    m_status = status;
    qCInfo(dshLog) << "status ->" << int(status);
    emit statusChanged(status);
}

void DshManager::appendLog(const QByteArray &utf8Bytes)
{
    QFile f(logPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(utf8Bytes);
}

void DshManager::wireProcess(QProcess *proc)
{
    // 合并 stdout/stderr。
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QByteArray chunk = proc->readAllStandardOutput();
        appendLog(chunk);
        m_buf.append(chunk);
        int idx;
        while ((idx = m_buf.indexOf('\n')) >= 0) {
            const QByteArray lineBytes = m_buf.left(idx);
            m_buf.remove(0, idx + 1);
            QString line = QString::fromUtf8(lineBytes);
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            emit logOutput(line);
        }
    });

    connect(proc, &QProcess::started, this, [this, proc]() {
        emit started(proc->processId());
        // 进程起来了，但 web 服务还要 compose 插件层才就绪 → 开始健康检查。
        startHealthCheck();
    });

    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            stopHealthCheck();
            setStatus(Status::Failed);
            emit errorOccurred(QStringLiteral(
                "dsh 启动失败：无法执行 npx。请确认已安装 Node.js（含 npm/npx）。"));
        }
    });

    connect(proc, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                stopHealthCheck();
                if (!m_buf.isEmpty()) {
                    emit logOutput(QString::fromUtf8(m_buf));
                    m_buf.clear();
                }
                const bool crashed = (exitStatus == QProcess::CrashExit);
                const bool abnormal = !m_stopping && (crashed || exitCode != 0);
                setStatus(abnormal ? Status::Failed : Status::Stopped);
                if (abnormal) {
                    emit errorOccurred(QStringLiteral("dsh 异常退出(code=%1)，详见日志。")
                                           .arg(exitCode));
                }
                emit stopped(exitCode);
            });
}

void DshManager::startHealthCheck()
{
    m_healthAttempts = 0;
    m_healthTimer->start();
}

void DshManager::stopHealthCheck()
{
    m_healthTimer->stop();
    m_healthAttempts = 0;
}

bool DshManager::start(QString *errorOut)
{
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        return true; // 已在运行（幂等）

    const QString npx = findNodeTool(QStringLiteral("npx"));
    if (npx.isEmpty()) {
        setStatus(Status::Failed);
        const QString msg = QStringLiteral(
            "未找到 npx。请先安装 Node.js（https://nodejs.org，自带 npm/npx）。");
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(msg);
        return false;
    }

    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_proc = new QProcess(this);
    m_stopping = false;
    m_proc->setWorkingDirectory(dshDir());
    m_proc->setProcessEnvironment(childEnvironment(npx));
    wireProcess(m_proc);

    setStatus(Status::Starting);
    // -y：跳过 npx 的安装确认。web profile 的 --host/--port 传监听地址。
    const QStringList args = {
        QStringLiteral("-y"), kPackage, QStringLiteral("web"),
        QStringLiteral("--host"), m_host,
        QStringLiteral("--port"), QString::number(m_port),
    };
    qCInfo(dshLog) << "start:" << npx << args;
    m_proc->start(npx, args);
    if (!m_proc->waitForStarted(8000)) {
        setStatus(Status::Failed);
        const QString msg = QStringLiteral("dsh 启动超时（npx 首次解析/下载可能较慢）。");
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(msg);
        return false;
    }
    return true;
}

void DshManager::stop(int msecs)
{
    if (!m_proc)
        return;
    m_stopping = true;
    stopHealthCheck();
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->terminate(); // 先 SIGTERM，让 dsh 正常收尾
        if (!m_proc->waitForFinished(msecs))
            m_proc->kill();
        m_proc->waitForFinished(1000);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
    setStatus(Status::Stopped);
}

bool DshManager::isRunning() const
{
    return m_proc && m_proc->state() == QProcess::Running;
}

// ---------------------------------------------------------------------------
// 全局安装 / 更新（共用 npm 子进程）
// ---------------------------------------------------------------------------

void DshManager::runNpmGlobal(const QStringList &args, const QString &startMsg,
                              const QString &okMsg)
{
    if (m_installer)
        return; // 已有安装/更新在进行

    const QString npm = findNodeTool(QStringLiteral("npm"));
    if (npm.isEmpty()) {
        emit installFinished(false, QStringLiteral("未找到 npm，请先安装 Node.js。"));
        return;
    }

    m_installer = new QProcess(this);
    m_installer->setProcessChannelMode(QProcess::MergedChannels);
    m_installer->setProcessEnvironment(childEnvironment(npm));
    m_installer->setWorkingDirectory(dshDir());

    connect(m_installer, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString out = QString::fromUtf8(m_installer->readAllStandardOutput());
        const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines)
            emit installLog(line);
    });
    connect(m_installer, &QProcess::finished, this,
            [this, okMsg](int exitCode, QProcess::ExitStatus) {
                const bool ok = (exitCode == 0);
                emit installFinished(ok, ok ? okMsg
                                            : QStringLiteral("npm 操作失败(code=%1)。").arg(exitCode));
                m_installer->deleteLater();
                m_installer = nullptr;
            });

    emit installLog(startMsg);
    m_installer->start(npm, args);
}

void DshManager::installGlobal()
{
    runNpmGlobal({QStringLiteral("install"), QStringLiteral("-g"), kPackage},
                 QStringLiteral("执行: npm install -g %1 ...").arg(kPackage),
                 QStringLiteral("dsh 全局安装完成。"));
}

void DshManager::updateGlobal()
{
    // @latest 显式追到 latest 标签（dsh 处于预览期，版本迭代快）。
    runNpmGlobal({QStringLiteral("install"), QStringLiteral("-g"),
                  kPackage + QStringLiteral("@latest")},
                 QStringLiteral("执行: npm install -g %1@latest ...").arg(kPackage),
                 QStringLiteral("dsh 已更新到最新版本。"));
}

// ---------------------------------------------------------------------------
// latest 版本查询
// ---------------------------------------------------------------------------

void DshManager::checkLatestVersion()
{
    if (m_versionChecker)
        return; // 已在查询

    const QString npm = findNodeTool(QStringLiteral("npm"));
    if (npm.isEmpty()) {
        emit latestVersionChecked(false, QString());
        return;
    }

    m_versionChecker = new QProcess(this);
    // 分离通道：版本号在 stdout，npm 警告在 stderr，只读 stdout 避免污染。
    m_versionChecker->setProcessChannelMode(QProcess::SeparateChannels);
    m_versionChecker->setProcessEnvironment(childEnvironment(npm));
    m_versionChecker->setWorkingDirectory(dshDir());

    connect(m_versionChecker, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
                const QString out =
                    QString::fromUtf8(m_versionChecker->readAllStandardOutput());
                const QString version = extractVersion(out);
                const bool ok = (exitCode == 0) && !version.isEmpty();
                emit latestVersionChecked(ok, ok ? version : QString());
                m_versionChecker->deleteLater();
                m_versionChecker = nullptr;
            });

    // `npm view <pkg> version` 读 registry 的 latest 标签（走 npmrc 镜像配置）。
    m_versionChecker->start(npm, {QStringLiteral("view"), kPackage,
                                  QStringLiteral("version")});
}

// ---------------------------------------------------------------------------
// 插件安装 / 卸载（dsh plugin --profile <p> add|remove <pkg>，内部转发 pnpm）
// ---------------------------------------------------------------------------

void DshManager::runPluginOp(const QString &profile, const QString &verb,
                             const QString &package, const QString &okMsg)
{
    if (m_pluginProc)
        return; // 已有插件操作在进行

    if (profile.trimmed().isEmpty() || package.trimmed().isEmpty()) {
        emit pluginOpFinished(false, QStringLiteral("profile 与包名都不能为空。"));
        return;
    }

    // 优先用全局 dsh；没装则退回 npx 拉起（与 start() 的策略一致）。
    QString program = findNodeTool(QStringLiteral("dsh"));
    QStringList args;
    if (!program.isEmpty()) {
        args = {QStringLiteral("plugin"), QStringLiteral("--profile"), profile,
                verb, package};
    } else {
        program = findNodeTool(QStringLiteral("npx"));
        if (program.isEmpty()) {
            emit pluginOpFinished(false, QStringLiteral(
                "未找到 dsh 或 npx，请先安装 Node.js。"));
            return;
        }
        args = {QStringLiteral("-y"), kPackage, QStringLiteral("plugin"),
                QStringLiteral("--profile"), profile, verb, package};
    }

    m_pluginProc = new QProcess(this);
    m_pluginProc->setProcessChannelMode(QProcess::MergedChannels);
    m_pluginProc->setProcessEnvironment(childEnvironment(program));
    m_pluginProc->setWorkingDirectory(dshDir());

    // dsh 找不到 pnpm 时只会退 code=127，光报退出码用户不知道要装什么。
    // 这里嗅一下输出里的 pnpm 缺失标记，失败时把处置办法一起报出去。
    // 匹配必须是**连续**子串：pnpm 自己的报错里有 "ERR_PNPM_FETCH_404 ... Not
    // Found - 404" 这种行，同时含 "pnpm" 与 "not found" 两个词——按「都出现即算」
    // 去判，装不存在的包时会误报成缺 pnpm，给出完全错的处置建议（实测踩到）。
    // 下面三条分别是 dsh 自己的原话、以及 shell 报命令缺失的两种常见措辞。
    // 用 shared_ptr 而不是成员变量：状态只属于这一次操作，随两个 lambda 一起销毁。
    auto pnpmMissing = std::make_shared<bool>(false);

    connect(m_pluginProc, &QProcess::readyReadStandardOutput, this, [this, pnpmMissing]() {
        const QString out = QString::fromUtf8(m_pluginProc->readAllStandardOutput());
        const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            if (line.contains(QLatin1String("pnpm not found"), Qt::CaseInsensitive)
                || line.contains(QLatin1String("pnpm: command not found"), Qt::CaseInsensitive)
                || line.contains(QLatin1String("pnpm: not found"), Qt::CaseInsensitive)) {
                *pnpmMissing = true;
            }
            emit pluginLog(line);
        }
    });
    connect(m_pluginProc, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart) {
                    emit pluginOpFinished(false, QStringLiteral("无法执行 dsh plugin 命令。"));
                    m_pluginProc->deleteLater();
                    m_pluginProc = nullptr;
                }
            });
    connect(m_pluginProc, &QProcess::finished, this,
            [this, okMsg, pnpmMissing](int exitCode, QProcess::ExitStatus) {
                const bool ok = (exitCode == 0);
                QString msg = okMsg;
                if (!ok) {
                    msg = QStringLiteral("插件操作失败(code=%1)。").arg(exitCode);
                    if (*pnpmMissing) {
                        msg += QStringLiteral(
                            "dsh 的插件管理是转发给 pnpm 做的，但 PATH 上找不到 pnpm。"
                            "请先安装：npm install -g pnpm（macOS 也可 brew install pnpm），"
                            "然后点「刷新环境」再重试。");
                    }
                }
                emit pluginOpFinished(ok, msg);
                m_pluginProc->deleteLater();
                m_pluginProc = nullptr;
            });

    emit pluginLog(QStringLiteral("执行: dsh plugin --profile %1 %2 %3 ...")
                       .arg(profile, verb, package));
    m_pluginProc->start(program, args);
}

void DshManager::addPlugin(const QString &profile, const QString &package)
{
    runPluginOp(profile, QStringLiteral("add"), package,
                QStringLiteral("插件已安装：%1").arg(package.trimmed()));
}

void DshManager::removePlugin(const QString &profile, const QString &package)
{
    runPluginOp(profile, QStringLiteral("remove"), package,
                QStringLiteral("插件已卸载：%1").arg(package.trimmed()));
}

} // namespace cubeshell
