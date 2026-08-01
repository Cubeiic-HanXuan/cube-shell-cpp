#pragma once

// HermesGateway.h — Hermes gateway lifecycle + platform config + skills.
// 对应Python: core/hermes/gateway_widget.py 的 GatewayWorker
//             + core/hermes/skills_widget.py 的 SkillsWorker
//
// - platform catalog     对应Python: gateway_widget.PLATFORMS(8 平台)
// - .env config load/save对应Python: _do_load_config/_do_save_config
// - running detection    对应Python: _detect_running(三层判据)
// - start/stop polling   对应Python: _do_start/_do_stop(10x1s 轮询)
// - platform test        对应Python: _do_test(hermes send --to ...)
// - skills scan/install  对应Python: SkillsWorker(_load_skills/_install/_delete)
//
// All operations run on the global thread pool (QtConcurrent); signals are
// emitted from worker threads — consumers MUST connect with
// Qt::QueuedConnection.

#include <QFuture>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace cubeshell {

class HermesBackend;

// 对应Python: gateway_widget.PLATFORMS 的一项
struct GatewayPlatform {
    QString id;
    QString name;
    QStringList fields; // .env 变量名 = {ID_UPPER}_{FIELD_UPPER}
};

// Per-platform config parsed from .env + gateway_state.json.
struct GatewayPlatformConfig {
    QString id;
    QMap<QString, QString> values; // field -> value
    bool connected = false;        // gateway_state.json platforms.<id>.state
};

// 对应Python: skills_widget 的 skill dict
struct HermesSkillInfo {
    QString name;
    QString description;
    QString version;
    QString author;
    QStringList tags;
    QString category;
    QString dirName;
    QString content; // SKILL.md 原文
};

class HermesGateway : public QObject {
    Q_OBJECT
public:
    explicit HermesGateway(HermesBackend *backend, QObject *parent = nullptr);
    ~HermesGateway() override;

    // 对应Python: gateway_widget.PLATFORMS
    static QList<GatewayPlatform> platforms();
    // Token/Secret 类字段判定(UI 密码回显用)
    // 对应Python: gateway_widget._SECRET_KEYWORDS
    static bool isSecretField(const QString &fieldName);

    // --- gateway ---

    // 对应Python: GatewayWorker._do_load_config
    void loadConfig();
    // 对应Python: GatewayWorker._do_check_status
    void checkStatus();
    // 对应Python: GatewayWorker._do_start / _do_stop(带 10 次轮询)
    void startGateway();
    void stopGateway();
    // 对应Python: GatewayWorker._do_test
    void testPlatform(const QString &platformId);
    // 对应Python: GatewayWorker._do_save_config(.env 保留注释/取消注释)
    void savePlatformConfig(const QString &platformId,
                            const QMap<QString, QString> &fields);

    // --- skills ---

    // 对应Python: SkillsWorker._load_skills(递归扫描 SKILL.md)
    void loadSkills();
    // 对应Python: SkillsWorker._install_skill / _delete_skill
    void installSkill(const QString &name);
    void removeSkill(const QString &name);

    // --- pure helpers (unit-testable) ---

    // 对应Python: GatewayWorker._do_save_config 的 .env 重写逻辑
    static QString rewriteEnvContent(const QString &envContent,
                                     const QString &platformId,
                                     const QMap<QString, QString> &fields);
    // 对应Python: skills_widget._parse_frontmatter(不依赖 YAML 库)
    static QMap<QString, QString> parseFrontmatter(const QString &content,
                                                   QStringList *tagsOut = nullptr);

signals:
    // Emitted from worker threads — connect with Qt::QueuedConnection.
    void configLoaded(const QList<cubeshell::GatewayPlatformConfig> &configs);
    void statusChecked(bool isRunning);
    void skillsLoaded(const QList<cubeshell::HermesSkillInfo> &skills);
    void commandDone(const QString &description, const QString &output);
    void errorOccurred(const QString &message);

private:
    void schedule(std::function<void()> job);
    bool detectRunning();           // 对应Python: _detect_running
    QString activeProfileGateway(); // 对应Python: _active_profile_gateway
    void findSkillsRecursive(const QString &baseDir, const QString &currentDir,
                             QList<HermesSkillInfo> &skills, int maxDepth,
                             int depth);

    HermesBackend *m_backend = nullptr; // not owned
    QVector<QFuture<void>> m_futures;   // joined in destructor
};

} // namespace cubeshell
