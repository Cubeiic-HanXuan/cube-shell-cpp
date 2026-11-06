// ClaudeSessionStore.cpp — 会话文件枚举、摘要缓存、按范围读取及安全导出。
// 本地与远程返回相同的字节分页结构，JSONL 解析统一交给 ClaudeSessionData。

#include "claude_code/ClaudeSessionStore.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SshClient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace cubeshell {
namespace {
const QString remoteRoot = QStringLiteral("root=\"${CLAUDE_CONFIG_DIR:-$HOME/.claude}/projects\"; ");
constexpr qint64 sampleSize = 64 * 1024;
}
ClaudeSessionStore::ClaudeSessionStore(const QString &claudeHome) : m_home(claudeHome) {}
ClaudeSessionStore::ClaudeSessionStore(RemoteCommand command)
    : m_remote(true), m_command(std::move(command)) {}
ClaudeSessionStore::ClaudeSessionStore(std::shared_ptr<SshClient> client)
    : m_remote(true), m_client(std::move(client)) {}

bool ClaudeSessionStore::stopped(QString *error) const
{
    if (m_cancelled.load()) {
        *error = QStringLiteral("读取已取消");
        return true;
    }
    return false;
}
QString ClaudeSessionStore::localPath(const QString &relativePath) const
{
    if (!ClaudeSessionData::validRelativePath(relativePath))
        return {};
    const QDir root(m_home + QStringLiteral("/projects"));
    const QFileInfo file(root.filePath(relativePath));
    const QString canonicalRoot = root.canonicalPath();
    const QString canonicalFile = file.canonicalFilePath();
    if (canonicalRoot.isEmpty() || !canonicalFile.startsWith(canonicalRoot + QLatin1Char('/'))
        || file.isSymLink() || QFileInfo(file.absolutePath()).isSymLink())
        return {};
    return file.absoluteFilePath();
}
QString ClaudeSessionStore::remoteFile(const QString &relativePath) const
{
    return QStringLiteral("\"$root\"/") + ClaudeSessionData::quote(relativePath);
}
QByteArray ClaudeSessionStore::remoteCommand(const QString &command, QString *error)
{
    if (stopped(error))
        return {};
    if (m_command)
        return m_command(command, error);
    if (!m_client) {
        *error = QStringLiteral("SSH 连接不可用");
        return {};
    }
    // 不复用其他模块的 CommandExecutor 状态，独立命令携带本存储的取消标记。
    const auto result = CommandExecutor::runCommand(m_client.get(), command, false, 30000, {}, &m_cancelled);
    if (!result.ok() || result.exitCode != 0) {
        *error = !result.errorMessage.isEmpty() ? result.errorMessage
            : result.timedOut ? QStringLiteral("读取远程会话超时")
            : QStringLiteral("无法读取远程会话：%1").arg(result.stderrText.trimmed());
        return {};
    }
    return result.stdoutText.toUtf8();
}

QList<ClaudeSessionInfo> ClaudeSessionStore::manifest(QString *error)
{
    QList<ClaudeSessionInfo> result;
    if (m_remote) {
        // 用 Base64 编码路径，再用制表符分隔字段；文件名自身含换行 / 制表符也不会串行。
        // stat 同时兼容 GNU 与 BSD 形式，项目目录和日志文件均排除符号链接。
        const QString script = remoteRoot + QStringLiteral(
            "[ -d \"$root\" ] || exit 0; [ -r \"$root\" ] || exit 1; "
            "for d in \"$root\"/*; do [ -d \"$d\" ] && [ ! -L \"$d\" ] || continue; "
            "for f in \"$d\"/*.jsonl; do [ -f \"$f\" ] && [ ! -L \"$f\" ] || continue; "
            "rel=${f#\"$root\"/}; printf '%s' \"$rel\" | base64 | tr -d '\\r\\n'; "
            "printf '\\t'; (stat -c '%s %Y' \"$f\" 2>/dev/null || stat -f '%z %m' \"$f\") || exit 1; "
            "done; done");
        const auto bytes = remoteCommand(script, error);
        for (const auto &line : bytes.split('\n')) {
            const auto fields = line.split('\t');
            if (fields.size() != 2)
                continue;
            ClaudeSessionInfo info;
            info.relativePath = QString::fromUtf8(QByteArray::fromBase64(fields[0]));
            if (!ClaudeSessionData::validRelativePath(info.relativePath))
                continue;
            const auto stats = fields[1].simplified().split(' ');
            if (stats.size() != 2)
                continue;
            info.fileSize = stats[0].toLongLong();
            info.updatedAtMs = stats[1].toLongLong() * 1000;
            info.sessionId = info.relativePath.section(QLatin1Char('/'), -1).chopped(6);
            result.append(info);
        }
    } else {
        const QDir root(m_home + QStringLiteral("/projects"));
        if (!root.exists())
            return result;
        if (!QFileInfo(root.absolutePath()).isReadable()) {
            *error = QStringLiteral("无权读取会话目录");
            return result;
        }
        for (const auto &project : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
            const QDir dir(root.filePath(project));
            for (const auto &file : dir.entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files | QDir::NoSymLinks)) {
                ClaudeSessionInfo info;
                info.relativePath = project + QLatin1Char('/') + file.fileName();
                if (!ClaudeSessionData::validRelativePath(info.relativePath))
                    continue;
                info.sessionId = file.completeBaseName();
                info.fileSize = file.size();
                info.updatedAtMs = file.lastModified().toMSecsSinceEpoch();
                result.append(info);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.updatedAtMs != b.updatedAtMs ? a.updatedAtMs > b.updatedAtMs : a.relativePath < b.relativePath;
    });
    return result;
}

QByteArray ClaudeSessionStore::readRange(const ClaudeSessionInfo &session, qint64 offset, qint64 size, QString *error)
{
    if (stopped(error))
        return {};
    if (!ClaudeSessionData::validRelativePath(session.relativePath) || offset < 0 || size < 0 || size > chunkSize) {
        *error = QStringLiteral("无效的会话文件或读取范围");
        return {};
    }
    if (m_remote) {
        // dd 按块定位后在本地裁掉前缀，避免逐字节跳过大文件；Base64 防止远程文本解码
        // 破坏跨页 UTF-8 或其他原始字节，分页拼接 / JSONL 导出才能与源文件一致。
        constexpr qint64 blockSize = 65536;
        const auto skip = offset / blockSize;
        const auto prefix = offset % blockSize;
        const auto count = (prefix + size + blockSize - 1) / blockSize;
        const QString script = remoteRoot + QStringLiteral(
            "f=%1; [ -f \"$f\" ] && [ -r \"$f\" ] && [ ! -L \"$f\" ] && [ ! -L \"${f%/*}\" ] || exit 1; "
            "dd if=\"$f\" bs=65536 skip=%2 count=%3 2>/dev/null | base64")
            .arg(remoteFile(session.relativePath)).arg(skip).arg(count);
        const auto encoded = remoteCommand(script, error);
        return QByteArray::fromBase64(encoded).mid(prefix, size);
    }
    const auto path = localPath(session.relativePath);
    if (path.isEmpty()) {
        *error = QStringLiteral("会话文件已不存在或位于项目目录之外");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
        *error = file.errorString();
        return {};
    }
    const auto bytes = file.read(size);
    if (file.error() != QFileDevice::NoError)
        *error = file.errorString();
    return bytes;
}

QHash<QString, QPair<QByteArray, QByteArray>> ClaudeSessionStore::remoteSummaries(
    const QList<ClaudeSessionInfo> &sessions, QString *error)
{
    QHash<QString, QPair<QByteArray, QByteArray>> samples;
    QList<ClaudeSessionInfo> changed;
    for (const auto &session : sessions) {
        const auto cached = m_cache.constFind(session.relativePath);
        if (cached == m_cache.cend() || cached->fileSize != session.fileSize || cached->updatedAtMs != session.updatedAtMs)
            changed.append(session);
    }
    // 每批最多 16 个变化文件，共用一次远程命令读取首尾固定长度样本。
    // 大项目刷新时避免逐文件读取头尾造成大量 SSH 往返。
    for (int start = 0; start < changed.size(); start += 16) {
        QString script = remoteRoot;
        for (int i = start; i < qMin(start + 16, static_cast<int>(changed.size())); ++i) {
            const auto &s = changed[i];
            script += QStringLiteral(
                "f=%1; if [ -f \"$f\" ] && [ -r \"$f\" ] && [ ! -L \"$f\" ] && [ ! -L \"${f%/*}\" ]; then "
                "printf '%2\\t'; dd if=\"$f\" bs=65536 count=1 2>/dev/null | base64 | tr -d '\\r\\n'; "
                "printf '\\t'; ").arg(remoteFile(s.relativePath)).arg(i);
            if (s.fileSize > sampleSize)
                script += QStringLiteral("tail -c %1 \"$f\" | base64 | tr -d '\\r\\n'; ").arg(qMin(sampleSize, s.fileSize - sampleSize));
            script += QStringLiteral("printf '\\n'; else printf '%1\\tERROR\\n'; fi; ").arg(i);
        }
        const auto output = remoteCommand(script, error);
        if (!error->isEmpty())
            break;
        for (const auto &line : output.split('\n')) {
            const auto fields = line.split('\t');
            if (fields.size() != 3)
                continue;
            bool valid = false;
            const int index = fields[0].toInt(&valid);
            if (valid && index >= start && index < qMin(start + 16, static_cast<int>(changed.size())))
                samples.insert(changed[index].relativePath, {QByteArray::fromBase64(fields[1]), QByteArray::fromBase64(fields[2])});
        }
    }
    return samples;
}

QList<ClaudeSessionInfo> ClaudeSessionStore::list(QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    auto result = manifest(error);
    QHash<QString, ClaudeSessionInfo> updatedCache;
    QHash<QString, QPair<QByteArray, QByteArray>> remoteSamples;
    if (m_remote && error->isEmpty())
        remoteSamples = remoteSummaries(result, error);
    QHash<QString, QJsonObject> indexEntries;
    if (!m_remote) {
        // 旧版 CLI 的索引只用于补齐缺失摘要；列表仍以真实 JSONL 为准，不展示已删除的文件。
        QSet<QString> projects;
        for (const auto &s : result)
            projects.insert(s.relativePath.section(QLatin1Char('/'), 0, 0));
        for (const auto &project : projects) {
            QFile index(m_home + QStringLiteral("/projects/") + project + QStringLiteral("/sessions-index.json"));
            if (!index.open(QIODevice::ReadOnly) || index.size() > 8 * 1024 * 1024)
                continue;
            for (const auto &value : QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("entries")).toArray()) {
                const auto entry = value.toObject();
                indexEntries.insert(project + QLatin1Char('/') + entry.value(QStringLiteral("sessionId")).toString() + QStringLiteral(".jsonl"), entry);
            }
        }
    }
    int unreadable = 0;
    QString lastError;
    for (auto &info : result) {
        if (stopped(error))
            break;
        const auto cached = m_cache.constFind(info.relativePath);
        if (cached != m_cache.cend() && cached->fileSize == info.fileSize && cached->updatedAtMs == info.updatedAtMs) {
            info = *cached;
            updatedCache.insert(info.relativePath, info);
            continue;
        }
        ClaudeSessionData data;
        QString readError;
        if (m_remote && !remoteSamples.contains(info.relativePath))
            readError = QStringLiteral("远程摘要不可读，请检查文件权限或连接状态");
        const auto head = m_remote ? remoteSamples.value(info.relativePath).first : readRange(info, 0, sampleSize, &readError);
        data.append(head, info.fileSize <= sampleSize);
        if (info.fileSize > sampleSize) {
            // 若首尾样本相邻，可继续喂给增量解析器；中间有缺口时必须跳过尾部首个半行，
            // 只逐条接收完整 JSON，不能把日志头的半行与不相邻的日志尾直接拼接。
            const qint64 tailStart = qMax(sampleSize, info.fileSize - sampleSize);
            auto tail = m_remote ? remoteSamples.value(info.relativePath).second : readRange(info, tailStart, sampleSize, &readError);
            if (tailStart == sampleSize) {
                data.append(tail, true);
            } else {
                const auto newline = tail.indexOf('\n');
                if (newline >= 0) {
                    tail.remove(0, newline + 1);
                    for (const auto &line : tail.split('\n')) {
                        const auto doc = QJsonDocument::fromJson(line);
                        if (doc.isObject())
                            data.accept(doc.object());
                    }
                }
                info.partialSummary = true;
            }
        }
        if (!readError.isEmpty()) {
            ++unreadable;
            lastError = readError;
            info.partialSummary = true;
        }
        const auto &summary = data.summary();
        info.name = summary.name;
        info.cwd = summary.cwd;
        info.firstMessage = summary.firstMessage;
        info.lastMessage = summary.lastMessage;
        info.startedAtMs = summary.startedAtMs;
        const auto index = indexEntries.value(info.relativePath);
        if (info.cwd.isEmpty())
            info.cwd = index.value(QStringLiteral("projectPath")).toString();
        if (info.name.isEmpty())
            info.name = index.value(QStringLiteral("summary")).toString();
        if (info.firstMessage.isEmpty())
            info.firstMessage = index.value(QStringLiteral("firstPrompt")).toString();
        if (info.name.isEmpty())
            info.name = info.sessionId;
        if (readError.isEmpty())
            updatedCache.insert(info.relativePath, info);
    }
    m_cache = std::move(updatedCache);
    if (unreadable)
        *error = QStringLiteral("%1 个会话的摘要读取失败：%2").arg(unreadable).arg(lastError);
    return result;
}

// 分页固定在枚举时的 fileSize 边界：日志追加不无限追读，日志缩短则报错要求重新刷新。
ClaudeSessionPage ClaudeSessionStore::read(const ClaudeSessionInfo &session, qint64 offset)
{
    QMutexLocker lock(&m_mutex);
    ClaudeSessionPage page;
    page.totalBytes = session.fileSize;
    page.nextOffset = offset;
    if (offset > session.fileSize) {
        page.error = QStringLiteral("文件已变化，请刷新会话");
        return page;
    }
    const qint64 expected = qMin(chunkSize, session.fileSize - offset);
    page.bytes = readRange(session, offset, expected, &page.error);
    if (page.error.isEmpty() && page.bytes.size() != expected)
        page.error = QStringLiteral("会话文件已缩短或读取不完整，请刷新后重试");
    page.nextOffset += page.bytes.size();
    page.endOfFile = page.nextOffset >= session.fileSize;
    return page;
}

bool ClaudeSessionStore::exportSession(const ClaudeSessionInfo &session, const QString &destination, bool markdown, QString *error)
{
    error->clear();
    // 本地导出不能覆盖任何原始历史文件；解析真实路径，防止目标经符号链接指回历史目录。
    if (!m_remote) {
        const QFileInfo target(destination);
        const QDir history(m_home + QStringLiteral("/projects"));
        const QString targetPath = target.canonicalFilePath().isEmpty()
            ? QFileInfo(target.absolutePath()).canonicalFilePath() + QLatin1Char('/') + target.fileName()
            : target.canonicalFilePath();
        if ((!history.canonicalPath().isEmpty() && targetPath.startsWith(history.canonicalPath() + QLatin1Char('/')))
            || target.absoluteFilePath() == QFileInfo(localPath(session.relativePath)).absoluteFilePath()
            || (!target.canonicalFilePath().isEmpty() && target.canonicalFilePath() == QFileInfo(localPath(session.relativePath)).canonicalFilePath())) {
            *error = QStringLiteral("不能覆盖原始会话文件");
            return false;
        }
    }
    // 分页写入临时目标，只有全部读取 / 转换成功才 commit；中途失败保留原目标文件。
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = output.errorString();
        return false;
    }
    ClaudeSessionData data;
    qint64 offset = 0;
    do {
        const auto page = read(session, offset);
        if (!page.error.isEmpty()) {
            *error = page.error;
            return false;
        }
        if (markdown)
            data.append(page.bytes, page.endOfFile);
        else if (output.write(page.bytes) != page.bytes.size()) {
            *error = output.errorString();
            return false;
        }
        offset = page.nextOffset;
        if (page.endOfFile)
            break;
    } while (!stopped(error));
    if (!error->isEmpty())
        return false;
    if (markdown) {
        const auto bytes = data.toMarkdown().toUtf8();
        if (output.write(bytes) != bytes.size()) {
            *error = output.errorString();
            return false;
        }
    }
    if (!output.commit()) {
        *error = output.errorString();
        return false;
    }
    return true;
}
} // namespace cubeshell
