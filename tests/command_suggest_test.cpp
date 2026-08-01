// Command suggestion unit test: CommandIndex, CommandHistory, and
// TerminalCommandSuggest::extractCommandFromPrompt.

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "terminal/CommandIndex.h"
#include "terminal/CommandHistory.h"
#include "terminal_command_suggest.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// ---------------------------------------------------------------------------
// CommandIndex tests
// ---------------------------------------------------------------------------

static void testCommandIndexBuiltinOnly()
{
    qInfo() << "--- testCommandIndexBuiltinOnly ---";
    CommandIndex idx;
    // load with a non-existent path → still merges builtin table
    bool jsonOk = idx.load(QStringLiteral("/nonexistent/path/fake.json"));
    CHECK(!jsonOk); // JSON not found
    CHECK(!idx.commands().isEmpty());
    // Builtin commands should be present
    CHECK(idx.commands().contains(QStringLiteral("ls")));
    CHECK(idx.commands().contains(QStringLiteral("docker")));
    CHECK(idx.commands().contains(QStringLiteral("grep")));
    // Builtin table has ~100 commands
    CHECK(idx.commands().size() >= 80);
}

static void testCommandIndexWithJson()
{
    qInfo() << "--- testCommandIndexWithJson ---";
    // Use the source tree conf/linux_commands.json
    const QString jsonPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/../../../../conf/linux_commands.json");
    // Also try the standard source conf location
    QString resolvedPath;
    QStringList candidates = {
        jsonPath,
        QDir::currentPath() + QStringLiteral("/conf/linux_commands.json"),
        QDir::currentPath() + QStringLiteral("/../conf/linux_commands.json"),
        QDir::currentPath() + QStringLiteral("/../../conf/linux_commands.json"),
        QDir::currentPath() + QStringLiteral("/../../../conf/linux_commands.json"),
    };
    for (const QString &p : candidates) {
        if (QFile::exists(p)) {
            resolvedPath = p;
            break;
        }
    }
    if (resolvedPath.isEmpty()) {
        qWarning() << "  SKIP: conf/linux_commands.json not found, testing default load()";
        CommandIndex idx;
        idx.load(); // let it auto-resolve
        CHECK(idx.commands().contains(QStringLiteral("ls")));
        return;
    }
    qInfo() << "  Using JSON:" << resolvedPath;
    CommandIndex idx;
    bool jsonOk = idx.load(resolvedPath);
    CHECK(jsonOk);
    // JSON + builtin merge should have more commands than builtin alone
    CHECK(idx.commands().size() >= 100);
    CHECK(idx.commands().contains(QStringLiteral("ls")));
    CHECK(idx.commands().contains(QStringLiteral("docker")));
    CHECK(idx.commands().contains(QStringLiteral("grep")));
}

static void testCommandIndexPrefixMatch()
{
    qInfo() << "--- testCommandIndexPrefixMatch ---";
    CommandIndex idx;
    idx.load(QStringLiteral("/nonexistent/path/fake.json"));

    // computeSuggestions("gre") should include grep, egrep, fgrep
    QStringList results = idx.computeSuggestions(QStringLiteral("gre"));
    CHECK(results.contains(QStringLiteral("grep")));
    // egrep/fgrep start with 'e'/'f' not 'gre', verify all results start with "gre"
    for (const QString &r : results) {
        CHECK(r.startsWith(QStringLiteral("gre")));
    }
    // grep should be in the results
    CHECK(!results.isEmpty());
}

static void testCommandIndexOptionMatch()
{
    qInfo() << "--- testCommandIndexOptionMatch ---";
    CommandIndex idx;
    idx.load(QStringLiteral("/nonexistent/path/fake.json"));

    // "ls -" should return ls options starting with '-'
    QStringList results = idx.computeSuggestions(QStringLiteral("ls -"));
    CHECK(!results.isEmpty());
    CHECK(results.contains(QStringLiteral("-l")));
    CHECK(results.contains(QStringLiteral("-a")));
    for (const QString &r : results) {
        CHECK(r.startsWith(QStringLiteral("-")));
    }

    // "ls -l" should return only options starting with "-l"
    QStringList results2 = idx.computeSuggestions(QStringLiteral("ls -l"));
    CHECK(!results2.isEmpty());
    for (const QString &r : results2) {
        CHECK(r.startsWith(QStringLiteral("-l")));
    }
}

static void testCommandIndexEmptyInput()
{
    qInfo() << "--- testCommandIndexEmptyInput ---";
    CommandIndex idx;
    idx.load(QStringLiteral("/nonexistent/path/fake.json"));

    // Empty input → all commands
    QStringList results = idx.computeSuggestions(QString());
    CHECK(results.size() >= 80);
    CHECK(results == idx.commands());

    // Whitespace-only → also all commands
    QStringList results2 = idx.computeSuggestions(QStringLiteral("   "));
    CHECK(results2 == idx.commands());
}

static void testCommandIndexNonCommandContext()
{
    qInfo() << "--- testCommandIndexNonCommandContext ---";
    CommandIndex idx;
    idx.load(QStringLiteral("/nonexistent/path/fake.json"));

    // Multi-token but last token doesn't start with '-' → empty
    QStringList results = idx.computeSuggestions(QStringLiteral("ls foo"));
    CHECK(results.isEmpty());
}

static void testCommandIndexTruncation()
{
    qInfo() << "--- testCommandIndexTruncation ---";
    CommandIndex idx;
    idx.load(QStringLiteral("/nonexistent/path/fake.json"));

    // Any result set should be ≤80
    QStringList all = idx.computeSuggestions(QString());
    // The full commands list might exceed 80 when we return ALL commands directly
    // Actually, computeSuggestions returns m_commands directly for empty input (no truncation for empty)
    // But for prefix searches, it's ≤80
    QStringList single = idx.computeSuggestions(QStringLiteral("a"));
    CHECK(single.size() <= 80);
}

// ---------------------------------------------------------------------------
// CommandHistory tests
// ---------------------------------------------------------------------------

static void testCommandHistoryBasic()
{
    qInfo() << "--- testCommandHistoryBasic ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();
    hist.addEntry(QStringLiteral("ls -la"), QStringLiteral("myhost"));

    // File should exist after addEntry (which calls save)
    CHECK(QFile::exists(histPath));

    // Verify JSON structure
    QFile f(histPath);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    CHECK(doc.isObject());
    QJsonObject root = doc.object();
    CHECK(root.contains(QStringLiteral("global")));
    CHECK(root.contains(QStringLiteral("by_profile")));
    CHECK(root.value(QStringLiteral("global")).isArray());
    CHECK(root.value(QStringLiteral("by_profile")).isObject());
}

static void testCommandHistoryRoundTrip()
{
    qInfo() << "--- testCommandHistoryRoundTrip ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    {
        CommandHistory hist(histPath);
        hist.load();
        hist.addEntry(QStringLiteral("docker ps"), QStringLiteral("server1"));
        hist.addEntry(QStringLiteral("git status"), QStringLiteral("server1"));
        hist.addEntry(QStringLiteral("ls -la"), QStringLiteral("server2"));
    }

    // Reload in a new instance
    CommandHistory hist2(histPath);
    hist2.load();
    CHECK(hist2.globalHistory().contains(QStringLiteral("docker ps")));
    CHECK(hist2.globalHistory().contains(QStringLiteral("git status")));
    CHECK(hist2.globalHistory().contains(QStringLiteral("ls -la")));
    CHECK(hist2.profileHistory(QStringLiteral("server1")).contains(QStringLiteral("docker ps")));
    CHECK(hist2.profileHistory(QStringLiteral("server1")).contains(QStringLiteral("git status")));
    CHECK(hist2.profileHistory(QStringLiteral("server2")).contains(QStringLiteral("ls -la")));
}

static void testCommandHistoryDedup()
{
    qInfo() << "--- testCommandHistoryDedup ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();
    hist.addEntry(QStringLiteral("ls"), QStringLiteral("host1"));
    hist.addEntry(QStringLiteral("cd /tmp"), QStringLiteral("host1"));
    hist.addEntry(QStringLiteral("ls"), QStringLiteral("host1")); // duplicate

    // "ls" should appear only once and be at the front
    CHECK(hist.globalHistory().count(QStringLiteral("ls")) == 1);
    CHECK(hist.globalHistory().first() == QStringLiteral("ls"));
    CHECK(hist.profileHistory(QStringLiteral("host1")).count(QStringLiteral("ls")) == 1);
    CHECK(hist.profileHistory(QStringLiteral("host1")).first() == QStringLiteral("ls"));
}

static void testCommandHistoryTruncation()
{
    qInfo() << "--- testCommandHistoryTruncation ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();

    // Add 501 unique entries → global should be capped at 500
    for (int i = 0; i < 501; ++i) {
        hist.addEntry(QStringLiteral("cmd_%1").arg(i), QStringLiteral("prof"));
    }
    CHECK(hist.globalHistory().size() == 500);
    // Most recent should be first
    CHECK(hist.globalHistory().first() == QStringLiteral("cmd_500"));

    // Profile should be capped at 200
    QTemporaryDir tmpDir2;
    CHECK(tmpDir2.isValid());
    const QString histPath2 = tmpDir2.path() + QStringLiteral("/command_history.json");
    CommandHistory hist2(histPath2);
    hist2.load();
    for (int i = 0; i < 201; ++i) {
        hist2.addEntry(QStringLiteral("pcmd_%1").arg(i), QStringLiteral("oneprof"));
    }
    CHECK(hist2.profileHistory(QStringLiteral("oneprof")).size() == 200);
    CHECK(hist2.profileHistory(QStringLiteral("oneprof")).first() == QStringLiteral("pcmd_200"));
}

static void testCommandHistorySuggestions()
{
    qInfo() << "--- testCommandHistorySuggestions ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();
    // Add to global
    hist.addEntry(QStringLiteral("docker ps"), QStringLiteral("other"));
    hist.addEntry(QStringLiteral("docker images"), QStringLiteral("other"));
    // Add to profile "myprof"
    hist.addEntry(QStringLiteral("docker exec -it"), QStringLiteral("myprof"));
    hist.addEntry(QStringLiteral("docker logs"), QStringLiteral("myprof"));

    // Suggestions for "docker" with profile "myprof":
    // profile items first, then global items (deduped)
    QStringList sugg = hist.suggestions(QStringLiteral("docker"), QStringLiteral("myprof"));
    CHECK(!sugg.isEmpty());
    // Should not include exact match "docker" itself (none of our entries are exactly "docker")
    // Profile entries should appear before global-only ones
    int execIdx = sugg.indexOf(QStringLiteral("docker exec -it"));
    int logsIdx = sugg.indexOf(QStringLiteral("docker logs"));
    int psIdx = sugg.indexOf(QStringLiteral("docker ps"));
    int imagesIdx = sugg.indexOf(QStringLiteral("docker images"));
    CHECK(execIdx >= 0);
    CHECK(logsIdx >= 0);
    CHECK(psIdx >= 0);
    CHECK(imagesIdx >= 0);
    // Profile items come first
    CHECK(execIdx < psIdx || logsIdx < psIdx);

    // Skip equal prefix: add exact "docker" entry then check suggestions skip it
    hist.addEntry(QStringLiteral("docker"), QStringLiteral("myprof"));
    QStringList sugg2 = hist.suggestions(QStringLiteral("docker"), QStringLiteral("myprof"));
    CHECK(!sugg2.contains(QStringLiteral("docker")));

    // ≤20 results
    CHECK(sugg2.size() <= 20);

    // Empty prefix → empty result
    QStringList sugg3 = hist.suggestions(QString(), QStringLiteral("myprof"));
    CHECK(sugg3.isEmpty());
}

static void testCommandHistoryCorruptedFile()
{
    qInfo() << "--- testCommandHistoryCorruptedFile ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    // Write corrupted JSON
    {
        QFile f(histPath);
        f.open(QIODevice::WriteOnly);
        f.write("{ this is not valid json!!! @#$%^");
        f.close();
    }

    // load() should not crash and should fall back to empty
    CommandHistory hist(histPath);
    hist.load();
    CHECK(hist.globalHistory().isEmpty());
}

static void testCommandHistoryEmptyProfileKey()
{
    qInfo() << "--- testCommandHistoryEmptyProfileKey ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();
    // profileKey empty → falls into "global" group semantics
    hist.addEntry(QStringLiteral("whoami"), QString());

    // Both global and profileHistory("") should contain it
    CHECK(hist.globalHistory().contains(QStringLiteral("whoami")));
    // profileHistory with empty key maps to "global" key
    CHECK(hist.profileHistory(QString()).contains(QStringLiteral("whoami")));

    // Verify it's stored under "global" key in by_profile
    QFile f(histPath);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QJsonObject bp = doc.object().value(QStringLiteral("by_profile")).toObject();
    CHECK(bp.contains(QStringLiteral("global")));
}

// 多实例并发写入：同路径两个实例（各自旧快照）先后 addEntry，
// 后写不得覆盖前写（进程级共享数据 + 落盘前磁盘合并）。
static void testCommandHistoryMultiInstanceNoLostWrite()
{
    qInfo() << "--- testCommandHistoryMultiInstanceNoLostWrite ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    // 模拟两个终端 Tab：各持独立实例，构造时各自 load() 一次磁盘快照
    CommandHistory hist1(histPath);
    hist1.load();
    CommandHistory hist2(histPath);
    hist2.load();

    hist1.addEntry(QStringLiteral("A"), QStringLiteral("prof1"));
    hist2.addEntry(QStringLiteral("B"), QStringLiteral("prof2"));

    // 重新 load 文件：A 与 B 必须同时存在，B（后写）在前
    CommandHistory verify(histPath);
    verify.load();
    CHECK(verify.globalHistory().contains(QStringLiteral("A")));
    CHECK(verify.globalHistory().contains(QStringLiteral("B")));
    CHECK(verify.globalHistory().indexOf(QStringLiteral("B"))
          < verify.globalHistory().indexOf(QStringLiteral("A")));
    // profile 分组各自保留
    CHECK(verify.profileHistory(QStringLiteral("prof1")).contains(QStringLiteral("A")));
    CHECK(verify.profileHistory(QStringLiteral("prof2")).contains(QStringLiteral("B")));

    // 共享按路径隔离：另一路径的实例不得混入本路径的数据
    QTemporaryDir tmpDirOther;
    CHECK(tmpDirOther.isValid());
    CommandHistory other(tmpDirOther.path() + QStringLiteral("/command_history.json"));
    other.load();
    CHECK(other.globalHistory().isEmpty());
    other.addEntry(QStringLiteral("OTHER"), QString());
    CHECK(!verify.globalHistory().contains(QStringLiteral("OTHER")));
}

// 外部进程写入合并：实例 load 后磁盘被外部改写加入 X，
// 随后 addEntry(Y) 不得覆盖 X（落盘前重读磁盘为基底合并）。
static void testCommandHistoryExternalWriteMerge()
{
    qInfo() << "--- testCommandHistoryExternalWriteMerge ---";
    QTemporaryDir tmpDir;
    CHECK(tmpDir.isValid());
    const QString histPath = tmpDir.path() + QStringLiteral("/command_history.json");

    CommandHistory hist(histPath);
    hist.load();

    // 模拟 Python 版等外部进程直接改写文件，加入条目 X
    {
        QJsonObject root;
        root.insert(QStringLiteral("global"),
                    QJsonArray::fromStringList({QStringLiteral("X")}));
        QJsonObject bp;
        bp.insert(QStringLiteral("extprof"),
                  QJsonArray::fromStringList({QStringLiteral("X")}));
        root.insert(QStringLiteral("by_profile"), bp);
        QFile f(histPath);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }

    hist.addEntry(QStringLiteral("Y"), QStringLiteral("myprof"));

    // 最终文件须同时含 X 与 Y（Y 头插在前），外部 profile 分组保留
    QFile f(histPath);
    CHECK(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QStringList global;
    for (const QJsonValue &v : root.value(QStringLiteral("global")).toArray())
        global.append(v.toString());
    CHECK(global.contains(QStringLiteral("X")));
    CHECK(global.contains(QStringLiteral("Y")));
    CHECK(global.first() == QStringLiteral("Y"));
    const QJsonObject bp = root.value(QStringLiteral("by_profile")).toObject();
    CHECK(bp.contains(QStringLiteral("extprof")));
    CHECK(bp.contains(QStringLiteral("myprof")));
}

// ---------------------------------------------------------------------------
// extractCommandFromPrompt tests
// ---------------------------------------------------------------------------

static void testExtractCommandFromPrompt()
{
    qInfo() << "--- testExtractCommandFromPrompt ---";

    // Standard bash prompt
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(
        QStringLiteral("user@host:~$ docker ps")) == QStringLiteral("docker ps"));

    // Root prompt with #
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(
        QStringLiteral("root@host:/tmp# ls -l")) == QStringLiteral("ls -l"));

    // Fancy prompt with ❯
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(
        QStringLiteral("❯ git status")) == QStringLiteral("git status"));

    // Fancy prompt with >
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(
        QStringLiteral("PS1> echo hello")) == QStringLiteral("echo hello"));

    // No marker → return trimmed
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(
        QStringLiteral("  ls -la  ")) == QStringLiteral("ls -la"));

    // Empty → empty
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(QString()).isEmpty());

    // Only whitespace/newlines
    CHECK(TerminalCommandSuggest::extractCommandFromPrompt(QStringLiteral("\n\r\n")).isEmpty());
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // CommandIndex tests
    testCommandIndexBuiltinOnly();
    testCommandIndexWithJson();
    testCommandIndexPrefixMatch();
    testCommandIndexOptionMatch();
    testCommandIndexEmptyInput();
    testCommandIndexNonCommandContext();
    testCommandIndexTruncation();

    // CommandHistory tests
    testCommandHistoryBasic();
    testCommandHistoryRoundTrip();
    testCommandHistoryDedup();
    testCommandHistoryTruncation();
    testCommandHistorySuggestions();
    testCommandHistoryCorruptedFile();
    testCommandHistoryEmptyProfileKey();
    testCommandHistoryMultiInstanceNoLostWrite();
    testCommandHistoryExternalWriteMerge();

    // extractCommandFromPrompt tests
    testExtractCommandFromPrompt();

    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
