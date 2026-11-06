#include "claude_code/ClaudeSkillStore.h"
#include "claude_code/ClaudeSessionData.h"
#include "ssh/CommandExecutor.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>

namespace cubeshell {
namespace {
QString quote(const QString &s) { return ClaudeSessionData::quote(s); }
QString slot(bool enabled) { return enabled ? QStringLiteral("skills") : QStringLiteral("skills-disabled"); }
const QString prelude = QStringLiteral(
    "set -e; fail() { printf '%s\\n' \"$*\" >&2; exit 1; }; "
    "if printf '' | base64 -d >/dev/null 2>&1; then decode() { base64 -d; }; "
    "else decode() { base64 -D; }; fi; ");
bool exists(const QString &path) { const QFileInfo f(path); return f.exists() || f.isSymLink(); }
bool safeAbsolute(const QString &path, bool remote)
{
    if (path.isEmpty()) return false;
    for (QChar c : path) if (c.unicode() < 32 || c.unicode() == 127) return false;
    return remote ? path.startsWith(QLatin1Char('/')) || path.startsWith(QStringLiteral("~/"))
                  : QDir::isAbsolutePath(path);
}
QByteArray loadFile(const QString &path, qint64 limit, QString *error)
{
    QFile f(path);
    if (!QFileInfo(path).isFile() || !f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取文件：%1 (%2)").arg(path, f.errorString());
        return {};
    }
    if (f.size() > limit) {
        *error = QStringLiteral("文件超过大小限制：%1").arg(path);
        return {};
    }
    const auto bytes = f.read(limit + 1);
    if (f.error() != QFileDevice::NoError || bytes.size() > limit)
        *error = QStringLiteral("文件读取失败或读取期间超过大小限制：%1").arg(path);
    return bytes;
}
bool writeFile(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        *error = QStringLiteral("无法保存 %1：%2").arg(path, f.errorString());
        return false;
    }
    return true;
}
// 输出一条固定字段的记录，内容、名称均经 Base64，不把文件名当成 shell 代码。
QString emitDocument()
{
    return QStringLiteral(
        "printf '%s' \"$name\" | base64 | tr -d '\\r\\n'; "
        "ro=0; [ ! -L \"$base\" ] && [ ! -L \"$parent\" ] && [ ! -L \"$d\" ] && [ ! -L \"$d/SKILL.md\" ] || ro=1; "
        "printf '\\t%s\\t%s\\t' \"$enabled\" \"$ro\"; "
        "if [ -f \"$d/SKILL.md\" ] && [ -r \"$d/SKILL.md\" ]; then "
        "size=$(wc -c < \"$d/SKILL.md\"); "
        "if [ \"$size\" -le 1048576 ]; then total=$((total + size)); "
        "[ \"$total\" -le 67108864 ] || fail '技能原文总量超过 64 MiB'; "
        "printf 'OK\\t'; dd if=\"$d/SKILL.md\" bs=65536 count=17 2>/dev/null | base64 | tr -d '\\r\\n'; "
        "else printf 'LARGE\\t'; fi; else printf 'UNREADABLE\\t'; fi; printf '\\n'; ");
}
ClaudeSkillInfo parseRecord(const QByteArray &line, const QString &base)
{
    const auto fields = line.split('\t');
    if (fields.size() != 5) return {};
    const auto name = QString::fromUtf8(QByteArray::fromBase64(fields[0]));
    if (!skills::validComponent(name)) return {};
    auto info = skills::parse(name, QByteArray::fromBase64(fields[4]));
    info.enabled = fields[1] == "1";
    info.readOnly = fields[2] == "1" || info.content.size() > skills::maxMarkdownBytes;
    info.path = base + QLatin1Char('/') + slot(info.enabled) + QLatin1Char('/') + name;
    if (fields[3] != "OK") {
        info.error = fields[3] == "LARGE" ? QStringLiteral("SKILL.md 超过 1 MiB")
                                          : QStringLiteral("SKILL.md 缺失、不是普通文件或不可读");
        info.readOnly = true;
    }
    return info;
}
}

ClaudeSkillStore::ClaudeSkillStore(const QString &home) : m_home(home) {}
ClaudeSkillStore::ClaudeSkillStore(std::shared_ptr<SshClient> client) : m_remote(true), m_client(std::move(client)) {}
ClaudeSkillStore::ClaudeSkillStore(RemoteCommand command) : m_remote(true), m_command(std::move(command)) {}

bool ClaudeSkillStore::stopped(QString *error) const
{
    if (!m_cancelled.load()) return false;
    *error = QStringLiteral("操作已取消，数据源已切换或面板已关闭");
    return true;
}
QByteArray ClaudeSkillStore::command(const QString &script, const QByteArray &input, QString *error)
{
    if (stopped(error)) return {};
    if (m_command) return m_command(prelude + script, input, error);
    if (!m_client) { *error = QStringLiteral("SSH 连接不可用，未操作本地文件"); return {}; }
    const auto r = CommandExecutor::runCommand(m_client.get(), prelude + script, false, 30000, input, &m_cancelled);
    if (!r.ok() || r.exitCode != 0) {
        *error = !r.errorMessage.isEmpty() ? r.errorMessage : r.timedOut ? QStringLiteral("远程 Skills 操作超时")
            : r.cancelled ? QStringLiteral("远程 Skills 操作已取消") : r.stderrText.trimmed();
        if (error->isEmpty()) *error = QStringLiteral("远程 Skills 操作失败（退出码 %1）").arg(r.exitCode);
        return {};
    }
    return r.stdoutText.toUtf8();
}
QString ClaudeSkillStore::root(const ClaudeSkillScope &scope, QString *error)
{
    if (stopped(error)) return {};
    if (scope.scope != QLatin1String("user") && scope.scope != QLatin1String("project")) {
        *error = QStringLiteral("无效的 Skills 作用域"); return {};
    }
    const bool project = scope.scope == QLatin1String("project");
    QString path = scope.projectPath;
    if (!m_remote && path.startsWith(QStringLiteral("~/"))) path = QDir::homePath() + path.mid(1);
    if (project && !safeAbsolute(path, m_remote)) {
        *error = QStringLiteral("请指定有效的项目绝对路径（可使用 ~/）"); return {};
    }
    if (m_remote) {
        QString script;
        if (project) {
            script = QStringLiteral("base=%1; case \"$base\" in '~/'*) base=\"$HOME/${base#\\~/}\";; esac; "
                "[ -d \"$base\" ] || fail '项目目录不存在'; base=\"$base/.claude\"; ").arg(quote(path));
        } else {
            script = QStringLiteral("base=\"${CLAUDE_CONFIG_DIR:-$HOME/.claude}\"; "
                "case \"$base\" in '~/'*) base=\"$HOME/${base#\\~/}\";; esac; ");
        }
        script += QStringLiteral("case \"$base\" in /*) ;; *) base=\"$PWD/$base\";; esac; printf '%s' \"$base\" | base64");
        const auto output = command(script, {}, error);
        if (!error->isEmpty()) return {};
        path = QString::fromUtf8(QByteArray::fromBase64(output));
        if (!safeAbsolute(path, true)) { *error = QStringLiteral("远程配置目录无效"); return {}; }
        return QDir::cleanPath(path);
    }
    if (project && !QFileInfo(path).isDir()) { *error = QStringLiteral("项目目录不存在"); return {}; }
    path = project ? path + QStringLiteral("/.claude") : m_home;
    if (path.isEmpty()) { *error = QStringLiteral("配置目录不能为空"); return {}; }
    return QDir(path).absolutePath();
}
QString ClaudeSkillStore::skillPath(const QString &base, const QString &directory, bool enabled) const
{
    return base + QLatin1Char('/') + slot(enabled) + QLatin1Char('/') + directory;
}
ClaudeSkillInfo ClaudeSkillStore::readAt(const QString &base, const QString &directory, bool enabled, QString *error)
{
    if (!skills::validComponent(directory)) { *error = QStringLiteral("无效的技能目录名"); return {}; }
    const auto path = skillPath(base, directory, enabled);
    if (m_remote) {
        const auto script = QStringLiteral("base=%1; parent=%2; d=%3; name=%4; enabled=%5; total=0; ")
            .arg(quote(base), quote(base + QLatin1Char('/') + slot(enabled)), quote(path), quote(directory), enabled ? "1" : "0")
            + emitDocument();
        const auto records = command(script, {}, error).split('\n');
        if (!error->isEmpty()) return {};
        const auto info = parseRecord(records.value(0), base);
        if (info.directory.isEmpty()) *error = QStringLiteral("远程技能返回格式无效");
        return info;
    }
    QString readError;
    auto info = skills::parse(directory, loadFile(path + QStringLiteral("/SKILL.md"), skills::maxMarkdownBytes, &readError));
    info.enabled = enabled;
    info.path = path;
    info.readOnly = QFileInfo(base).isSymLink() || QFileInfo(QFileInfo(path).absolutePath()).isSymLink()
        || QFileInfo(path).isSymLink() || QFileInfo(path + QStringLiteral("/SKILL.md")).isSymLink();
    if (!readError.isEmpty()) { info.error = readError; info.readOnly = true; }
    return info;
}
ClaudeSkillInfo ClaudeSkillStore::read(const ClaudeSkillScope &scope, const QString &directory, bool enabled, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    return error->isEmpty() ? readAt(base, directory, enabled, error) : ClaudeSkillInfo{};
}
QList<ClaudeSkillInfo> ClaudeSkillStore::list(const ClaudeSkillScope &scope, QString *rootOut, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    *rootOut = base;
    QList<ClaudeSkillInfo> result;
    if (!error->isEmpty()) return result;
    if (m_remote) {
        const auto script = QStringLiteral("base=%1; total=0; count=0; ").arg(quote(base)) + QStringLiteral(
            "for enabled in 1 0; do if [ \"$enabled\" = 1 ]; then parent=\"$base/skills\"; else parent=\"$base/skills-disabled\"; fi; "
            "[ -e \"$parent\" ] || [ -L \"$parent\" ] || continue; "
            "[ -d \"$parent\" ] && [ -r \"$parent\" ] && [ -x \"$parent\" ] || fail '无法读取技能目录'; "
            "for d in \"$parent\"/* \"$parent\"/.[!.]* \"$parent\"/..?*; do "
            "[ -d \"$d\" ] || [ -L \"$d\" ] || continue; name=${d##*/}; "
            "count=$((count + 1)); [ \"$count\" -le 2048 ] || fail '技能数量超过 2048'; ")
            + emitDocument() + QStringLiteral("done; done");
        const auto output = command(script, {}, error);
        if (!error->isEmpty()) return result;
        for (const auto &line : output.split('\n')) {
            if (line.isEmpty()) continue;
            auto info = parseRecord(line, base);
            if (info.directory.isEmpty()) { *error = QStringLiteral("存在无效的技能目录或远程返回记录"); continue; }
            result.append(info);
        }
    } else {
        qint64 total = 0;
        for (bool enabled : {true, false}) {
            const auto parent = base + QLatin1Char('/') + slot(enabled);
            if (!exists(parent)) continue;
            if (!QFileInfo(parent).isDir() || !QFileInfo(parent).isReadable()) {
                *error = QStringLiteral("无法读取技能目录：%1").arg(parent); continue;
            }
            for (const auto &dir : QDir(parent).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
                if (stopped(error)) return {};
                if (!dir.isDir() && !dir.isSymLink()) continue;
                if (result.size() >= skills::maxSkills) { *error = QStringLiteral("技能数量超过 2048"); return {}; }
                if (!skills::validComponent(dir.fileName())) { *error = QStringLiteral("存在无效的技能目录名"); continue; }
                QString readError;
                auto info = readAt(base, dir.fileName(), enabled, &readError);
                total += info.content.size();
                if (total > skills::maxBundleBytes) { *error = QStringLiteral("技能原文总量超过 64 MiB"); return {}; }
                result.append(info);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.name == b.name ? a.key() < b.key() : a.name < b.name;
    });
    return result;
}

QString ClaudeSkillStore::remoteGuard(const QString &base, const ClaudeSkillInfo &info) const
{
    const auto path = skillPath(base, info.directory, info.enabled);
    return QStringLiteral("base=%1; parent=%2; d=%3; "
        "[ ! -L \"$base\" ] && [ ! -L \"$parent\" ] && [ ! -L \"$d\" ] && [ ! -L \"$d/SKILL.md\" ] || fail '符号链接技能只读'; "
        "[ -d \"$d\" ] || fail '技能目录已不存在'; ")
        .arg(quote(base), quote(base + QLatin1Char('/') + slot(info.enabled)), quote(path));
}
bool ClaudeSkillStore::writable(const QString &base, const ClaudeSkillInfo &info, QString *error)
{
    if (stopped(error)) return false;
    if (!skills::validComponent(info.directory) || info.readOnly) {
        *error = QStringLiteral("技能只读或目录名无效"); return false;
    }
    if (m_remote) return true; // 每个写命令在同一远程 shell 内重新检查，不依赖旧列表的权限信息
    const auto path = skillPath(base, info.directory, info.enabled);
    if (!QFileInfo(path).isDir()) { *error = QStringLiteral("技能目录已不存在"); return false; }
    for (const auto &p : {base, base + QLatin1Char('/') + slot(info.enabled), path, path + QStringLiteral("/SKILL.md")}) {
        if (QFileInfo(p).isSymLink()) { *error = QStringLiteral("符号链接技能只读"); return false; }
    }
    return true;
}

// 不跟随目录链接；浏览时标记链接，打包时拒绝，避免导入/导出整个外部目录。
bool ClaudeSkillStore::inventory(const QString &path, ClaudeSkillBundle *bundle, bool bytes, QString *error)
{
    qint64 total = 0;
    if (m_remote) {
        const auto script = QStringLiteral("start=%1; [ -d \"$start\" ] || fail '技能目录不存在'; ").arg(quote(path))
            + QStringLiteral(
            "walk() ( [ \"$2\" -le 64 ] || fail '目录嵌套超过 64 层'; "
            "[ -r \"$1\" ] && [ -x \"$1\" ] || fail '资源目录不可读'; "
            "for p in \"$1\"/* \"$1\"/.[!.]* \"$1\"/..?*; do "
            "[ -e \"$p\" ] || [ -L \"$p\" ] || continue; "
            "rel=${p#\"$start\"/}; printf '%s' \"$rel\" | base64 | tr -d '\\r\\n'; "
            "if [ -L \"$p\" ]; then printf '\\tL\\t0\\t0\\n'; "
            "elif [ -d \"$p\" ]; then printf '\\tD\\t0\\t0\\n'; walk \"$p\" $(( $2 + 1 )); "
            "elif [ -f \"$p\" ]; then x=0; [ ! -x \"$p\" ] || x=1; size=$(wc -c < \"$p\"); printf '\\tF\\t%s\\t%s\\n' \"$x\" \"$size\"; "
            "else printf '\\tS\\t0\\t0\\n'; fi; done ); "
            "{ walk \"$start\" 0; printf '__SKILL_MANIFEST_DONE__\\n'; } | "
            "( count=0; complete=0; while IFS= read -r record; do "
            "if [ \"$record\" = __SKILL_MANIFEST_DONE__ ]; then complete=1; continue; fi; "
            "count=$((count + 1)); [ \"$count\" -le 2048 ] || fail '技能资源超过 2048 项'; "
            "printf '%s\\n' \"$record\"; done; [ \"$complete\" = 1 ] || fail '资源清单读取不完整' )");
        const auto output = command(script, {}, error);
        if (!error->isEmpty()) return false;
        for (const auto &line : output.split('\n')) {
            if (line.isEmpty()) continue;
            if (bundle->size() >= skills::maxFiles) { *error = QStringLiteral("技能资源超过 2048 项"); return false; }
            const auto parts = line.split('\t');
            if (parts.size() != 4) { *error = QStringLiteral("资源清单格式无效"); return false; }
            ClaudeSkillFile f;
            f.path = QString::fromUtf8(QByteArray::fromBase64(parts[0]));
            if (!skills::validRelativePath(f.path)) { *error = QStringLiteral("资源路径无效：%1").arg(f.path); return false; }
            if (parts[1] == "L" || parts[1] == "S") {
                if (bytes) { *error = QStringLiteral("不能打包符号链接或特殊文件：%1").arg(f.path); return false; }
                f.path += QStringLiteral(" [链接/特殊文件，只读]");
            }
            f.directory = parts[1] == "D";
            f.executable = parts[2] == "1";
            const auto size = parts[3].toLongLong();
            total += size;
            if (bytes && (size > skills::maxFileBytes || total > skills::maxBundleBytes)) {
                *error = QStringLiteral("技能超过文件 16 MiB / 总计 64 MiB 限制"); return false;
            }
            if (bytes && !f.directory) {
                // 再次检查大小/链接，限制读取量，防止清单枚举后文件变化。
                const auto cmd = QStringLiteral("f=%1; [ -f \"$f\" ] && [ ! -L \"$f\" ] || fail '资源已变化'; "
                    "size=$(wc -c < \"$f\"); [ \"$size\" -le 16777216 ] || fail '资源超过 16 MiB'; "
                    "dd if=\"$f\" bs=65536 count=257 2>/dev/null | base64").arg(quote(path + QLatin1Char('/') + f.path));
                f.bytes = QByteArray::fromBase64(command(cmd, {}, error));
                if (!error->isEmpty()) return false;
                if (f.bytes.size() != size) { *error = QStringLiteral("读取期间资源已变化，请重试"); return false; }
            }
            bundle->append(f);
        }
        return true;
    }
    if (!QFileInfo(path).isDir() || !QFileInfo(path).isReadable()) { *error = QStringLiteral("技能目录不可读"); return false; }
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (stopped(error)) return false;
        it.next();
        if (bundle->size() >= skills::maxFiles) { *error = QStringLiteral("技能资源超过 2048 项"); return false; }
        const auto fi = it.fileInfo();
        ClaudeSkillFile f;
        f.path = QDir(path).relativeFilePath(fi.filePath());
        if (!skills::validRelativePath(f.path)) { *error = QStringLiteral("资源路径无效：%1").arg(f.path); return false; }
        if (fi.isSymLink() || (!fi.isFile() && !fi.isDir())) {
            if (bytes) { *error = QStringLiteral("不能打包符号链接或特殊文件：%1").arg(f.path); return false; }
            f.path += QStringLiteral(" [链接/特殊文件，只读]");
        } else {
            f.directory = fi.isDir();
            if (f.directory && !fi.isReadable()) { *error = QStringLiteral("资源子目录不可读：%1").arg(f.path); return false; }
            f.executable = fi.permissions().testFlag(QFileDevice::ExeOwner);
            if (bytes && !f.directory) {
                f.bytes = loadFile(fi.filePath(), skills::maxFileBytes, error);
                if (!error->isEmpty()) return false;
                total += f.bytes.size();
                if (total > skills::maxBundleBytes) { *error = QStringLiteral("技能目录超过 64 MiB"); return false; }
            }
        }
        bundle->append(f);
    }
    return true;
}
QStringList ClaudeSkillStore::files(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    if (!error->isEmpty() || !skills::validComponent(info.directory)) {
        if (error->isEmpty()) *error = QStringLiteral("无效的技能目录名");
        return {};
    }
    ClaudeSkillBundle items;
    if (!inventory(skillPath(base, info.directory, info.enabled), &items, false, error)) return {};
    QStringList result;
    for (const auto &f : items) result.append(f.path + (f.directory ? QStringLiteral("/") : QString()));
    result.sort();
    return result;
}

ClaudeSkillBundle ClaudeSkillStore::localBundle(const QString &path, QString *error)
{
    ClaudeSkillBundle bundle;
    const QFileInfo f(path);
    if (f.isSymLink()) { *error = QStringLiteral("不能导入符号链接"); return {}; }
    if (f.isFile()) {
        bundle.append({QStringLiteral("SKILL.md"), loadFile(path, skills::maxMarkdownBytes, error), false, false});
    } else if (f.isDir()) {
        ClaudeSkillStore local(m_home);
        local.inventory(f.absoluteFilePath(), &bundle, true, error);
    } else { *error = QStringLiteral("导入源必须是 SKILL.md 或完整技能目录"); }
    if (error->isEmpty()) *error = skills::validateBundle(bundle);
    return error->isEmpty() ? bundle : ClaudeSkillBundle{};
}
bool ClaudeSkillStore::writeLocalBundle(const QString &destination, const ClaudeSkillBundle &bundle, QString *error)
{
    if (!safeAbsolute(destination, false)) { *error = QStringLiteral("导出目标必须是本地绝对路径"); return false; }
    if (exists(destination)) { *error = QStringLiteral("目标已存在，未覆盖：%1").arg(destination); return false; }
    const auto parent = QFileInfo(destination).absolutePath();
    if (!QFileInfo(parent).isDir()) { *error = QStringLiteral("目标父目录不存在"); return false; }
    QTemporaryDir staging(parent + QStringLiteral("/.cube-skill-XXXXXX"));
    if (!staging.isValid()) { *error = staging.errorString(); return false; }
    for (const auto &file : bundle) {
        if (stopped(error)) return false;
        const auto path = staging.path() + QLatin1Char('/') + file.path;
        if (!QDir().mkpath(file.directory ? path : QFileInfo(path).absolutePath())) { *error = QStringLiteral("无法创建资源目录"); return false; }
        if (file.directory) continue;
        if (!writeFile(path, file.bytes, error)) return false;
        if (file.executable && !QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther)) {
            *error = QStringLiteral("无法保留脚本执行权限"); return false;
        }
    }
    if (stopped(error)) return false;
    if (exists(destination) || !QDir().rename(staging.path(), destination)) { *error = QStringLiteral("目标已存在或发布技能目录失败"); return false; }
    staging.setAutoRemove(false);
    return true;
}
bool ClaudeSkillStore::create(const ClaudeSkillScope &scope, const QString &name, const ClaudeSkillBundle &bundle, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    if (!skills::validNewName(name)) { *error = QStringLiteral("目录名须为 1–64 位小写字母、数字、连字符，且不能使用保留名称"); return false; }
    *error = skills::validateBundle(bundle);
    if (!error->isEmpty()) return false;
    const auto base = root(scope, error);
    if (!error->isEmpty()) return false;
    const auto destination = skillPath(base, name, true);
    if (!m_remote) {
        for (const auto &p : {base, base + QStringLiteral("/skills"), base + QStringLiteral("/skills-disabled")}) {
            if (QFileInfo(p).isSymLink()) { *error = QStringLiteral("符号链接管理目录只读"); return false; }
        }
        if (exists(skillPath(base, name, false)) || exists(destination)) { *error = QStringLiteral("同名技能已存在（含停用目录）"); return false; }
        if (!QDir().mkpath(base + QStringLiteral("/skills"))) { *error = QStringLiteral("无法创建技能目录"); return false; }
        return writeLocalBundle(destination, bundle, error);
    }
    QString script = QStringLiteral("base=%1; dest=%2; other=%3; "
        "[ ! -L \"$base\" ] && [ ! -L \"$base/skills\" ] && [ ! -L \"$base/skills-disabled\" ] || fail '符号链接管理目录只读'; "
        "[ ! -e \"$dest\" ] && [ ! -L \"$dest\" ] && [ ! -e \"$other\" ] && [ ! -L \"$other\" ] || fail '同名技能已存在'; "
        "mkdir -p \"$base/skills\"; tmp=$(mktemp -d \"$base/skills/.cube-skill-XXXXXX\"); "
        "trap 'rm -rf \"$tmp\"' EXIT; trap 'exit 1' HUP INT TERM; ")
        .arg(quote(base), quote(destination), quote(skillPath(base, name, false)));
    QByteArray input;
    for (const auto &file : bundle) {
        script += QStringLiteral("p=\"$tmp\"/%1; ").arg(quote(file.path));
        if (file.directory) script += QStringLiteral("mkdir -p \"$p\"; ");
        else {
            script += QStringLiteral("mkdir -p \"${p%/*}\"; IFS= read -r data; printf '%s' \"$data\" | decode > \"$p\"; ");
            if (file.executable) script += QStringLiteral("chmod 755 \"$p\"; ");
            input += file.bytes.toBase64() + '\n';
        }
    }
    script += QStringLiteral("[ ! -e \"$dest\" ] && [ ! -L \"$dest\" ] || fail '目标已存在'; "
        "mv -n \"$tmp\" \"$dest\"; [ ! -d \"$tmp\" ] || fail '发布失败：目标已存在'; trap - EXIT");
    command(script, input, error);
    return error->isEmpty();
}
bool ClaudeSkillStore::save(const ClaudeSkillScope &scope, const ClaudeSkillInfo &original, const QByteArray &content, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    *error = skills::validateDocument(content);
    if (!error->isEmpty()) return false;
    const auto base = root(scope, error);
    if (!error->isEmpty() || !writable(base, original, error)) return false;
    const auto path = skillPath(base, original.directory, original.enabled) + QStringLiteral("/SKILL.md");
    if (!m_remote) {
        const auto current = loadFile(path, skills::maxMarkdownBytes, error);
        if (!error->isEmpty()) return false;
        if (current != original.content) { *error = QStringLiteral("SKILL.md 已被外部修改，请重新读取后再保存"); return false; }
        return !stopped(error) && writeFile(path, content, error);
    }
    const auto script = remoteGuard(base, original) + QStringLiteral(
        "[ -f \"$d/SKILL.md\" ] || fail 'SKILL.md 已不存在'; "
        "tmp=$(mktemp -d \"$d/.cube-edit-XXXXXX\"); trap 'rm -rf \"$tmp\"' EXIT; trap 'exit 1' HUP INT TERM; "
        "IFS= read -r old; IFS= read -r new; printf '%s' \"$old\" | decode > \"$tmp/old\"; "
        "printf '%s' \"$new\" | decode > \"$tmp/new\"; "
        "cmp -s \"$d/SKILL.md\" \"$tmp/old\" || fail 'SKILL.md 已被外部修改，请重新读取'; "
        "cp -p \"$d/SKILL.md\" \"$tmp/ready\"; cat \"$tmp/new\" > \"$tmp/ready\"; "
        "mv -f \"$tmp/ready\" \"$d/SKILL.md\"");
    command(script, original.content.toBase64() + '\n' + content.toBase64() + '\n', error);
    return error->isEmpty();
}
bool ClaudeSkillStore::setEnabled(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, bool enabled, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    if (!error->isEmpty() || !writable(base, info, error)) return false;
    if (enabled == info.enabled) return true;
    const auto dest = skillPath(base, info.directory, enabled);
    if (!m_remote) {
        if (QFileInfo(base + QLatin1Char('/') + slot(enabled)).isSymLink()) { *error = QStringLiteral("目标管理目录是符号链接"); return false; }
        if (exists(dest)) { *error = QStringLiteral("目标已有同名技能，未覆盖"); return false; }
        if (!QDir().mkpath(QFileInfo(dest).absolutePath()) || !QDir().rename(skillPath(base, info.directory, info.enabled), dest)) {
            *error = QStringLiteral("无法移动技能目录"); return false;
        }
        return true;
    }
    const auto script = remoteGuard(base, info) + QStringLiteral("dest=%1; "
        "[ ! -L \"${dest%/*}\" ] || fail '目标管理目录是符号链接'; "
        "[ ! -e \"$dest\" ] && [ ! -L \"$dest\" ] || fail '目标已有同名技能'; "
        "mkdir -p \"${dest%/*}\"; mv -n \"$d\" \"$dest\"; [ ! -d \"$d\" ] || fail '移动失败：目标已存在'").arg(quote(dest));
    command(script, {}, error);
    return error->isEmpty();
}
bool ClaudeSkillStore::remove(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    if (!error->isEmpty() || !writable(base, info, error)) return false;
    if (m_remote) command(remoteGuard(base, info) + QStringLiteral("rm -rf \"$d\""), {}, error);
    else if (!QDir(skillPath(base, info.directory, info.enabled)).removeRecursively()) *error = QStringLiteral("删除技能目录失败");
    return error->isEmpty();
}
bool ClaudeSkillStore::importSkill(const ClaudeSkillScope &scope, const QString &name, const QString &source, QString *error)
{
    error->clear();
    if (stopped(error)) return false;
    const auto bundle = localBundle(source, error);
    return error->isEmpty() && create(scope, name, bundle, error);
}
bool ClaudeSkillStore::exportSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, const QString &destination, QString *error)
{
    QMutexLocker lock(&m_mutex);
    error->clear();
    const auto base = root(scope, error);
    if (!error->isEmpty() || !writable(base, info, error)) return false;
    const auto source = skillPath(base, info.directory, info.enabled);
    if (!m_remote) {
        const auto sourcePath = QFileInfo(source).canonicalFilePath();
        const auto parent = QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath();
        if (!sourcePath.isEmpty() && (parent == sourcePath || parent.startsWith(sourcePath + QLatin1Char('/')))) {
            *error = QStringLiteral("不能导出到原始技能目录内部"); return false;
        }
    } else {
        command(remoteGuard(base, info), {}, error);
        if (!error->isEmpty()) return false;
    }
    ClaudeSkillBundle bundle;
    if (!inventory(source, &bundle, true, error)) return false;
    *error = skills::validateBundle(bundle);
    if (!error->isEmpty()) return false;
    return writeLocalBundle(destination, bundle, error);
}
} // namespace cubeshell
