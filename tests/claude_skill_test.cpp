// claude_skill_test.cpp — 临时目录中验证本地存储与真实 POSIX 远程协议。
// 不读取真实 ~/.claude，不运行 Claude，不连接真实 SSH，也不执行导入的脚本。
#include "claude_code/ClaudeSkillData.h"
#include "claude_code/ClaudeSkillStore.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

using namespace cubeshell;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { qWarning() << "FAIL:" << #c << "line" << __LINE__; ++failures; } } while (0)
static void put(const QString &path, const QByteArray &bytes)
{
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write(bytes) == bytes.size());
}
static QByteArray get(const QString &path)
{
    QFile f(path); CHECK(f.open(QIODevice::ReadOnly)); return f.readAll();
}
static ClaudeSkillBundle fixture()
{
    return {{"SKILL.md", QByteArray("---\nname: example\ndescription: |\n  first line\n  中文描述\ndisable-model-invocation: true\nuser-invocable: true\ncustom-field: [keep, me]\n---\n\n# Instructions\n\nDo not execute scripts during import.\n"), false, false},
            {"references", {}, true, false},
            {"references/notes ' 中文.md", QByteArray("exact\r\nbytes\r\n"), false, false},
            {"scripts/run.sh", QByteArray("#!/bin/sh\nprintf 'MUST NOT RUN'\n"), false, true},
            {"assets/data.bin", QByteArray::fromHex("00ff807f1020304050"), false, false},
            {"empty", {}, true, false},
            {".hidden", QByteArray("hidden"), false, false}};
}
static void parserTests()
{
    const auto document = fixture().first().bytes;
    const auto info = skills::parse("fallback", document);
    CHECK(info.name == "example");
    CHECK(info.description.contains(QStringLiteral("中文描述")));
    CHECK(info.manualOnly && info.userInvocable);
    CHECK(info.content == document);
    CHECK(info.error.isEmpty());
    CHECK(skills::parse("fallback", "# plain markdown\n").name == "fallback");
    CHECK(skills::parse("fallback", "---\r\nname: windows\r\n---\r\nbody").name == "windows");
    CHECK(!skills::validateDocument("---\nname: [oops\n---\nbody").isEmpty());
    CHECK(!skills::validateDocument("---\nname: missing-end\n").isEmpty());
    CHECK(!skills::validateDocument("---\nname: one\nname: two\n---\n").isEmpty());
    CHECK(!skills::validateDocument(QByteArray::fromHex("fffe00")).isEmpty());
    CHECK(!skills::validateDocument(QByteArray(skills::maxMarkdownBytes + 1, 'x')).isEmpty());
    CHECK(!skills::validateDocument(" \n").isEmpty());
    CHECK(skills::validNewName("review-2"));
    for (const QString name : {"", "../escape", "a/b", "a\\b", ".", "..", "-option", "UPPER", "synced", "con", "a:b"})
        CHECK(!skills::validNewName(name));
    CHECK(skills::validComponent("legacy name '中文"));
    CHECK(!skills::validComponent("evil\nline"));
    CHECK(!skills::validRelativePath("../../outside"));
    CHECK(!skills::validRelativePath("/absolute"));
    CHECK(!skills::validRelativePath("C:/absolute"));
    auto bad = fixture(); bad.append({"../outside", "x", false, false});
    CHECK(!skills::validateBundle(bad).isEmpty());
    bad = fixture(); bad.append({"skill.md", "x", false, false});
    CHECK(!skills::validateBundle(bad).isEmpty());
    bad = fixture(); bad.append({"references", "file conflicts with directory", false, false});
    CHECK(!skills::validateBundle(bad).isEmpty());
    CHECK(skills::validateBundle(fixture()).isEmpty());
    for (const QString path : {"NUL.txt", "assets/bad?name", "assets/end.", "assets/space "}) {
        bad = fixture(); bad.append({path, "x", false, false});
        CHECK(!skills::validateBundle(bad).isEmpty());
    }
    bad = fixture(); bad.append({"huge.bin", QByteArray(skills::maxFileBytes + 1, 'x'), false, false});
    CHECK(!skills::validateBundle(bad).isEmpty());
    bad = fixture();
    for (int i = 0; i < skills::maxFiles; ++i) bad.append({QString::number(i), {}, false, false});
    CHECK(!skills::validateBundle(bad).isEmpty());
}
static void exercise(ClaudeSkillStore &store, const ClaudeSkillScope &scope, const QString &root, const QString &out)
{
    QString error, resolved;
    CHECK(store.list(scope, &resolved, &error).isEmpty()); CHECK(error.isEmpty()); CHECK(resolved == root);
    CHECK(store.create(scope, "example", fixture(), &error)); CHECK(error.isEmpty());
    CHECK(!store.create(scope, "example", fixture(), &error)); CHECK(!error.isEmpty());
    CHECK(!store.create(scope, "../bad", fixture(), &error));
    auto list = store.list(scope, &resolved, &error); CHECK(error.isEmpty()); CHECK(list.size() == 1);
    if (list.size() != 1) return;
    auto info = store.read(scope, "example", true, &error);
    CHECK(error.isEmpty()); CHECK(info.content == fixture().first().bytes); CHECK(info.manualOnly);
    const auto files = store.files(scope, info, &error); CHECK(error.isEmpty());
    CHECK(files.contains("scripts/run.sh")); CHECK(files.contains("empty/")); CHECK(files.contains(".hidden"));
    CHECK(get(root + "/skills/example/assets/data.bin") == fixture()[4].bytes);
    CHECK(get(root + "/skills/example/references/notes ' 中文.md") == fixture()[2].bytes);
#if defined(Q_OS_UNIX)
    CHECK(QFileInfo(root + "/skills/example/scripts/run.sh").permissions().testFlag(QFileDevice::ExeOwner));
#endif
    const auto changed = info.content + QByteArray("\nnew content 中文\n");
    CHECK(store.save(scope, info, changed, &error)); CHECK(error.isEmpty());
    CHECK(get(root + "/skills/example/SKILL.md") == changed);
    CHECK(!store.save(scope, info, info.content, &error)); CHECK(error.contains(QStringLiteral("外部修改")));
    CHECK(get(root + "/skills/example/SKILL.md") == changed);
    info = store.read(scope, "example", true, &error);
    CHECK(!store.save(scope, info, "---\nname: [broken\n---\n", &error));
    CHECK(get(root + "/skills/example/SKILL.md") == changed);

    CHECK(store.setEnabled(scope, info, false, &error)); CHECK(error.isEmpty());
    CHECK(!QFileInfo::exists(root + "/skills/example"));
    CHECK(get(root + "/skills-disabled/example/SKILL.md") == changed);
    CHECK(!store.create(scope, "example", fixture(), &error));
    auto disabled = store.read(scope, "example", false, &error);
    CHECK(!disabled.enabled); CHECK(disabled.content == changed);
    CHECK(store.exportSkill(scope, disabled, out + "/exported", &error)); CHECK(error.isEmpty());
    CHECK(get(out + "/exported/assets/data.bin") == fixture()[4].bytes);
    CHECK(QFileInfo(out + "/exported/empty").isDir());
    CHECK(!store.exportSkill(scope, disabled, out + "/exported", &error));
    CHECK(!store.exportSkill(scope, disabled, out + "/missing-parent/export", &error));
    CHECK(!QFileInfo::exists(out + "/missing-parent"));
    if (!store.isRemote()) {
        CHECK(!store.exportSkill(scope, disabled, root + "/skills-disabled/example/nested-export", &error));
        CHECK(!QFileInfo::exists(root + "/skills-disabled/example/nested-export"));
    }
    CHECK(store.importSkill(scope, "imported", out + "/exported", &error)); CHECK(error.isEmpty());
    CHECK(get(root + "/skills/imported/SKILL.md") == changed);
    CHECK(store.importSkill(scope, "single", out + "/exported/SKILL.md", &error)); CHECK(error.isEmpty());
    CHECK(!QFileInfo::exists(root + "/skills/single/scripts"));

    // 启用目标冲突必须拒绝，不把源目录嵌套移动进已有目录。
    put(root + "/skills/example/SKILL.md", "keep target\n");
    CHECK(!store.setEnabled(scope, disabled, true, &error));
    CHECK(get(root + "/skills/example/SKILL.md") == "keep target\n");
    CHECK(get(root + "/skills-disabled/example/SKILL.md") == changed);
    CHECK(QDir(root + "/skills/example").removeRecursively());
    CHECK(store.setEnabled(scope, disabled, true, &error));
    CHECK(QFileInfo::exists(root + "/skills/example/scripts/run.sh"));

    // 已有名称不受新建命名规范限制，保存不改变目录和未知 YAML 字段。
    const auto legacy = root + "/skills/legacy ' 中文";
    put(legacy + "/SKILL.md", "---\ncustom: original\n---\r\nraw\r\n");
    auto old = store.read(scope, "legacy ' 中文", true, &error);
    CHECK(old.name == "legacy ' 中文"); CHECK(error.isEmpty());
    CHECK(store.save(scope, old, old.content, &error));
    CHECK(get(legacy + "/SKILL.md") == old.content);
    CHECK(store.remove(scope, old, &error));

    // 异常 YAML 仍出现在列表，可以原文修复。
    put(root + "/skills/broken/SKILL.md", "---\nname: [broken\n---\nbody");
    auto broken = store.read(scope, "broken", true, &error);
    CHECK(!broken.error.isEmpty()); CHECK(!broken.readOnly);
    CHECK(store.save(scope, broken, "# repaired\n", &error));
    CHECK(get(root + "/skills/broken/SKILL.md") == "# repaired\n");
    put(root + "/skills/oversize/SKILL.md", QByteArray(skills::maxMarkdownBytes + 1, 'x'));
    const auto oversize = store.read(scope, "oversize", true, &error);
    CHECK(!oversize.error.isEmpty()); CHECK(oversize.readOnly);
    const auto withOversize = store.list(scope, &resolved, &error);
    CHECK(error.isEmpty()); CHECK(!withOversize.isEmpty());
    CHECK(QDir(root + "/skills/oversize").removeRecursively());

#if defined(Q_OS_UNIX)
    const auto outside = out + "/outside";
    put(outside + "/SKILL.md", "# outside\n");
    CHECK(QFile::link(outside, root + "/skills/linked"));
    auto linked = store.read(scope, "linked", true, &error);
    CHECK(linked.readOnly); CHECK(linked.content == "# outside\n");
    CHECK(!store.save(scope, linked, "bad", &error));
    CHECK(!store.setEnabled(scope, linked, false, &error));
    CHECK(!store.remove(scope, linked, &error));
    // 即便调用方伪造只读标志，存储也会重新检测真实链接。
    linked.readOnly = false;
    CHECK(!store.remove(scope, linked, &error));
    CHECK(get(outside + "/SKILL.md") == "# outside\n");
    CHECK(!store.importSkill(scope, "link-copy", root + "/skills/linked", &error));
    CHECK(QFile::link(outside, root + "/skills/imported/external"));
    auto imported = store.read(scope, "imported", true, &error);
    CHECK(!store.exportSkill(scope, imported, out + "/with-link", &error));
    CHECK(!QFileInfo::exists(out + "/with-link"));
    CHECK(!store.importSkill(scope, "contains-link", root + "/skills/imported", &error));
    CHECK(!QFileInfo::exists(root + "/skills/contains-link"));
    CHECK(store.remove(scope, imported, &error)); // 只删除目录内的链接，不跟随到目标
    CHECK(get(outside + "/SKILL.md") == "# outside\n");
#endif
    info = store.read(scope, "example", true, &error);
    auto forged = info; forged.directory = "../../outside";
    CHECK(!store.remove(scope, forged, &error));
    CHECK(store.remove(scope, info, &error)); CHECK(error.isEmpty());
    CHECK(!QFileInfo::exists(root + "/skills/example"));
    CHECK(QFileInfo::exists(root + "/skills/single/SKILL.md"));
    CHECK(store.list({"project", ""}, &resolved, &error).isEmpty()); CHECK(!error.isEmpty());
    CHECK(!store.create({"project", "relative"}, "bad", fixture(), &error));
    CHECK(!store.create({"invalid", ""}, "bad", fixture(), &error));
}
static void localTests()
{
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const auto home = temp.path() + "/local ' 中文/.claude";
    CHECK(QDir().mkpath(temp.path() + "/out"));
    ClaudeSkillStore store(home);
    exercise(store, {}, home, temp.path() + "/out");
    QString error, resolved;
    const auto project = temp.path() + "/project ; '$()";
    CHECK(QDir().mkpath(project));
    CHECK(store.create({"project", project}, "project-skill", fixture(), &error));
    CHECK(QFileInfo::exists(project + "/.claude/skills/project-skill/SKILL.md"));
    CHECK(!QFileInfo::exists(home + "/skills/project-skill"));
    store.cancel(); CHECK(store.list({}, &resolved, &error).isEmpty()); CHECK(!error.isEmpty());
    CHECK(!store.create({}, "cancelled", fixture(), &error));
    ClaudeSkillStore disconnected(std::shared_ptr<SshClient>{});
    CHECK(disconnected.list({}, &resolved, &error).isEmpty()); CHECK(!error.isEmpty());
}
static void remoteTests()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const auto home = temp.path() + "/remote ' ; $(touch NOT-RUN) 中文/.claude";
    CHECK(QDir().mkpath(temp.path() + "/out"));
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("CLAUDE_CONFIG_DIR", home);
    env.insert("HOME", temp.path());
    auto run = [env](const QString &command, const QByteArray &input, QString *error) {
        QProcess p;
        p.setProcessEnvironment(env);
        p.start("/bin/sh", {"-c", command});
        if (!p.waitForStarted(5000)) { *error = p.errorString(); return QByteArray{}; }
        p.write(input); p.closeWriteChannel();
        if (!p.waitForFinished(20000)) { p.kill(); p.waitForFinished(); *error = "timeout"; }
        if (p.exitCode() != 0) {
            *error = QString::fromUtf8(p.readAllStandardError());
            if (error->isEmpty()) *error = QStringLiteral("shell exited %1").arg(p.exitCode());
        }
        return p.readAllStandardOutput();
    };
    ClaudeSkillStore store{ClaudeSkillStore::RemoteCommand(run)};
    exercise(store, {}, home, temp.path() + "/out");
    QString error;
    CHECK(QDir().mkpath(temp.path() + "/project '中文"));
    CHECK(store.create({"project", "~/project '中文"}, "project-skill", fixture(), &error));
    if (!error.isEmpty()) qWarning() << error;
    CHECK(QFileInfo::exists(temp.path() + "/project '中文/.claude/skills/project-skill/SKILL.md"));
    CHECK(!QFileInfo::exists(home + "/skills/project-skill"));
    // 模拟传输损坏：临时文件可失败，不能发布半份目录或覆盖现有原文。
    ClaudeSkillStore brokenTransfer{ClaudeSkillStore::RemoteCommand([&run](const QString &command, const QByteArray &input, QString *outError) {
        auto damaged = input;
        if (!damaged.isEmpty()) damaged[0] = '!';
        return run(command, damaged, outError);
    })};
    CHECK(!brokenTransfer.create({}, "broken-transfer", fixture(), &error));
    CHECK(!QFileInfo::exists(home + "/skills/broken-transfer"));
    CHECK(QDir(home + "/skills").entryList({".cube-skill-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    const auto original = store.read({}, "single", true, &error);
    CHECK(!brokenTransfer.save({}, original, "# should not be committed\n", &error));
    CHECK(get(home + "/skills/single/SKILL.md") == original.content);
    CHECK(QDir(home + "/skills/single").entryList({".cube-edit-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    store.cancel();
    CHECK(!store.create({}, "cancelled", fixture(), &error));
    qInfo() << "POSIX remote Skills protocol exercised against temporary directories";
#endif
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    parserTests(); localTests(); remoteTests();
    qInfo() << "claude_skill_test failures:" << failures;
    return failures ? 1 : 0;
}
