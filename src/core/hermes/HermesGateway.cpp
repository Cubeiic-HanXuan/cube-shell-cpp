// HermesGateway.cpp — see HermesGateway.h for the port map.
// 对应Python: core/hermes/gateway_widget.py + skills_widget.py

#include "hermes/HermesGateway.h"

#include "hermes/HermesBackend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>
#include <QtConcurrent>

namespace cubeshell {

// ---------------------------------------------------------------------------
// platform catalog
// ---------------------------------------------------------------------------

// 对应Python: gateway_widget.PLATFORMS
QList<GatewayPlatform> HermesGateway::platforms()
{
    static const QList<GatewayPlatform> kPlatforms = {
        {QStringLiteral("telegram"), QStringLiteral("Telegram"),
         {QStringLiteral("bot_token"), QStringLiteral("home_channel"),
          QStringLiteral("allowed_users")}},
        {QStringLiteral("discord"), QStringLiteral("Discord"),
         {QStringLiteral("bot_token"), QStringLiteral("home_channel"),
          QStringLiteral("allowed_users")}},
        {QStringLiteral("slack"), QStringLiteral("Slack"),
         {QStringLiteral("bot_token"), QStringLiteral("app_token"),
          QStringLiteral("home_channel")}},
        {QStringLiteral("feishu"), QStringLiteral("飞书"),
         {QStringLiteral("app_id"), QStringLiteral("app_secret"),
          QStringLiteral("domain"), QStringLiteral("connection_mode"),
          QStringLiteral("group_policy")}},
        {QStringLiteral("wecom"), QStringLiteral("企业微信"),
         {QStringLiteral("corp_id"), QStringLiteral("agent_id"),
          QStringLiteral("secret")}},
        {QStringLiteral("dingtalk"), QStringLiteral("钉钉"),
         {QStringLiteral("app_key"), QStringLiteral("app_secret"),
          QStringLiteral("robot_code")}},
        {QStringLiteral("matrix"), QStringLiteral("Matrix"),
         {QStringLiteral("homeserver"), QStringLiteral("user_id"),
          QStringLiteral("access_token")}},
        {QStringLiteral("whatsapp"), QStringLiteral("WhatsApp"),
         {QStringLiteral("phone_number_id"), QStringLiteral("access_token"),
          QStringLiteral("verify_token")}},
    };
    return kPlatforms;
}

// 对应Python: gateway_widget._SECRET_KEYWORDS
bool HermesGateway::isSecretField(const QString &fieldName)
{
    const QString low = fieldName.toLower();
    return low.contains(QLatin1String("token"))
        || low.contains(QLatin1String("secret"))
        || low.contains(QLatin1String("key"))
        || low.contains(QLatin1String("password"));
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

HermesGateway::HermesGateway(HermesBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
}

HermesGateway::~HermesGateway()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

void HermesGateway::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

// ---------------------------------------------------------------------------
// gateway config
// ---------------------------------------------------------------------------

// 对应Python: GatewayWorker._do_load_config
void HermesGateway::loadConfig()
{
    schedule([this]() {
        const QString home = m_backend->hermesHome();

        // 读取 .env 文件解析平台配置
        const QString envContent =
            m_backend->readFile(home + QStringLiteral("/.env"));
        QMap<QString, QString> envVars;
        const QStringList envLines = envContent.split(QLatin1Char('\n'));
        for (const QString &raw : envLines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq > 0)
                envVars.insert(line.left(eq).trimmed(),
                               line.mid(eq + 1).trimmed());
        }

        // 读取 gateway_state.json 获取平台连接状态
        QJsonObject platformStates;
        const QString stateContent =
            m_backend->readFile(home + QStringLiteral("/gateway_state.json"));
        if (!stateContent.isEmpty()) {
            const QJsonDocument doc =
                QJsonDocument::fromJson(stateContent.toUtf8());
            if (doc.isObject())
                platformStates =
                    doc.object().value(QStringLiteral("platforms")).toObject();
        }

        // 将 .env 变量按平台分组
        QList<GatewayPlatformConfig> configs;
        const QList<GatewayPlatform> all = platforms();
        for (const GatewayPlatform &p : all) {
            const QString prefix = p.id.toUpper() + QLatin1Char('_');
            GatewayPlatformConfig cfg;
            cfg.id = p.id;
            for (const QString &field : p.fields) {
                const QString envKey = prefix + field.toUpper();
                if (envVars.contains(envKey))
                    cfg.values.insert(field, envVars.value(envKey));
            }
            if (cfg.values.isEmpty())
                continue; // 有配置值才认为已配置
            const QJsonObject pstate =
                platformStates.value(p.id).toObject();
            cfg.connected = pstate.value(QStringLiteral("state")).toString()
                            == QLatin1String("connected");
            configs.append(cfg);
        }
        emit configLoaded(configs);
    });
}

// 对应Python: GatewayWorker._active_profile_gateway
QString HermesGateway::activeProfileGateway()
{
    const QString output =
        m_backend->execCli({QStringLiteral("profile"), QStringLiteral("list")});
    if (output.isEmpty())
        return QString();
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString raw = line.trimmed();
        if (raw.isEmpty() || raw.contains(QChar(0x2500)) // '─' 分隔线
            || raw.startsWith(QLatin1String("Profile")))
            continue;
        if (!raw.startsWith(QChar(0x25C6))) // '◆' 活跃标记
            continue;
        const QString body = raw.mid(1);
        const QStringList parts =
            body.split(QRegularExpression(QStringLiteral("\\s+")),
                       Qt::SkipEmptyParts);
        // 形如: <name> <model> <gateway> ...
        if (parts.size() >= 3)
            return parts.at(2).trimmed().toLower();
    }
    return QString();
}

// 对应Python: GatewayWorker._detect_running(三层判据)
bool HermesGateway::detectRunning()
{
    // 1) 主判据:profile list 活跃行的 Gateway 列
    const QString gw = activeProfileGateway();
    if (!gw.isEmpty()) {
        emit commandDone(QStringLiteral("检查网关状态"),
                         QStringLiteral("profile list: gateway=%1").arg(gw));
        return gw == QLatin1String("running");
    }

    const QString home = m_backend->hermesHome();

    // 2) 回退:gateway_state.json(守护进程写入)
    const QString stateContent =
        m_backend->readFile(home + QStringLiteral("/gateway_state.json"));
    if (!stateContent.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(stateContent.toUtf8());
        if (doc.isObject()) {
            const QString state = doc.object()
                                      .value(QStringLiteral("gateway_state"))
                                      .toString().trimmed().toLower();
            if (!state.isEmpty()) {
                emit commandDone(QStringLiteral("检查网关状态"),
                                 QStringLiteral("gateway_state.json: %1").arg(state));
                return state == QLatin1String("running");
            }
        }
    }

    // 3) 回退:解析 CLI 文本输出(放宽匹配,兼容多种格式)
    const QString output = m_backend->execCli(
        {QStringLiteral("gateway"), QStringLiteral("status")});
    emit commandDone(QStringLiteral("检查网关状态"),
                     output.isEmpty() ? QStringLiteral("(无输出)") : output);
    if (output.isEmpty())
        return false;
    const QString lower = output.toLower();
    // 明确的停止标记优先,避免 "not running" 里含 "running" 被误判
    if (lower.contains(QLatin1String("not loaded"))
        || lower.contains(QLatin1String("not running"))
        || lower.contains(QLatin1String("inactive")))
        return false;
    if (lower.contains(QLatin1String("is loaded"))
        || lower.contains(QLatin1String("is running"))
        || lower.contains(QLatin1String("active (running)")))
        return true;
    // 兜底:出现 PID 数字通常意味着进程存在
    return lower.contains(QLatin1String("pid"));
}

// 对应Python: GatewayWorker._do_check_status
void HermesGateway::checkStatus()
{
    schedule([this]() {
        emit statusChecked(detectRunning());
    });
}

// 对应Python: GatewayWorker._do_start(启动异步,轮询 10 次)
void HermesGateway::startGateway()
{
    schedule([this]() {
        const QString output = m_backend->execCli(
            {QStringLiteral("gateway"), QStringLiteral("start")});
        emit commandDone(QStringLiteral("启动网关"),
                         output.isEmpty() ? QStringLiteral("(无输出)") : output);
        bool isRunning = false;
        for (int i = 0; i < 10; ++i) { // 最多约 10 秒
            QThread::msleep(1000);
            if (detectRunning()) {
                isRunning = true;
                break;
            }
        }
        emit statusChecked(isRunning);
    });
}

// 对应Python: GatewayWorker._do_stop(停止同样异步轮询)
void HermesGateway::stopGateway()
{
    schedule([this]() {
        const QString output = m_backend->execCli(
            {QStringLiteral("gateway"), QStringLiteral("stop")});
        emit commandDone(QStringLiteral("停止网关"),
                         output.isEmpty() ? QStringLiteral("(无输出)") : output);
        bool isRunning = true;
        for (int i = 0; i < 10; ++i) {
            QThread::msleep(1000);
            if (!detectRunning()) {
                isRunning = false;
                break;
            }
        }
        emit statusChecked(isRunning);
    });
}

// 对应Python: GatewayWorker._do_test
void HermesGateway::testPlatform(const QString &platformId)
{
    schedule([this, platformId]() {
        const QString output = m_backend->execCli(
            {QStringLiteral("send"), QStringLiteral("--to"), platformId,
             QStringLiteral("Test from CubeShell")});
        emit commandDone(QStringLiteral("测试 %1").arg(platformId),
                         output.isEmpty() ? QStringLiteral("(无输出)") : output);
    });
}

// 对应Python: GatewayWorker._do_save_config 的 .env 重写(纯函数部分)
QString HermesGateway::rewriteEnvContent(const QString &envContent,
                                         const QString &platformId,
                                         const QMap<QString, QString> &fields)
{
    const QString prefix = platformId.toUpper() + QLatin1Char('_');
    const QStringList lines = envContent.split(QLatin1Char('\n'));
    QStringList newLines;
    QSet<QString> updatedKeys;

    for (const QString &line : lines) {
        const QString stripped = line.trimmed();
        if (stripped.startsWith(QLatin1Char('#'))) {
            // 检查是否是被注释的同名变量,如果要设置它就取消注释
            QString commentBody = stripped;
            while (commentBody.startsWith(QLatin1Char('#'))
                   || commentBody.startsWith(QLatin1Char(' ')))
                commentBody.remove(0, 1);
            const int eq = commentBody.indexOf(QLatin1Char('='));
            if (eq > 0) {
                const QString ck = commentBody.left(eq).trimmed();
                if (ck.startsWith(prefix)) {
                    const QString fieldName = ck.mid(prefix.size()).toLower();
                    if (fields.contains(fieldName)
                        && !fields.value(fieldName).isEmpty()) {
                        newLines.append(ck + QLatin1Char('=')
                                        + fields.value(fieldName));
                        updatedKeys.insert(fieldName);
                        continue;
                    }
                }
            }
            newLines.append(line);
            continue;
        }
        const int eq = stripped.indexOf(QLatin1Char('='));
        if (eq > 0) {
            const QString key = stripped.left(eq).trimmed();
            if (key.startsWith(prefix)) {
                const QString fieldName = key.mid(prefix.size()).toLower();
                if (fields.contains(fieldName)) {
                    newLines.append(key + QLatin1Char('=')
                                    + fields.value(fieldName));
                    updatedKeys.insert(fieldName);
                    continue;
                }
            }
        }
        newLines.append(line);
    }

    // 追加未更新的新字段
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (!updatedKeys.contains(it.key()) && !it.value().isEmpty())
            newLines.append(prefix + it.key().toUpper() + QLatin1Char('=')
                            + it.value());
    }
    // 去掉 split 产生的末尾空行再统一补一个换行
    while (!newLines.isEmpty() && newLines.last().isEmpty())
        newLines.removeLast();
    return newLines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// 对应Python: GatewayWorker._do_save_config
void HermesGateway::savePlatformConfig(const QString &platformId,
                                       const QMap<QString, QString> &fields)
{
    schedule([this, platformId, fields]() {
        const QString envPath =
            m_backend->hermesHome() + QStringLiteral("/.env");
        const QString envContent = m_backend->readFile(envPath);
        const QString newContent =
            rewriteEnvContent(envContent, platformId, fields);
        if (!m_backend->writeFile(envPath, newContent)) {
            emit errorOccurred(QStringLiteral("写入 .env 失败: %1").arg(envPath));
            return;
        }
        emit commandDone(QStringLiteral("保存 %1 配置").arg(platformId),
                         QStringLiteral("配置已保存到 .env"));
    });
}

// ---------------------------------------------------------------------------
// skills
// ---------------------------------------------------------------------------

// 对应Python: skills_widget._parse_frontmatter
QMap<QString, QString> HermesGateway::parseFrontmatter(const QString &content,
                                                       QStringList *tagsOut)
{
    QMap<QString, QString> meta;
    if (!content.trimmed().startsWith(QLatin1String("---")))
        return meta;
    // 找到前后两个 ---
    static const QRegularExpression fmRe(
        QStringLiteral("^---\\s*\\n(.*?)\\n---"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = fmRe.match(content);
    if (!m.hasMatch())
        return meta;

    static const QRegularExpression kvRe(
        QStringLiteral("^(\\w+)\\s*:\\s*(.+)$"));
    const QStringList lines = m.captured(1).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QRegularExpressionMatch kv = kvRe.match(line);
        if (!kv.hasMatch())
            continue;
        const QString key = kv.captured(1);
        QString value = kv.captured(2).trimmed();
        // 解析数组格式 [tag1, tag2, ...]
        if (value.startsWith(QLatin1Char('[')) && value.endsWith(QLatin1Char(']'))) {
            if (tagsOut && key == QLatin1String("tags")) {
                const QStringList items =
                    value.mid(1, value.size() - 2).split(QLatin1Char(','));
                for (const QString &item : items) {
                    QString t = item.trimmed();
                    t.remove(QLatin1Char('"'));
                    t.remove(QLatin1Char('\''));
                    if (!t.isEmpty())
                        tagsOut->append(t);
                }
            }
            continue;
        }
        // 去除引号
        value.remove(QLatin1Char('"'));
        value.remove(QLatin1Char('\''));
        meta.insert(key, value);
    }
    return meta;
}

// 对应Python: SkillsWorker._find_skills_recursive
void HermesGateway::findSkillsRecursive(const QString &baseDir,
                                        const QString &currentDir,
                                        QList<HermesSkillInfo> &skills,
                                        int maxDepth, int depth)
{
    if (depth > maxDepth)
        return;
    const QStringList entries = m_backend->listDir(currentDir);
    if (entries.isEmpty())
        return;

    if (depth > 0) { // 跳过根 skills/ 目录本身
        QString content;
        if (entries.contains(QStringLiteral("SKILL.md")))
            content = m_backend->readFile(currentDir + QStringLiteral("/SKILL.md"));
        if (content.isEmpty() && entries.contains(QStringLiteral("skill.md")))
            content = m_backend->readFile(currentDir + QStringLiteral("/skill.md"));
        if (!content.isEmpty()) {
            QString rel = currentDir.mid(baseDir.size());
            while (rel.startsWith(QLatin1Char('/')))
                rel.remove(0, 1);
            const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);

            HermesSkillInfo info;
            QStringList tags;
            const QMap<QString, QString> meta = parseFrontmatter(content, &tags);
            info.dirName = parts.isEmpty() ? QString() : parts.last();
            info.category = parts.isEmpty() ? QString() : parts.first();
            info.name = meta.value(QStringLiteral("name"), info.dirName);
            info.description = meta.value(QStringLiteral("description"));
            info.version = meta.value(QStringLiteral("version"));
            info.author = meta.value(QStringLiteral("author"));
            info.tags = tags;
            info.content = content;
            skills.append(info);
        }
    }

    // 继续递归子目录查找更多 skills
    for (const QString &entry : entries) {
        if (entry.startsWith(QLatin1Char('.'))
            || entry.endsWith(QLatin1String(".md")))
            continue;
        // 跳过常见非 skill 资源目录
        if (entry == QLatin1String("references") || entry == QLatin1String("templates")
            || entry == QLatin1String("assets") || entry == QLatin1String("examples")
            || entry == QLatin1String("__pycache__"))
            continue;
        const QString subPath = currentDir + QLatin1Char('/') + entry;
        // 只递归目录(能 list 出内容即视为目录)
        const QStringList subEntries = m_backend->listDir(subPath);
        if (!subEntries.isEmpty())
            findSkillsRecursive(baseDir, subPath, skills, maxDepth, depth + 1);
    }
}

// 对应Python: SkillsWorker._load_skills
void HermesGateway::loadSkills()
{
    schedule([this]() {
        const QString skillsDir =
            m_backend->hermesHome() + QStringLiteral("/skills");
        QList<HermesSkillInfo> skills;
        if (m_backend->fileExists(skillsDir))
            findSkillsRecursive(skillsDir, skillsDir, skills, 4, 0);
        emit skillsLoaded(skills);
    });
}

// 对应Python: SkillsWorker._install_skill
void HermesGateway::installSkill(const QString &name)
{
    if (name.isEmpty()) {
        emit errorOccurred(QStringLiteral("Skill 名称不能为空"));
        return;
    }
    schedule([this, name]() {
        const QString output = m_backend->execCli(
            {QStringLiteral("skills"), QStringLiteral("install"), name});
        emit commandDone(QStringLiteral("安装 Skill: %1").arg(name), output);
    });
}

// 对应Python: SkillsWorker._delete_skill
void HermesGateway::removeSkill(const QString &name)
{
    if (name.isEmpty()) {
        emit errorOccurred(QStringLiteral("Skill 名称不能为空"));
        return;
    }
    schedule([this, name]() {
        const QString output = m_backend->execCli(
            {QStringLiteral("skills"), QStringLiteral("remove"), name});
        emit commandDone(QStringLiteral("删除 Skill: %1").arg(name), output);
    });
}

} // namespace cubeshell
