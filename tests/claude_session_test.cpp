// claude_session_test.cpp — 合成会话的解析、分页、导出与远程读取协议回归。
// 仅使用临时目录，不读取用户 ~/.claude，也不运行 Claude CLI；远程协议由本地 shell 模拟。
#include "claude_code/ClaudeSessionData.h"
#include "claude_code/ClaudeSessionStore.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

using namespace cubeshell;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { qWarning() << "FAIL:" << #condition << "line" << __LINE__; ++failures; } } while (0)
static const QString sid = QStringLiteral("01234567-89ab-4cde-8fab-0123456789ab");
static QByteArray line(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }
static QJsonObject message(const QString &role, const QJsonValue &content, const QString &uuid)
{
    return {{"type", role}, {"uuid", uuid}, {"sessionId", sid}, {"cwd", "/tmp/project with ' quotes"},
            {"timestamp", "2026-11-05T09:08:00.000Z"},
            {"message", QJsonObject{{"role", role}, {"content", content}, {"model", role == "assistant" ? "test-model" : ""}}}};
}
static QByteArray sample()
{
    QByteArray bytes = line(QJsonObject{{"type", "file-history-snapshot"}, {"snapshot", QJsonObject{}}});
    bytes += line(message("user", QStringLiteral("<system-reminder>不应作为标题</system-reminder>\n优化中文接口"), "u1"));
    bytes += line(message("assistant", QJsonArray{
        QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "ENCRYPTED-NOT-DISPLAYED"}},
        QJsonObject{{"type", "text"}, {"text", "先检查 **代码**。"}},
        QJsonObject{{"type", "tool_use"}, {"id", "tool1"}, {"name", "Edit"},
                    {"input", QJsonObject{{"file_path", "/tmp/app.cpp"}, {"old_string", "old"}, {"new_string", "new"}}}}}, "a1"));
    bytes += line(message("user", QJsonArray{QJsonObject{{"type", "tool_result"}, {"tool_use_id", "tool1"},
        {"is_error", true}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Exit code 1\n失败：未找到内容"}}}}}}, "r1"));
    auto meta = message("user", "environment context", "meta");
    meta.insert("isMeta", true);
    bytes += line(meta);
    bytes += line(QJsonObject{{"type", "system"}, {"subtype", "compact_boundary"}, {"uuid", "compact"}});
    bytes += line(message("user", QJsonArray{QJsonObject{{"type", "text"}, {"text", "继续"}},
                                             QJsonObject{{"type", "image"}, {"source", QJsonObject{{"data", "DO-NOT-RENDER"}}}}}, "u2"));
    bytes += line(message("assistant", QJsonArray{QJsonObject{{"type", "text"}, {"text", "最终回复 ``` html <script>alert(1)</script>"}}}, "a2"));
    bytes += line(QJsonObject{{"type", "ai-title"}, {"aiTitle", "自动标题"}});
    bytes += line(QJsonObject{{"type", "custom-title"}, {"customTitle", "我的会话"}});
    bytes += line(QJsonObject{{"type", "summary"}, {"summary", "不应覆盖自定义标题"}});
    return bytes;
}
// 同一份合成日志整块读取与每 7 字节切分读取应得到相同结果，覆盖中文 UTF-8 跨块边界。
// 同时检查标题优先级、工具结果归并、附件占位及无最终回复的轮次统计。
static void parserTests()
{
    const auto bytes = sample();
    ClaudeSessionData whole;
    whole.append(bytes, true);
    ClaudeSessionData chunked;
    for (int i = 0; i < bytes.size(); i += 7)
        chunked.append(bytes.mid(i, 7), i + 7 >= bytes.size());
    CHECK(whole.toMarkdown() == chunked.toMarkdown());
    CHECK(whole.summary().name == QStringLiteral("我的会话"));
    CHECK(whole.summary().firstMessage == QStringLiteral("优化中文接口"));
    CHECK(whole.summary().lastMessage.contains(QStringLiteral("最终回复")));
    CHECK(whole.turns().size() == 2);
    const auto turns = whole.presentationTurns();
    CHECK(turns.size() == 2);
    CHECK(turns[0].userEntry == 0);
    CHECK(turns[0].firstReply == -1);
    CHECK(turns[0].tools == 1 && turns[0].failures == 1);
    CHECK(turns[0].changedFiles == 0 && turns[0].pendingTools == 0);
    CHECK(turns[1].firstReply >= 0 && turns[1].steps == 0);
    CHECK(turns[1].replyPreview.contains(QStringLiteral("最终回复")));
    CHECK(whole.toolCount() == 1);
    CHECK(whole.failureCount() == 1);
    CHECK(whole.models() == QStringList{"test-model"});
    CHECK(!whole.toMarkdown().contains("ENCRYPTED-NOT-DISPLAYED"));
    CHECK(!whole.toMarkdown().contains("DO-NOT-RENDER"));
    CHECK(whole.toMarkdown().contains(QStringLiteral("思考内容不可见")));
    CHECK(whole.toMarkdown().contains(QStringLiteral("图片附件")));
    bool found = false;
    for (const auto &entry : whole.entries()) {
        if (entry.kind == ClaudeSessionEntry::Tool) {
            found = true;
            CHECK(entry.hasResult && entry.failed && entry.isChange());
            CHECK(entry.filePath() == "/tmp/app.cpp");
            CHECK(entry.result.contains("Exit code 1"));
        }
    }
    CHECK(found);
    const auto count = whole.entries().size();
    whole.append(bytes, true);
    CHECK(whole.entries().size() == count); // 按记录 UUID 去重，重复追加不得产生重复条目
    whole.append("broken-json\n", true);
    CHECK(whole.malformedLines() == 1);
    const auto last = line(message("assistant", "跨块完成", "last"));
    whole.append(last.left(12), true);
    CHECK(whole.hasIncompleteLine());
    whole.append(last.mid(12), true);
    CHECK(!whole.hasIncompleteLine());
    CHECK(whole.summary().lastMessage == QStringLiteral("跨块完成"));
    CHECK(ClaudeSessionData::fenced("```\nhi").startsWith("````\n"));
    auto side = message("user", "subagent not a main turn", "side");
    side.insert("isSidechain", true);
    whole.accept(side);
    CHECK(whole.turns().size() == 2);
}
// 恢复命令仅验证字符串，不执行命令；覆盖 POSIX / PowerShell 引号、控制字符与目录越界。
static void commandTests()
{
    ClaudeSessionInfo info;
    info.sessionId = sid;
    info.cwd = "/tmp/it's a project";
    const auto posix = ClaudeSessionData::resumeCommand(info);
    CHECK(posix.contains("'\\''"));
    CHECK(posix.contains(" && claude --resume '"));
    const auto ps = ClaudeSessionData::resumeCommand(info, true);
    CHECK(ps.contains("it''s"));
    CHECK(ps.startsWith("Set-Location -LiteralPath"));
    CHECK(ps.contains("if ($?)"));
    info.cwd = "/tmp/\nmalicious";
    CHECK(ClaudeSessionData::resumeCommand(info).isEmpty());
    info.cwd = "/tmp";
    info.sessionId = "--dangerous; command";
    CHECK(ClaudeSessionData::resumeCommand(info).isEmpty());
    CHECK(ClaudeSessionData::validRelativePath("-project/" + sid + ".jsonl"));
    CHECK(!ClaudeSessionData::validRelativePath("../" + sid + ".jsonl"));
    CHECK(!ClaudeSessionData::validRelativePath("a/subagents/" + sid + ".jsonl"));
    CHECK(!ClaudeSessionData::validRelativePath("/a/" + sid + ".jsonl"));
    CHECK(!ClaudeSessionData::validRelativePath("a/agent-123.jsonl"));
    CHECK(ClaudeSessionData::projectName("C:\\work\\example\\") == "example");
    info.name = "中文 api";
    info.cwd = "/tmp/project";
    info.firstMessage = "prompt";
    CHECK(ClaudeSessionData::matches(info, "API project"));
    CHECK(!ClaudeSessionData::matches(info, "notfound"));
    CHECK(ClaudeSessionData::relativeTime(1000, 61000) == QStringLiteral("1 分钟前"));
}
// 临时大文件覆盖首尾摘要、旧索引兼容、多页拼接、两种导出及截断 / 取消 / 失联错误。
static void storeTests()
{
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const auto dir = temp.path() + "/projects/-project";
    CHECK(QDir().mkpath(dir));
    const auto source = dir + "/" + sid + ".jsonl";
    QFile file(source);
    CHECK(file.open(QIODevice::WriteOnly));
    QByteArray bytes = sample();
    for (int i = 0; i < 5000; ++i)
        bytes += line(message("assistant", QString(90, 'x') + QString::number(i), QString::number(i)));
    bytes += line(QJsonObject{{"type", "custom-title"}, {"customTitle", "末尾重命名"}});
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    QFile index(dir + "/sessions-index.json");
    CHECK(index.open(QIODevice::WriteOnly));
    index.write(QJsonDocument(QJsonObject{{"entries", QJsonArray{
        QJsonObject{{"sessionId", sid}, {"summary", "stale title"}},
        QJsonObject{{"sessionId", "ffffffff-89ab-4cde-8fab-0123456789ab"}, {"summary", "missing file"}}}}}).toJson());
    index.close();
    ClaudeSessionStore store(temp.path());
    QString error;
    QElapsedTimer timer;
    timer.start();
    const auto sessions = store.list(&error);
    CHECK(error.isEmpty());
    CHECK(sessions.size() == 1);
    if (sessions.isEmpty())
        return;
    const auto &session = sessions[0];
    CHECK(session.name == QStringLiteral("末尾重命名"));
    CHECK(session.partialSummary);
    CHECK(session.firstMessage == QStringLiteral("优化中文接口"));
    CHECK(store.list(&error)[0].name == session.name);
    QByteArray restored;
    ClaudeSessionData parsed;
    qint64 offset = 0;
    int pages = 0;
    do {
        const auto page = store.read(session, offset);
        CHECK(page.error.isEmpty());
        CHECK(page.nextOffset >= offset);
        restored += page.bytes;
        parsed.append(page.bytes, page.endOfFile);
        offset = page.nextOffset;
        ++pages;
        if (page.endOfFile || !page.error.isEmpty()) break;
    } while (pages < 100);
    CHECK(pages > 1);
    CHECK(restored == bytes);
    CHECK(parsed.entries().size() > 5000);
    const auto raw = temp.path() + "/export.jsonl";
    CHECK(store.exportSession(session, raw, false, &error));
    QFile exported(raw);
    CHECK(exported.open(QIODevice::ReadOnly));
    CHECK(exported.readAll() == bytes);
    const auto md = temp.path() + "/export.md";
    CHECK(store.exportSession(session, md, true, &error));
    QFile markdown(md);
    CHECK(markdown.open(QIODevice::ReadOnly));
    CHECK(markdown.readAll().contains("xxxx4999"));
    CHECK(!store.exportSession(session, source, false, &error));
    CHECK(!error.isEmpty());
    ClaudeSessionInfo invalid = session;
    invalid.relativePath = "../" + sid + ".jsonl";
    CHECK(!store.read(invalid, 0).error.isEmpty());
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.close();
    CHECK(!store.read(session, 0).error.isEmpty());
    store.cancel();
    CHECK(!store.read(session, 0).error.isEmpty());
    qInfo() << "Synthetic large transcript:" << bytes.size() << "bytes," << pages << "pages," << timer.elapsed() << "ms";
    ClaudeSessionStore remote(std::shared_ptr<SshClient>{});
    CHECK(remote.list(&error).isEmpty());
    CHECK(!error.isEmpty()); // 远程连接失效必须报错，不能回退读取本地历史
}
// 在临时目录内用本地 /bin/sh 执行远程读取脚本，验证 Base64 / stat / dd 协议与批量缓存。
// 17 个文件跨两批取摘要，不需要真实 SSH 服务器；非 POSIX 平台跳过此脚本协议测试。
static void remoteProtocolTests()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    QTemporaryDir temp;
    const auto dir = temp.path() + "/projects/-quotes' and spaces";
    CHECK(QDir().mkpath(dir));
    QByteArray bytes = sample();
    for (int i = 0; i < 2000; ++i)
        bytes += line(message("assistant", QStringLiteral("远程分块内容 %1").arg(i), QString::number(i)));
    for (int i = 0; i < 17; ++i) {
        const auto id = QStringLiteral("01234567-89ab-4cde-8fab-%1").arg(i, 12, 16, QLatin1Char('0'));
        QFile file(dir + "/" + id + ".jsonl");
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(i == 0 ? bytes : sample());
    }
    int commands = 0;
    ClaudeSessionStore store(ClaudeSessionStore::RemoteCommand([&](const QString &command, QString *error) {
        ++commands;
        QProcess proc;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("CLAUDE_CONFIG_DIR", temp.path());
        proc.setProcessEnvironment(env);
        proc.start("/bin/sh", {"-c", command});
        if (!proc.waitForFinished(15000) || proc.exitCode() != 0)
            *error = QString::fromUtf8(proc.readAllStandardError());
        return proc.readAllStandardOutput();
    }));
    QString error;
    const auto sessions = store.list(&error);
    CHECK(error.isEmpty());
    CHECK(sessions.size() == 17);
    CHECK(commands == 3); // 一次清单 + 两批摘要，不按文件分别请求首尾内容
    CHECK(store.list(&error).size() == 17);
    CHECK(commands == 4); // 摘要命中缓存，刷新只需重新取清单
    for (const auto &session : sessions) {
        CHECK(!session.name.isEmpty());
        CHECK(session.cwd == "/tmp/project with ' quotes");
        if (session.fileSize != bytes.size())
            continue;
        QByteArray restored;
        qint64 offset = 0;
        do {
            const auto page = store.read(session, offset);
            CHECK(page.error.isEmpty());
            restored += page.bytes;
            offset = page.nextOffset;
            if (page.endOfFile || !page.error.isEmpty()) break;
        } while (offset < bytes.size());
        CHECK(restored == bytes);
    }
#endif
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    parserTests();
    commandTests();
    storeTests();
    remoteProtocolTests();
    qInfo() << "claude_session_test failures:" << failures;
    return failures ? 1 : 0;
}
