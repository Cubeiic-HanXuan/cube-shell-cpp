#include "claude_code/ClaudeSkillData.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <yaml-cpp/yaml.h>

namespace cubeshell::skills {

bool validComponent(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c.unicode() < 32 || c.unicode() == 127)
            return false;
    }
    return true;
}
bool validRelativePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 4096 || path.contains(QLatin1Char(':')))
        return false;
    const auto parts = path.split(QLatin1Char('/'));
    if (parts.size() > 64)
        return false;
    for (const auto &part : parts)
        if (!validComponent(part)) return false;
    return true;
}
bool validNewName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    static const QRegularExpression windowsDevice(QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])$"));
    return pattern.match(name).hasMatch() && !windowsDevice.match(name).hasMatch()
        && name != QLatin1String("synced") && name != QLatin1String("anthropic-skills");
}

ClaudeSkillInfo parse(const QString &directory, const QByteArray &content)
{
    ClaudeSkillInfo info;
    info.directory = directory;
    info.name = directory;
    info.content = content;
    if (content.size() > maxMarkdownBytes) {
        info.error = QStringLiteral("SKILL.md 超过 1 MiB，无法编辑");
        return info;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(content);
    if (decoder.hasError() || text.contains(QChar::Null)) {
        info.error = QStringLiteral("SKILL.md 必须是有效的 UTF-8 文本");
        return info;
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const auto lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first() != QLatin1String("---"))
        return info;
    int end = 1;
    while (end < lines.size() && lines[end] != QLatin1String("---")) ++end;
    if (end == lines.size()) {
        info.error = QStringLiteral("YAML frontmatter 缺少结束分隔行 ---");
        return info;
    }
    try {
        const auto header = lines.mid(1, end - 1).join(QLatin1Char('\n')).toUtf8();
        const auto node = YAML::Load(header.toStdString());
        if (!node || node.IsNull()) return info;
        if (!node.IsMap()) {
            info.error = QStringLiteral("YAML frontmatter 必须是键值映射");
            return info;
        }
        QSet<QString> seen;
        for (const auto &entry : node) {
            const auto key = QString::fromStdString(entry.first.as<std::string>());
            if (seen.contains(key)) {
                info.error = QStringLiteral("重复的 YAML 字段：%1").arg(key);
                return info;
            }
            seen.insert(key);
        }
        if (node["name"]) {
            const auto name = QString::fromStdString(node["name"].as<std::string>()).trimmed();
            if (!name.isEmpty()) info.name = name;
        }
        if (node["description"])
            info.description = QString::fromStdString(node["description"].as<std::string>());
        if (node["disable-model-invocation"])
            info.manualOnly = node["disable-model-invocation"].as<bool>();
        if (node["user-invocable"])
            info.userInvocable = node["user-invocable"].as<bool>();
    } catch (const YAML::Exception &e) {
        info.error = QStringLiteral("YAML 格式错误：%1").arg(QString::fromUtf8(e.what()));
    }
    return info;
}
QString validateDocument(const QByteArray &content)
{
    if (content.trimmed().isEmpty()) return QStringLiteral("SKILL.md 内容不能为空");
    return parse(QStringLiteral("skill"), content).error;
}
QString validateBundle(const ClaudeSkillBundle &bundle)
{
    if (bundle.isEmpty() || bundle.size() > maxFiles)
        return QStringLiteral("技能目录不能为空，最多支持 2048 个文件/目录");
    QSet<QString> paths;
    QSet<QString> files;
    qint64 total = 0;
    bool hasDocument = false;
    static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\\..*)?$"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression invalidCharacters(QStringLiteral("[<>\"|?*]"));
    for (const auto &file : bundle) {
        const auto key = file.path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (!validRelativePath(file.path) || paths.contains(key))
            return QStringLiteral("无效或重复的资源路径：%1").arg(file.path);
        // 避免导出到 Windows/macOS 时发生路径归一化覆盖。
        for (const auto &part : file.path.split(QLatin1Char('/'))) {
            if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))
                || reserved.match(part).hasMatch() || invalidCharacters.match(part).hasMatch())
                return QStringLiteral("资源路径包含不可移植的文件名：%1").arg(file.path);
        }
        paths.insert(key);
        if (!file.directory) files.insert(key);
        if (file.bytes.size() > maxFileBytes || (file.directory && !file.bytes.isEmpty()))
            return QStringLiteral("资源文件超过 16 MiB 或目录数据无效：%1").arg(file.path);
        total += file.bytes.size();
        if (total > maxBundleBytes) return QStringLiteral("技能目录超过 64 MiB");
        if (file.path == QLatin1String("SKILL.md") && !file.directory) {
            hasDocument = true;
            const auto error = validateDocument(file.bytes);
            if (!error.isEmpty()) return error;
        }
    }
    for (const auto &path : paths) {
        auto parent = path;
        while (parent.contains(QLatin1Char('/'))) {
            parent = parent.left(parent.lastIndexOf(QLatin1Char('/')));
            if (files.contains(parent)) return QStringLiteral("资源文件和目录冲突：%1").arg(path);
        }
    }
    return hasDocument ? QString() : QStringLiteral("技能目录根部缺少 SKILL.md");
}
QByteArray documentTemplate(const QString &name)
{
    return QStringLiteral("---\nname: %1\ndescription: 描述这个技能的用途和适用场景。\n---\n\n# %1\n\n在这里编写技能指令。可以引用 references/、scripts/ 等目录中的辅助文件。\n")
        .arg(name).toUtf8();
}
QString invocationLabel(const ClaudeSkillInfo &info)
{
    if (info.manualOnly && !info.userInvocable) return QStringLiteral("禁止手动/自动调用");
    if (info.manualOnly) return QStringLiteral("仅手动调用");
    if (!info.userInvocable) return QStringLiteral("仅模型调用");
    return QStringLiteral("手动 / 模型");
}
} // namespace cubeshell::skills
