#pragma once

// ClaudeCodeBackend.h — Claude Code CLI integration (local & remote).
// 对应Python: core/claude_code/backend.py（LocalBackend / RemoteBackend）
//
// Protocol note (confirmed from the Python source): Claude Code is driven
// through the `claude` CLI — NOT a local/tcp socket.
//   - one-shot commands:   claude <args>          (JSON or text stdout)
//   - remote execution:    claude <args>; echo "__EXIT_CODE__$?"  哨兵取退出码
//   - chat streaming:      claude -p <prompt> --output-format stream-json
//                          (JSON-line protocol: one JSON object per line)
//   - session transcripts: ~/.claude/projects/<proj>/<sessionId>.jsonl
//
// - run_command        对应Python: LocalBackend/RemoteBackend.run_command
// - version/auth/daemon对应Python: get_version/get_auth_status/get_daemon_status
// - sessions           对应Python: list_sessions(_read_transcript_sessions)
// - settings/MCP       对应Python: read/write_settings + read/write_mcp_config
//
// Blocking methods are worker-thread safe. The async refresh*/startChat API
// emits signals on this object's thread (remote stream chunks are re-routed
// via Qt::QueuedConnection internally).

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

// 对应Python: list_sessions 返回的 session dict
struct ClaudeSessionInfo {
    QString sessionId;
    QString name;
    QString cwd;
    QString status;     // saved / running / ...
    bool running = false;
    qint64 startedAtMs = 0;
};

class ClaudeCodeBackend : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 30 * 1000; // 对应Python: timeout=30

    explicit ClaudeCodeBackend(QObject *parent = nullptr);
    ~ClaudeCodeBackend() override;

    // Remote mode: all CLI/file operations run over the executor.
    // 对应Python: RemoteBackend(ssh_conn);nullptr 回到 LocalBackend 行为
    void setRemoteExecutor(CommandExecutor *executor);
    bool isRemote() const { return !m_executor.isNull(); }

    // --- blocking API (worker-thread safe) ---

    // 对应Python: run_command(远程带 __EXIT_CODE__$? 哨兵)
    ClaudeRunResult runCommand(const QStringList &args,
                               int timeoutMs = kDefaultTimeoutMs);
    // 对应Python: get_version(claude --version)
    QString version();
    // 对应Python: get_auth_status(--json 优先,--text 回退)
    QJsonObject authStatus();
    // 对应Python: get_daemon_status(claude daemon status)
    QJsonObject daemonStatus();
    // 对应Python: list_sessions(transcript 扫描 + agents --json 叠加)
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

    // --- async wrappers (QtConcurrent; signals from worker threads) ---

    // Emits statusLoaded(version, auth, daemon, binPath).
    // 对应Python: status_widget.py::StatusWorker.run（行 44-69）
    void refreshStatus();
    // Emits sessionsLoaded(sessions).
    void refreshSessions();
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

    // --- chat streaming (JSON-line protocol) ---

    // Start `claude -p <prompt> --output-format stream-json --verbose`
    // (plus --resume <sessionId> when given). Each stdout line is one JSON
    // object emitted via chatMessage(). Only one chat at a time; must be
    // called on this object's thread.
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
    // Emitted from worker threads — connect with Qt::QueuedConnection.
    void statusLoaded(const QString &version, const QJsonObject &auth,
                      const QJsonObject &daemon, const QString &binPath);
    void sessionsLoaded(const QList<cubeshell::ClaudeSessionInfo> &sessions);
    // claude update 输出（stdout 优先，空则 stderr）。
    // 对应Python: status_widget.py::UpdateWorker.finished
    void updateFinished(const QString &output);
    // settings.json / MCP 配置的异步读写结果。
    void settingsLoaded(const QJsonObject &settings);
    void settingsSaved(bool ok, const QString &message);
    void mcpConfigLoaded(const QJsonObject &config);
    void mcpConfigSaved(bool ok, const QString &message);
    // Chat lifecycle (emitted on this object's thread).
    void chatMessage(const QJsonObject &message);
    void chatFinished(int exitCode);
    void chatError(const QString &message);

private:
    void schedule(std::function<void()> job);
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
    QList<ClaudeSessionInfo> readLocalTranscripts();
    QList<ClaudeSessionInfo> readRemoteTranscripts();
    QMap<QString, QString> liveAgentStatus();

    // not owned；executor 随 SshSessionTab 析构，QPointer 自动置空，
    // 所有远程分支判空后回退本地（对应Python: RemoteBackend 失效回退）。
    QPointer<CommandExecutor> m_executor;
    QProcess *m_chatProcess = nullptr;     // local chat stream
    bool m_remoteChatActive = false;
    QByteArray m_chatBuffer;
    mutable QString m_cachedBin;
    mutable QString m_cachedLoginPath;
    mutable bool m_loginPathProbed = false;
    QVector<QFuture<void>> m_futures; // joined in destructor
};

} // namespace cubeshell
