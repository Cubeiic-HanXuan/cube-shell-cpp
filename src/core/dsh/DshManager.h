#pragma once

// DshManager.h — DeepSeek Harness（dsh）本地管理器。
//
// DeepSeek Harness 是 DeepSeek 开源的 AI agent harness（npm: @deepseek-ai/dsh，
// 开发者预览版）。它以 web profile 提供一个浏览器管理界面：
//     npx -y @deepseek-ai/dsh web --host 127.0.0.1 --port 3080
// 默认监听 http://127.0.0.1:3080。
//
// 本类仿 forwarder/FrpManager 的设计：用 QProcess 托管这个长驻子进程——
// 生命周期可控、可采集日志、可感知退出码；stop() 走 terminate -> kill 两段式。
// 与 FrpManager 的差异：
//   * 只托管一个进程（dsh web），不是 frpc/frps 两个。
//   * 增加端口健康检查：进程启动后 web 服务要compose插件层才就绪，
//     用 QTcpSocket 轮询 host:port，连通后 emit webReady()。
//   * 增加 Node 环境检测（node/npm/npx 是否可用、版本、全局是否已装 dsh）。
//
// 平台门控：依赖本机 exec 与 Node.js，整模块在 CUBESHELL_WITH_LOCALPROC 下
// 编译（鸿蒙沙箱禁止 exec，UI 入口由同名宏摘除）。
//
// GUI 进程的 PATH 坑：macOS 下图形应用不继承 shell 的 PATH，nvm/fnm 装的
// node/npm/npx 不在其列。findNodeTool() 依次查 进程 PATH -> 登录 shell PATH ->
// 常见版本管理器目录（与 ClaudeCodeBackend::findClaudeBin 同源）。启动子进程时
// 把解析到的工具所在 bin 目录 prepend 到子进程 PATH——npx 是带
// `#!/usr/bin/env node` shebang 的脚本，得靠 PATH 找到 node 才能跑。
//
// 线程模型：本类与其 QProcess/QTcpSocket 同线程（Qt 信号本就同线程投递）。
// 唯一例外是环境检测：detectEnvironment() 要起 4 个子进程，其中登录 shell 取
// PATH 一项实测就近 1 秒（zsh 要加载 nvm 初始化），放 UI 线程会把面板冻住。
// 故提供 detectEnvironmentAsync()：在 QtConcurrent 线程池里跑，完成后发
// environmentDetected。detectEnvironment() 本身是静态纯函数（只读文件系统 +
// 起子进程，不碰成员），所以可安全在工作线程执行。

#include <QDateTime>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTcpSocket;
class QTimer;

namespace cubeshell {

class DshManager : public QObject {
    Q_OBJECT
public:
    // dsh web 默认监听端口（README；--port 0 表示让 OS 选空闲端口）。
    static constexpr int kDefaultPort = 3080;
    static constexpr const char *kDefaultHost = "127.0.0.1";

    // 进程运行状态。
    enum class Status {
        Stopped,    // 未运行
        Starting,   // 已发起启动，等待进程与 web 服务就绪
        Running,    // web 服务已就绪（健康检查通过）
        Failed,     // 启动失败或异常退出
    };
    Q_ENUM(Status)

    // Node 环境检测结果（detectEnvironment 返回）。
    struct Environment {
        QString nodeVersion;          // "v24.13.0"；未找到为空
        QString npmVersion;           // "11.6.2"；未找到为空
        QString npxPath;              // 解析到的 npx 绝对路径；未找到为空
        QString npmPath;              // 解析到的 npm 绝对路径；未找到为空
        bool dshGlobalInstalled = false; // 全局是否已 `npm i -g @deepseek-ai/dsh`
        QString dshVersion;           // 已全局安装的 dsh 版本；未安装为空
        // 能否经 npx 启动 dsh（无需全局安装）。
        bool canRun() const { return !npxPath.isEmpty(); }
    };

    explicit DshManager(QObject *parent = nullptr);
    ~DshManager() override;

    // --- 路径 ---
    // cube-shell 托管的 dsh 目录（自动创建），仅放本管理器自己的日志（dsh.log）。
    // 注意：不覆盖子进程的 DSH_HOME —— web 进程与终端 dsh CLI 共用用户默认的
    // ~/.dsh（同一份 profile/会话/插件），保证面板与 CLI 看到的是同一个 dsh。
    static QString dshDir();
    static QString logPath();

    // 用户的 dsh home（$DSH_HOME，未设置则 ~/.dsh）。profile/会话/设置都在其下。
    static QString dshHome();
    static QString settingsPath();   // <home>/settings.yaml
    static QString profilesDir();    // <home>/profiles
    static QString sessionsDir();    // <home>/sessions
    static QString storagesDir();    // <home>/storages（dsh 自己的元信息缓存）

    // --- profile / 插件 / 会话（同步读，均为本地目录/文件扫描） ---

    // 已存在的 profile 名（<home>/profiles 下的目录，排除 node_modules）。
    static QStringList listProfiles();

    // profile 里已装的一个插件（package.json 的 dependencies 项）。
    struct PluginInfo {
        QString name;
        QString version;
    };
    // 读 <profiles>/<profile>/package.json 的 dependencies 得到已装插件。
    // 只取显式依赖（等价 pnpm list 的 dependencies，不含传递依赖），
    // 无需起子进程，面板切换 profile 时即时可得。
    static QList<PluginInfo> listPlugins(const QString &profile);
    // profile 的 dsh.profile.bundles（组成该 profile 的 bundle 列表）。
    static QStringList profileBundles(const QString &profile);

    // 一条历史会话。会话正文（session.jsonl.zstd）是 zstd 压缩的，这里不解压；
    // 但 dsh 在 <home>/storages/ 下自带元信息缓存，标题与工作目录可无损取到。
    struct SessionInfo {
        // dsh 的会话 id，**含 "session-" 前缀**（如 session-2d418edb-…）。
        // 这就是磁盘目录名、storages 的键、workspace.json sessionIds 的元素，
        // 也是 `--resume` 要的实参：去掉前缀传裸 uuid 会报 "session not found"
        // （实测：带前缀在任意目录都能恢复，裸 uuid 在任意目录都失败）。
        QString id;
        QString workspace;   // 工作区目录名（形如 --Users-hanxuan-deepseek--）
        // 会话所属工作目录的真实路径。恢复时把终端开在这里，好让 agent 的文件
        // 操作落在会话原本的项目目录上（`--resume` 本身不挑目录）。
        QString cwd;
        bool cwdExact = false; // true=取自 storages 缓存；false=目录名反解的近似值
        QString title;       // dsh 生成的会话标题（空=未命名/无缓存）
        int turns = -1;       // 对话轮次；0=空会话，-1=缓存里没有这条会话
        QDateTime modified;
        qint64 sizeBytes = 0;
    };
    // 扫描 <home>/sessions/<workspace>/session-*/，按修改时间新→旧排序。
    // 同时读 storages 缓存补齐 cwd/title/turns。
    static QList<SessionInfo> listSessions();
    // 删除一条会话（整个 session-<uuid> 目录）。id 为 SessionInfo::id（含前缀）。
    static bool deleteSession(const QString &workspace, const QString &id,
                              QString *errorOut = nullptr);
    // 工作区目录名 → 近似路径（--Users-hanxuan-deepseek-- → /Users/hanxuan/deepseek）。
    // 编码把 '/' 换成 '-'，不可逆：路径本身含 '-' 时反解必错。仅当 storages
    // 缓存里查不到该会话时兜底，结果在 SessionInfo 里标 cwdExact=false。
    static QString decodeWorkspaceDir(const QString &encoded);

    // --- settings.yaml 读写（纯文本，不引 YAML 库） ---
    static QString readSettings(QString *errorOut = nullptr);
    static bool writeSettings(const QString &content, QString *errorOut = nullptr);

    // --- 环境检测 ---
    // 阻塞版：起 4 个子进程，实测 ~1.1s（其中登录 shell 取 PATH 约 0.9s）。
    // **不要在 UI 线程调用**，会明显卡顿。静态纯函数，可在工作线程执行。
    static Environment detectEnvironment();
    // 非阻塞版：把上面这坨丢到 QtConcurrent 线程池，完成后发 environmentDetected。
    // 已有一次检测在跑时重复调用会被忽略（幂等）。
    void detectEnvironmentAsync();
    bool isDetectingEnvironment() const;

    // --- 监听配置 ---
    void setListen(const QString &host, int port);
    QString host() const { return m_host; }
    int port() const { return m_port; }
    // web 界面地址（端口 0 时按默认端口展示，实际端口以健康检查/日志为准）。
    QString webUrl() const;

    // --- 进程启停（QProcess 托管） ---
    // 启动 `npx -y @deepseek-ai/dsh web --host <host> --port <port>`。
    // 已在运行则直接返回 true（幂等）。
    bool start(QString *errorOut = nullptr);
    // 停止（terminate -> kill 两段式，等待 msecs）。
    void stop(int msecs = 4000);
    bool isRunning() const;
    Status status() const { return m_status; }

    // --- 全局安装 / 更新（`npm install -g`） ---
    // 全局安装 dsh（未安装时）。异步执行，过程日志经 installLog()、结果经
    // installFinished() 回报。
    void installGlobal();
    // 更新 dsh 到 latest 标签（已安装时）。同样经 installLog()/installFinished()。
    void updateGlobal();
    bool isInstalling() const { return m_installer != nullptr; }

    // --- 版本查询 ---
    // 异步查询 npm registry 上 dsh 的 latest 版本（`npm view <pkg> version`，
    // 走用户 npmrc 配置的 registry/镜像）。结果经 latestVersionChecked() 回报。
    void checkLatestVersion();

    // --- 插件安装 / 卸载（`dsh plugin --profile <p> add|remove <pkg>`） ---
    // dsh 把 plugin 子命令转发给 profile 目录里的 pnpm。异步执行，过程日志经
    // pluginLog()、结果经 pluginOpFinished() 回报。
    void addPlugin(const QString &profile, const QString &package);
    void removePlugin(const QString &profile, const QString &package);
    bool isPluginBusy() const { return m_pluginProc != nullptr; }

    // 解析 node 家族可执行文件（node/npm/npx/dsh）。供环境检测与测试复用。
    static QString findNodeTool(const QString &name);

signals:
    void statusChanged(cubeshell::DshManager::Status status);
    // 进程已启动（pid 可用）。
    void started(qint64 pid);
    // 进程已退出。
    void stopped(int exitCode);
    // web 服务健康检查通过，可在浏览器打开 url。
    void webReady(const QString &url);
    // 进程输出的一行日志（UTF-8 解码，已去掉行尾换行）。
    void logOutput(const QString &line);
    // 启动/运行错误。
    void errorOccurred(const QString &message);
    // 全局安装/更新过程与结果。
    void installLog(const QString &line);
    void installFinished(bool ok, const QString &message);
    // latest 版本查询结果。ok=false 时 latestVersion 为空（多为网络/registry 问题）。
    void latestVersionChecked(bool ok, const QString &latestVersion);
    // 插件安装/卸载过程与结果。
    void pluginLog(const QString &line);
    void pluginOpFinished(bool ok, const QString &message);
    // detectEnvironmentAsync() 的结果（在 UI 线程投递）。
    void environmentDetected(const cubeshell::DshManager::Environment &env);

private:
    void setStatus(Status status);
    void wireProcess(QProcess *proc);
    void startHealthCheck();
    void stopHealthCheck();
    // 追加写日志文件（显式 UTF-8 字节，不做本地编码转换）。
    static void appendLog(const QByteArray &utf8Bytes);
    // 构造子进程环境：PATH = 工具 bin 目录 + 进程 PATH + 登录 shell PATH（去重）。
    // 并入登录 shell PATH 是必需的：dsh 还要在 PATH 上找 pnpm（plugin 子命令转发
    // 给它），而 pnpm 常与 node 不在同一目录。不覆盖 DSH_HOME，沿用默认 ~/.dsh。
    static QProcessEnvironment childEnvironment(const QString &toolPath);
    // 登录 shell 的完整 PATH（GUI 启动 PATH 不完整时兜底）。
    static QString loginShellPath();
    // installGlobal/updateGlobal 共用的 npm 子进程启动（占用 m_installer）。
    void runNpmGlobal(const QStringList &args, const QString &startMsg,
                      const QString &okMsg);
    // addPlugin/removePlugin 共用的 dsh plugin 子进程启动（占用 m_pluginProc）。
    void runPluginOp(const QString &profile, const QString &verb,
                     const QString &package, const QString &okMsg);

    QString m_host = QLatin1String(kDefaultHost);
    int m_port = kDefaultPort;

    QProcess *m_proc = nullptr;
    Status m_status = Status::Stopped;
    // 异步环境检测。QFutureWatcher 在 UI 线程把结果转成 environmentDetected。
    QFutureWatcher<Environment> m_envWatcher;
    // 主动停止标记：terminate/kill 导致的退出不算异常退出（不报 Failed）。
    bool m_stopping = false;
    // 行缓冲（进程输出可能在任意字节处切断）。
    QByteArray m_buf;

    // 端口健康检查（Starting 后轮询，连通即 webReady）。
    QTimer *m_healthTimer = nullptr;
    int m_healthAttempts = 0;

    // 全局安装/更新子进程（与主进程相互独立）。
    QProcess *m_installer = nullptr;
    // latest 版本查询子进程。
    QProcess *m_versionChecker = nullptr;
    // 插件安装/卸载子进程。
    QProcess *m_pluginProc = nullptr;
};

} // namespace cubeshell
