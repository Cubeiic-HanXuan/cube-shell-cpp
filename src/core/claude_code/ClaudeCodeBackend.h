#pragma once

// ClaudeCodeBackend.h — Claude Code CLI 集成与历史会话访问（本地 / 远程）。
// 对应Python: core/claude_code/backend.py（LocalBackend / RemoteBackend）
//
// 协议说明：命令与聊天通过 claude CLI 驱动，不使用本地 / TCP 套接字服务。
//   - 一次性命令：claude <args>，标准输出为 JSON 或文本；
//   - 远程执行：claude <args>; echo "__EXIT_CODE__$?"，通过哨兵提取退出码；
//   - 流式聊天：claude -p <prompt> --output-format stream-json，逐行解析 JSON；
//   - 历史会话：直接读取 Claude 配置目录下 projects/<proj>/<sessionId>.jsonl，
//     由 ClaudeSessionStore 负责本地 / SSH 分页，与 CLI 可用性相互独立。
//
// - run_command        对应Python: LocalBackend/RemoteBackend.run_command
// - version/auth/daemon对应Python: get_version/get_auth_status/get_daemon_status
// - sessions           对应Python: list_sessions，C++ 扩展为独立存储与增量快照
// - settings/MCP       对应Python: read/write_settings + read/write_mcp_config
//
// 线程约定：界面线程提交异步任务，refresh* 的结果可能从工作线程发出，
// 界面消费端使用 Qt::QueuedConnection；聊天生命周期信号在本对象线程发出。
// 会话任务提交时固定 store / source，析构前取消读取并等待 m_futures 收尾。

#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include "claude_code/ClaudeSessionStore.h"
#include "claude_code/ClaudeSkillStore.h"

class QProcess;
class QProcessEnvironment;

namespace cubeshell {

class CommandExecutor;

// 对应Python: run_command 的 (returncode, stdout, stderr) 三元组
struct ClaudeRunResult {
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
};


class ClaudeCodeBackend : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 30 * 1000; // 对应Python: timeout=30

    explicit ClaudeCodeBackend(QObject *parent = nullptr);
    ~ClaudeCodeBackend() override;

    // 远程 CLI / 配置操作使用 executor；历史读取使用 client 建立独立命令通道。
    // 对应Python: RemoteBackend(ssh_conn)；nullptr 切回本地，同时更换会话存储实例。
    void setRemoteExecutor(CommandExecutor *executor, std::shared_ptr<SshClient> client = {});
    bool isRemote() const { return !m_executor.isNull(); }
    quint64 sessionSource() const { return m_sessionSource; }
    // 存储来源不随 QPointer 失效而变成本地，避免连接断开后误恢复到本机。
    bool sessionSourceIsRemote() const { return m_sessionStore->isRemote(); }
    CommandExecutor *remoteExecutor() const;
    // 异步读取一页并在工作线程复制 / 更新快照，通过 sessionPageReady 返回。
    void readSessionPage(const ClaudeSessionInfo &session, qint64 offset, quint64 request,
                         const ClaudeSessionSnapshot &snapshot);
    // 独立后台导出，不依赖详情页当前已加载的内容，结果通过 sessionExported 返回。
    void exportSession(const ClaudeSessionInfo &session, const QString &destination,
                       bool markdown, quint64 request);

    // Skills 文件操作固定提交时的数据源；与会话共用来源代次，结果必须匹配 request。
    void refreshSkills(const ClaudeSkillScope &scope, quint64 request);
    void readSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, quint64 request);
    void createSkill(const ClaudeSkillScope &scope, const QString &name, const QByteArray &content, quint64 request);
    void saveSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, const QByteArray &content, quint64 request);
    void importSkill(const ClaudeSkillScope &scope, const QString &name, const QString &path, quint64 request);
    void exportSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, const QString &path, quint64 request);
    void setSkillEnabled(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, bool enabled, quint64 request);
    void removeSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, quint64 request);

    // --- 阻塞接口（耗时操作应由工作线程调用） ---

    // 对应Python: run_command(远程带 __EXIT_CODE__$? 哨兵)
    ClaudeRunResult runCommand(const QStringList &args,
                               int timeoutMs = kDefaultTimeoutMs);
    // 对应Python: get_version(claude --version)
    QString version();
    // 对应Python: get_auth_status(--json 优先,--text 回退)
    QJsonObject authStatus();
    // 对应Python: get_daemon_status(claude daemon status)
    QJsonObject daemonStatus();
    // 直接读取已保存历史，不依赖 CLI 可用性；按文件最近活跃时间排序。
    QList<ClaudeSessionInfo> listSessions();

    // 对应Python: read_settings / write_settings(~/.claude/settings.json)
    QJsonObject readSettings();
    bool writeSettings(const QJsonObject &settings, QString *errorOut = nullptr);
    // 对应Python: read_mcp_config / write_mcp_config
    // scope: "user" -> ~/.claude.json, "project" -> <projectPath>/.mcp.json
    QJsonObject readMcpConfig(const QString &scope = QStringLiteral("user"),
                              const QString &projectPath = QString());
    bool writeMcpConfig(const QJsonObject &config,
                        const QString &scope = QStringLiteral("user"),
                        const QString &projectPath = QString(),
                        QString *errorOut = nullptr);

    // --- 异步包装（QtConcurrent 调度，结果可能从工作线程发出） ---

    // 发出 statusLoaded(version, auth, daemon, binPath)。
    // 对应Python: status_widget.py::StatusWorker.run（行 44-69）
    void refreshStatus();
    // 发出 sessionListReady(source, request, sessions, error)；request 由界面递增以识别过期结果。
    void refreshSessions(quint64 request = 0);
    // 后台执行 `claude update`，完成后 emit updateFinished(output)。
    // 对应Python: status_widget.py::UpdateWorker.run（行 25-32）
    void refreshUpdate();
    // Emits settingsLoaded(settings)。对应Python: settings_widget.py::SettingsWorker(load)
    void refreshSettings();
    // Emits settingsSaved(ok, message)。对应Python: settings_widget.py::SettingsWorker(save)
    void saveSettings(const QJsonObject &settings);
    // Emits mcpConfigLoaded(config)。对应Python: mcp_widget.py::McpWorker(load)
    void refreshMcpConfig(const QString &scope, const QString &projectPath);
    // Emits mcpConfigSaved(ok, message)。对应Python: mcp_widget.py::McpWorker(save)
    void saveMcpConfig(const QJsonObject &config, const QString &scope,
                       const QString &projectPath);

    // 安装路径卡片文案：本地为 claude 二进制完整路径，远程固定 "claude (远程)"。
    // 对应Python: status_widget.py::StatusWorker.run 行 57-65
    QString binPath();

    // --- 流式聊天（逐行 JSON 协议） ---

    // 执行 claude -p <prompt> --output-format stream-json --verbose，
    // 指定会话 ID 时追加 --resume。每行 JSON 经 chatMessage() 发出。
    // 同时只允许一个聊天，必须在本对象所属线程调用。
    // 对应Python: claude CLI 的 stream-json 输出协议
    bool startChat(const QString &prompt, const QString &cwd = QString(),
                   const QString &resumeSessionId = QString());
    void abortChat();
    bool isChatting() const;

    // --- pure helpers (unit-testable) ---

    // 对应Python: _parse_daemon_status
    static QJsonObject parseDaemonStatus(int exitCode, const QString &stdoutText);
    // 对应Python: _parse_auth_text(key: value 行 -> dict)
    static QJsonObject parseAuthText(const QString &text);
    // 对应Python: _parse_transcript_head(头部 80 行提取标题/cwd/时间戳)
    static void parseTranscriptHead(const QString &content, QString *nameOut,
                                    QString *cwdOut, qint64 *firstTsMsOut,
                                    int maxLines = 80);
    // 对应Python: RemoteBackend.run_command 的哨兵解析
    static ClaudeRunResult parseExitCodeSentinel(const QString &output);
    // 构造 "切到 cwd 再执行 command" 的终端命令（POSIX: cd 'dir' && cmd；
    // Windows PowerShell: Set-Location -LiteralPath 'dir'; if ($?) { cmd }）。
    // 对应Python: backend.py::build_cd_command（行 20-36）
    static QString buildCdCommand(const QString &cwd, const QString &command);
    // 官方安装脚本命令（macOS/Linux: curl ... install.sh | bash；
    // Windows: irm ... install.ps1 | iex）。
    // 对应Python: backend.py::build_install_command（行 39-52）
    static QString buildInstallCommand();

signals:
    void skillsListed(quint64 source, quint64 request, const QList<cubeshell::ClaudeSkillInfo> &skills,
                      const QString &root, const QString &error);
    void skillLoaded(quint64 source, quint64 request, const cubeshell::ClaudeSkillInfo &skill,
                     const QStringList &files, const QString &error);
    void skillOperationFinished(quint64 source, quint64 request, const QString &error);
    // 以下异步结果可能从工作线程发出，界面连接使用 Qt::QueuedConnection。
    void statusLoaded(const QString &version, const QJsonObject &auth,
                      const QJsonObject &daemon, const QString &binPath);
    // 来源切换在界面线程通知；子面板立即清除旧内容并作废原请求。
    void sessionSourceChanged();
    // source 标识存储来源，request 标识调用方请求；两者必须同时匹配才能更新界面。
    void sessionListReady(quint64 source, quint64 request,
                          const QList<cubeshell::ClaudeSessionInfo> &sessions, const QString &error);
    // page 只携带进度 / 错误信息，原始 bytes 已释放，解析结果在只读 snapshot 中。
    void sessionPageReady(quint64 source, quint64 request, const cubeshell::ClaudeSessionPage &page,
                          const cubeshell::ClaudeSessionSnapshot &snapshot);
    void sessionExported(quint64 source, quint64 request, const QString &destination, const QString &error);
    // claude update 输出（stdout 优先，空则 stderr）。
    // 对应Python: status_widget.py::UpdateWorker.finished
    void updateFinished(const QString &output);
    // settings.json / MCP 配置的异步读写结果。
    void settingsLoaded(const QJsonObject &settings);
    void settingsSaved(bool ok, const QString &message);
    void mcpConfigLoaded(const QJsonObject &config);
    void mcpConfigSaved(bool ok, const QString &message);
    // 聊天生命周期信号在本对象所属线程发出。
    void chatMessage(const QJsonObject &message);
    void chatFinished(int exitCode);
    void chatError(const QString &message);

private:
    void schedule(std::function<void()> job);
    void skillOperation(quint64 request, std::function<void(ClaudeSkillStore &, QString *)> operation);
    // 把 settings.json 的 env 字段注入本地子进程环境（ANTHROPIC_AUTH_TOKEN/
    // ANTHROPIC_API_KEY 及 ANTHROPIC_DEFAULT_*_MODEL 等约定见
    // settings_widget.py::_build_settings_dict 行 347-396）。
    void applySettingsEnv(QProcessEnvironment *env);
    void feedChatBuffer(const QByteArray &chunk);
    void flushChatBuffer();
    QString findClaudeBin() const;
    QString loginShellPath() const;
    QString shellQuote(const QString &s) const;
    QString claudeHome() const;

    // 非拥有指针；executor 随 SshSessionTab 析构，QPointer 自动置空。
    // CLI / 配置路径沿用原有判空逻辑；历史存储独立记录来源，失联时报告错误而非读取本地。
    QPointer<CommandExecutor> m_executor;
    QProcess *m_chatProcess = nullptr;     // local chat stream
    bool m_remoteChatActive = false;
    QByteArray m_chatBuffer;
    mutable QString m_cachedBin;
    mutable QString m_cachedLoginPath;
    mutable bool m_loginPathProbed = false;
    quint64 m_sessionSource = 1; // 每次切换数据源递增，与异步结果一起回传
    std::shared_ptr<ClaudeSkillStore> m_skillStore;
    std::shared_ptr<ClaudeSessionStore> m_sessionStore; // 任务捕获共享引用，切换模式不破坏在途存储
    std::shared_ptr<std::atomic<quint64>> m_sessionReadRevision = std::make_shared<std::atomic<quint64>>(0); // 新分页请求淘汰旧读取
    QVector<QFuture<void>> m_futures; // 析构前等待任务结束，保证捕获的 this 不悬挂
};

} // namespace cubeshell
