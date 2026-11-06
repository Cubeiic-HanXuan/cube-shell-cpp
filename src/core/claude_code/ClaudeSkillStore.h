#pragma once

// ClaudeSkillStore.h — 不依赖 Claude CLI 的本地 / SSH Skills 存储。
// 每个实例固定来源；调用方在工作线程运行。远程命令注入用于临时目录协议测试。
#include "claude_code/ClaudeSkillData.h"
#include <QMutex>
#include <atomic>
#include <functional>
#include <memory>

namespace cubeshell {
class SshClient;

class ClaudeSkillStore {
public:
    using RemoteCommand = std::function<QByteArray(const QString &, const QByteArray &, QString *)>;
    explicit ClaudeSkillStore(const QString &claudeHome);
    explicit ClaudeSkillStore(std::shared_ptr<SshClient> client);
    explicit ClaudeSkillStore(RemoteCommand command);
    bool isRemote() const { return m_remote; }
    void cancel() { m_cancelled.store(true); }

    QList<ClaudeSkillInfo> list(const ClaudeSkillScope &scope, QString *rootOut, QString *error);
    ClaudeSkillInfo read(const ClaudeSkillScope &scope, const QString &directory, bool enabled, QString *error);
    QStringList files(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, QString *error);
    bool create(const ClaudeSkillScope &scope, const QString &name, const ClaudeSkillBundle &bundle, QString *error);
    bool save(const ClaudeSkillScope &scope, const ClaudeSkillInfo &original, const QByteArray &content, QString *error);
    bool setEnabled(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, bool enabled, QString *error);
    bool remove(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, QString *error);
    bool importSkill(const ClaudeSkillScope &scope, const QString &name, const QString &source, QString *error);
    bool exportSkill(const ClaudeSkillScope &scope, const ClaudeSkillInfo &info, const QString &destination, QString *error);

private:
    QString root(const ClaudeSkillScope &scope, QString *error);
    QString skillPath(const QString &root, const QString &directory, bool enabled) const;
    ClaudeSkillInfo readAt(const QString &root, const QString &directory, bool enabled, QString *error);
    bool writable(const QString &root, const ClaudeSkillInfo &info, QString *error);
    QByteArray command(const QString &script, const QByteArray &input, QString *error);
    bool stopped(QString *error) const;
    QString remoteGuard(const QString &root, const ClaudeSkillInfo &info) const;
    ClaudeSkillBundle localBundle(const QString &path, QString *error);
    bool writeLocalBundle(const QString &destination, const ClaudeSkillBundle &bundle, QString *error);
    bool inventory(const QString &path, ClaudeSkillBundle *bundle, bool bytes, QString *error);

    QString m_home;
    bool m_remote = false;
    std::shared_ptr<SshClient> m_client;
    RemoteCommand m_command;
    std::atomic<bool> m_cancelled{false};
    QMutex m_mutex;
};
} // namespace cubeshell
