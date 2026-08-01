// HermesBackend.cpp — see HermesBackend.h for the port map.
// 对应Python: core/hermes/backend.py

#include "hermes/HermesBackend.h"

#include "ssh/CommandExecutor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QtDebug>

namespace cubeshell {

// ---------------------------------------------------------------------------
// SseParser
// ---------------------------------------------------------------------------

void SseParser::reset()
{
    m_buffer.clear();
    m_event.clear();
    m_dataLines.clear();
}

QList<SseParser::Event> SseParser::feed(const QByteArray &chunk)
{
    m_buffer.append(chunk);
    return consumeLines();
}

QList<SseParser::Event> SseParser::finish()
{
    QList<Event> events = consumeLines();
    if (!m_dataLines.isEmpty()) {
        // 流结束但最后一个事件缺少空行分隔:仍然派发,避免丢数据
        Event ev;
        ev.event = m_event;
        ev.data = m_dataLines.join(QLatin1Char('\n'));
        events.append(ev);
    }
    reset();
    return events;
}

QList<SseParser::Event> SseParser::consumeLines()
{
    QList<Event> events;
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        QByteArray raw = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (raw.endsWith('\r'))
            raw.chop(1);

        if (raw.isEmpty()) {
            // 空行 → 派发累积的事件
            if (!m_dataLines.isEmpty()) {
                Event ev;
                ev.event = m_event;
                ev.data = m_dataLines.join(QLatin1Char('\n'));
                events.append(ev);
            }
            m_event.clear();
            m_dataLines.clear();
            continue;
        }
        if (raw.startsWith(':'))
            continue; // SSE 注释行

        QByteArray field, value;
        const int colon = raw.indexOf(':');
        if (colon < 0) {
            field = raw;
        } else {
            field = raw.left(colon);
            value = raw.mid(colon + 1);
            if (value.startsWith(' '))
                value.remove(0, 1);
        }
        if (field == "data")
            m_dataLines.append(QString::fromUtf8(value));
        else if (field == "event")
            m_event = QString::fromUtf8(value);
        // id/retry 字段忽略(本客户端无重连需求)
    }
    return events;
}

// ---------------------------------------------------------------------------
// HermesBackend — construction / mode
// ---------------------------------------------------------------------------

HermesBackend::HermesBackend(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void HermesBackend::setRemoteExecutor(CommandExecutor *executor)
{
    m_executor = executor;
    m_cachedRemoteHome.clear();
}

// 对应Python: backend._find_hermes_bin
QString HermesBackend::findHermesBin() const
{
    if (!m_cachedBin.isEmpty())
        return m_cachedBin;
    QString found = QStandardPaths::findExecutable(QStringLiteral("hermes"));
    if (found.isEmpty()) {
        // GUI 应用可能缺少用户 PATH,搜索常见安装位置
        const QString home = QDir::homePath();
        const QStringList candidates = {
            home + QStringLiteral("/.local/bin/hermes"),
            QStringLiteral("/usr/local/bin/hermes"),
            QStringLiteral("/opt/homebrew/bin/hermes"),
            home + QStringLiteral("/.hermes/bin/hermes"),
        };
        for (const QString &c : candidates) {
            const QFileInfo fi(c);
            if (fi.isFile() && fi.isExecutable()) {
                found = c;
                break;
            }
        }
    }
    // 都找不到就返回裸命令名,让后续报错更明确
    m_cachedBin = found.isEmpty() ? QStringLiteral("hermes") : found;
    return m_cachedBin;
}

QString HermesBackend::shellQuote(const QString &s) const
{
    QString escaped = s;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QStringLiteral("'%1'").arg(escaped);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

// 对应Python: LocalBackend.exec_cli / RemoteBackend.exec_cli
QString HermesBackend::execCli(const QStringList &args, int timeoutMs)
{
    if (m_executor) {
        QStringList quoted;
        for (const QString &a : args)
            quoted << (a.contains(QLatin1Char(' ')) || a.contains(QLatin1Char('\''))
                           ? shellQuote(a) : a);
        const QString cmd = QStringLiteral("hermes ") + quoted.join(QLatin1Char(' '));
        const ExecResult res = m_executor->exec(cmd, false, timeoutMs);
        return res.stdoutText;
    }

    QProcess proc;
    proc.start(findHermesBin(), args);
    if (!proc.waitForStarted(5000))
        return QString();
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return QString();
    }
    return QString::fromUtf8(proc.readAllStandardOutput());
}

// ---------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------

QString HermesBackend::readFile(const QString &path)
{
    if (m_executor) {
        const ExecResult res = m_executor->exec(
            QStringLiteral("cat %1 2>/dev/null").arg(shellQuote(path)),
            false, kDefaultTimeoutMs);
        return res.stdoutText;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

bool HermesBackend::writeFile(const QString &path, const QString &content)
{
    if (m_executor) {
        // base64 往返保证内容字节精确通过 exec 通道
        const QByteArray b64 = content.toUtf8().toBase64();
        const QString dir = path.section(QLatin1Char('/'), 0, -2);
        if (!dir.isEmpty())
            m_executor->exec(QStringLiteral("mkdir -p %1").arg(shellQuote(dir)),
                             false, kDefaultTimeoutMs);
        const ExecResult res = m_executor->exec(
            QStringLiteral("echo %1 | base64 -d > %2")
                .arg(QString::fromLatin1(b64), shellQuote(path)),
            false, kDefaultTimeoutMs);
        return res.ok();
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty())
        QDir().mkpath(dir);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content.toUtf8());
    return true;
}

QStringList HermesBackend::listDir(const QString &path)
{
    if (m_executor) {
        const ExecResult res = m_executor->exec(
            QStringLiteral("ls -1A %1 2>/dev/null").arg(shellQuote(path)),
            false, kDefaultTimeoutMs);
        QStringList out;
        const QStringList lines =
            res.stdoutText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &l : lines)
            out << l.trimmed();
        return out;
    }
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                | QDir::Hidden);
}

bool HermesBackend::fileExists(const QString &path)
{
    if (m_executor) {
        // Python 远端用 test -f,但调用点也用于目录判断,这里用 -e 更正确
        const ExecResult res = m_executor->exec(
            QStringLiteral("test -e %1 && echo exists").arg(shellQuote(path)),
            false, kDefaultTimeoutMs);
        return res.stdoutText.contains(QLatin1String("exists"));
    }
    return QFileInfo::exists(path);
}

bool HermesBackend::deleteFile(const QString &path)
{
    if (m_executor) {
        const ExecResult res = m_executor->exec(
            QStringLiteral("rm -f %1").arg(shellQuote(path)),
            false, kDefaultTimeoutMs);
        return res.ok();
    }
    return QFile::remove(path);
}

// 对应Python: get_hermes_home
QString HermesBackend::hermesHome()
{
    if (m_executor) {
        if (m_cachedRemoteHome.isEmpty()) {
            const ExecResult res = m_executor->exec(
                QStringLiteral("echo $HOME"), false, kDefaultTimeoutMs);
            const QString home = res.stdoutText.trimmed();
            m_cachedRemoteHome = home.isEmpty()
                ? QStringLiteral("~/.hermes")
                : home + QStringLiteral("/.hermes");
        }
        return m_cachedRemoteHome;
    }
    return QDir::homePath() + QStringLiteral("/.hermes");
}

// ---------------------------------------------------------------------------
// api server config（文本级 YAML/.env 解析,不引 YAML 库）
// ---------------------------------------------------------------------------

// 对应Python: get_api_server_url 的 yaml 路径 platforms.api_server.extra
QString HermesBackend::parseApiServerUrl(const QString &configYaml)
{
    QString host = QStringLiteral("127.0.0.1");
    QString port = QStringLiteral("8642");
    // 逐行按缩进定位 platforms: → api_server: → extra: → host/port
    int level = 0; // 0=顶层 1=platforms 2=api_server 3=extra
    int platformsIndent = -1, apiIndent = -1, extraIndent = -1;
    const QStringList lines = configYaml.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        int indent = 0;
        while (indent < line.size() && line.at(indent) == QLatin1Char(' '))
            ++indent;

        // 缩进回退 → 离开当前层级
        if (level >= 3 && indent <= extraIndent) level = 2;
        if (level >= 2 && indent <= apiIndent) level = 1;
        if (level >= 1 && indent <= platformsIndent) level = 0;

        if (level == 0 && trimmed == QLatin1String("platforms:")) {
            level = 1;
            platformsIndent = indent;
        } else if (level == 1 && trimmed == QLatin1String("api_server:")) {
            level = 2;
            apiIndent = indent;
        } else if (level == 2 && trimmed == QLatin1String("extra:")) {
            level = 3;
            extraIndent = indent;
        } else if (level == 3) {
            if (trimmed.startsWith(QLatin1String("host:")))
                host = trimmed.mid(5).trimmed()
                           .remove(QLatin1Char('"')).remove(QLatin1Char('\''));
            else if (trimmed.startsWith(QLatin1String("port:")))
                port = trimmed.mid(5).trimmed()
                           .remove(QLatin1Char('"')).remove(QLatin1Char('\''));
        }
    }
    if (host.isEmpty())
        host = QStringLiteral("127.0.0.1");
    if (port.isEmpty())
        port = QStringLiteral("8642");
    return QStringLiteral("http://%1:%2").arg(host, port);
}

// 对应Python: get_api_server_key 的 .env 扫描
QString HermesBackend::parseApiServerKey(const QString &envContent)
{
    const QStringList lines = envContent.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("API_SERVER_KEY=")))
            return line.section(QLatin1Char('='), 1).trimmed();
    }
    return QString();
}

QString HermesBackend::apiServerUrl()
{
    const QString content =
        readFile(hermesHome() + QStringLiteral("/config.yaml"));
    if (content.isEmpty())
        return QStringLiteral("http://127.0.0.1:8642");
    return parseApiServerUrl(content);
}

QString HermesBackend::apiServerKey()
{
    const QString envKey =
        parseApiServerKey(readFile(hermesHome() + QStringLiteral("/.env")));
    if (!envKey.isEmpty())
        return envKey;
    return QStringLiteral("change-me-local-dev");
}

// 对应Python: list_profiles(default 恒在首位)
QStringList HermesBackend::listProfiles()
{
    QStringList profiles{QStringLiteral("default")};
    QStringList names = listDir(hermesHome() + QStringLiteral("/profiles"));
    names.sort();
    for (const QString &name : names) {
        if (name.startsWith(QLatin1Char('.')) || name == QLatin1String("default"))
            continue;
        profiles << name;
    }
    return profiles;
}

// ---------------------------------------------------------------------------
// sqlite
// ---------------------------------------------------------------------------

namespace {

// Parse `sqlite3 -separator "|" -header` output: first line = column names,
// each following line = one row of pipe-separated values.
QList<HermesBackend::SqliteRow> parseSqliteOutput(const QString &output)
{
    QList<HermesBackend::SqliteRow> rows;
    QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString &l : lines) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    }
    if (lines.isEmpty())
        return rows;
    const QStringList headers = lines.first().split(QLatin1Char('|'));
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList values = lines.at(i).split(QLatin1Char('|'));
        HermesBackend::SqliteRow row;
        for (int c = 0; c < headers.size(); ++c)
            row.columns.insert(headers.at(c),
                               c < values.size() ? values.at(c) : QString());
        rows.append(row);
    }
    return rows;
}

} // namespace

// 对应Python: LocalBackend.read_sqlite / RemoteBackend.read_sqlite
QList<HermesBackend::SqliteRow> HermesBackend::readSqlite(const QString &dbPath,
                                                          const QString &sql)
{
    if (m_executor) {
        // 远程走 sqlite3 CLI,与 execCli 的 executor 分支同一模式
        const QString cmd =
            QStringLiteral("sqlite3 -separator '|' -header %1 %2")
                .arg(shellQuote(dbPath), shellQuote(sql));
        const ExecResult res = m_executor->exec(cmd, false, kDefaultTimeoutMs);
        if (!res.ok()) {
            qWarning() << "readSqlite: remote sqlite3 failed for" << dbPath;
            return {};
        }
        return parseSqliteOutput(res.stdoutText);
    }

    QProcess proc;
    proc.start(QStringLiteral("sqlite3"),
               {QStringLiteral("-separator"), QStringLiteral("|"),
                QStringLiteral("-header"), dbPath, sql});
    if (!proc.waitForStarted(5000)) {
        qWarning() << "readSqlite: sqlite3 not found or failed to start";
        return {};
    }
    if (!proc.waitForFinished(kDefaultTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        qWarning() << "readSqlite: sqlite3 timed out for" << dbPath;
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        qWarning() << "readSqlite: sqlite3 failed:"
                   << QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return {};
    }
    return parseSqliteOutput(QString::fromUtf8(proc.readAllStandardOutput()));
}

// ---------------------------------------------------------------------------
// HTTP/SSE
// ---------------------------------------------------------------------------

bool HermesBackend::startSseRequest(const QString &path, const QByteArray &jsonBody)
{
    if (m_sseReply)
        return false; // 一次只允许一条流

    QNetworkRequest req{QUrl(apiServerUrl() + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + apiServerKey().toUtf8());

    m_sseParser.reset();
    m_sseReply = m_nam->post(req, jsonBody);

    connect(m_sseReply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_sseReply)
            return;
        const QList<SseParser::Event> events =
            m_sseParser.feed(m_sseReply->readAll());
        for (const SseParser::Event &ev : events) {
            if (ev.data == QLatin1String("[DONE]"))
                continue; // OpenAI 风格结束标记,由 finished 统一收尾
            emit sseEvent(ev.event.isEmpty() ? QStringLiteral("message")
                                             : ev.event, ev.data);
        }
    });
    connect(m_sseReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_sseReply;
        m_sseReply = nullptr;
        reply->deleteLater();
        const QList<SseParser::Event> tail = m_sseParser.finish();
        for (const SseParser::Event &ev : tail) {
            if (ev.data != QLatin1String("[DONE]"))
                emit sseEvent(ev.event.isEmpty() ? QStringLiteral("message")
                                                 : ev.event, ev.data);
        }
        if (reply->error() != QNetworkReply::NoError
            && reply->error() != QNetworkReply::OperationCanceledError) {
            emit sseError(reply->errorString());
            return;
        }
        emit sseFinished();
    });
    return true;
}

void HermesBackend::abortSseRequest()
{
    if (m_sseReply)
        m_sseReply->abort();
}

} // namespace cubeshell
