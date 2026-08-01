#pragma once

// HermesBackend.h — Hermes Agent data-access layer (local & remote).
// 对应Python: core/hermes/backend.py（LocalBackend / RemoteBackend）
//
// - hermes CLI execution    对应Python: exec_cli
// - file access             对应Python: read_file/write_file/list_dir/...
// - api server url/key      对应Python: get_api_server_url/get_api_server_key
//   (config.yaml is parsed text-level — no YAML library involved)
// - profile listing         对应Python: list_profiles
//
// Plus an HTTP/SSE client for the Hermes API server. Per the Phase 5 task
// contract this module ships its OWN SSE parser (SseParser below) instead of
// including the Phase 4 ai/SseClient which may still be under parallel
// development.
//
// Blocking methods (execCli/readFile/...) are safe to call from worker
// threads (HermesTaskModel/HermesGateway run them via QtConcurrent). The SSE
// streaming API must be used from the thread owning this object's event loop.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {

class CommandExecutor;

// Minimal incremental Server-Sent-Events parser (RFC-style framing):
//   - "data:" lines accumulate, "event:" sets the event name,
//   - an empty line dispatches the pending event,
//   - ":" comment lines are ignored, CR is tolerated.
// 内联实现,不依赖 Phase 4 的 ai/SseClient。
class SseParser {
public:
    struct Event {
        QString event; // empty -> default "message"
        QString data;
    };

    // Feed a raw network chunk; returns every event completed by this chunk.
    QList<Event> feed(const QByteArray &chunk);
    // Flush a trailing event that was never terminated by a blank line.
    QList<Event> finish();
    void reset();

private:
    QList<Event> consumeLines();
    QByteArray m_buffer;
    QString m_event;
    QStringList m_dataLines;
};

class HermesBackend : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultTimeoutMs = 30 * 1000; // 对应Python: timeout=30

    explicit HermesBackend(QObject *parent = nullptr);

    // Remote mode: all CLI/file operations run over the executor.
    // 对应Python: RemoteBackend(ssh_conn);nullptr 回到 LocalBackend 行为
    void setRemoteExecutor(CommandExecutor *executor);
    bool isRemote() const { return m_executor != nullptr; }

    // --- CLI ---

    // Run `hermes <args>` and return stdout (blocking, worker-thread safe).
    // 对应Python: LocalBackend.exec_cli / RemoteBackend.exec_cli
    QString execCli(const QStringList &args, int timeoutMs = kDefaultTimeoutMs);

    // --- files ---

    // 对应Python: read_file / write_file / list_dir / file_exists / delete_file
    QString readFile(const QString &path);
    bool writeFile(const QString &path, const QString &content);
    QStringList listDir(const QString &path);
    bool fileExists(const QString &path);
    bool deleteFile(const QString &path);

    // 对应Python: get_hermes_home(本地 ~/.hermes,远程 $HOME/.hermes)
    QString hermesHome();

    // --- api server ---

    // 对应Python: get_api_server_url(config.yaml platforms.api_server.extra)
    QString apiServerUrl();
    // 对应Python: get_api_server_key(.env API_SERVER_KEY= 优先)
    QString apiServerKey();

    // 对应Python: list_profiles(返回目录名列表,首项恒为 default)
    QStringList listProfiles();

    // --- sqlite ---

    // One result row keyed by column name.
    // 对应Python: read_sqlite 返回的 dict 行
    struct SqliteRow { QHash<QString, QString> columns; };

    // Run `sqlite3 -separator "|" -header <dbPath> <sql>` (QProcess locally,
    // CommandExecutor remotely — same split as execCli) and parse the
    // pipe-separated output. Failures log a qWarning() and yield an empty
    // list; never throws/crashes.
    // 对应Python: LocalBackend.read_sqlite / RemoteBackend.read_sqlite
    QList<SqliteRow> readSqlite(const QString &dbPath, const QString &sql);

    // --- HTTP/SSE ---

    // POST jsonBody to <apiServerUrl()><path> with bearer auth and stream
    // the SSE response; events arrive via sseEvent(). Only one stream at a
    // time. Must be called on this object's thread.
    bool startSseRequest(const QString &path, const QByteArray &jsonBody);
    void abortSseRequest();
    bool isStreaming() const { return m_sseReply != nullptr; }

    // --- pure helpers (unit-testable) ---

    // Text-level lookup of platforms.api_server.extra.{host,port} in YAML.
    // 对应Python: yaml.safe_load 路径访问的文本级等价实现
    static QString parseApiServerUrl(const QString &configYaml);
    // 对应Python: get_api_server_key 的 .env 扫描部分
    static QString parseApiServerKey(const QString &envContent);

signals:
    // SSE stream lifecycle (emitted on this object's thread).
    void sseEvent(const QString &event, const QString &data);
    void sseFinished();
    void sseError(const QString &message);

private:
    QString findHermesBin() const;
    QString shellQuote(const QString &s) const;

    CommandExecutor *m_executor = nullptr; // not owned
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_sseReply = nullptr;
    SseParser m_sseParser;
    mutable QString m_cachedBin;
    QString m_cachedRemoteHome;
};

} // namespace cubeshell
