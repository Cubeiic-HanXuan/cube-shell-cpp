#pragma once

// ClaudeSkillData.h — 自定义 Skills 的身份、原文与可移植目录包。
// 原文始终保留，YAML 只用于提取摘要和校验，不参与重新序列化。
#include <QByteArray>
#include <QFileDevice>
#include <QList>
#include <QMetaType>
#include <QStringList>

namespace cubeshell {

struct ClaudeSkillScope {
    QString scope = QStringLiteral("user");
    QString projectPath;
    bool operator==(const ClaudeSkillScope &other) const
    { return scope == other.scope && projectPath == other.projectPath; }
};

struct ClaudeSkillInfo {
    QString directory;
    QString name;
    QString description;
    QString path;
    QString error;
    QByteArray content;
    bool enabled = true;
    bool readOnly = false;
    bool manualOnly = false;
    bool userInvocable = true;
    QString key() const { return (enabled ? QStringLiteral("on/") : QStringLiteral("off/")) + directory; }
};

struct ClaudeSkillFile {
    QString path; // 包内相对路径，不允许链接、特殊文件或向上越界
    QByteArray bytes;
    bool directory = false;
    bool executable = false;
};
using ClaudeSkillBundle = QList<ClaudeSkillFile>;

namespace skills {
constexpr qint64 maxMarkdownBytes = 1024 * 1024;
constexpr qint64 maxFileBytes = 16 * 1024 * 1024;
constexpr qint64 maxBundleBytes = 64 * 1024 * 1024;
constexpr int maxFiles = 2048;
constexpr int maxSkills = 2048;

ClaudeSkillInfo parse(const QString &directory, const QByteArray &content);
bool validComponent(const QString &name);
bool validRelativePath(const QString &path);
bool validNewName(const QString &name);
QString validateDocument(const QByteArray &content);
QString validateBundle(const ClaudeSkillBundle &bundle);
QByteArray documentTemplate(const QString &name);
QString invocationLabel(const ClaudeSkillInfo &info);
}
} // namespace cubeshell
Q_DECLARE_METATYPE(cubeshell::ClaudeSkillScope)
Q_DECLARE_METATYPE(cubeshell::ClaudeSkillInfo)
Q_DECLARE_METATYPE(QList<cubeshell::ClaudeSkillInfo>)
