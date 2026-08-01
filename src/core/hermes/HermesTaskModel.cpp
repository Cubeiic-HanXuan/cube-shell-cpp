// HermesTaskModel.cpp — see HermesTaskModel.h for the port map.
// 对应Python: core/hermes/cron_widget.py 的 CronWorker

#include "hermes/HermesTaskModel.h"

#include "hermes/HermesBackend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtConcurrent>

namespace cubeshell {

// ---------------------------------------------------------------------------
// HermesCronJob
// ---------------------------------------------------------------------------

HermesCronJob HermesCronJob::fromJson(const QJsonObject &obj, const QString &profile)
{
    HermesCronJob job;
    job.raw = obj;
    job.id = obj.value(QStringLiteral("id")).toVariant().toString();
    job.name = obj.value(QStringLiteral("name")).toString();

    // schedule 可能是对象 {"expr": "...", "display": "..."} 或字符串
    // 对应Python: CronJobDialog._populate_data 的 schedule 兼容处理
    const QJsonValue sched = obj.value(QStringLiteral("schedule"));
    if (sched.isObject()) {
        const QJsonObject so = sched.toObject();
        job.schedule = so.value(QStringLiteral("display")).toString();
        if (job.schedule.isEmpty())
            job.schedule = so.value(QStringLiteral("expr")).toString();
    } else {
        job.schedule = sched.toVariant().toString();
    }

    // 状态兼容 state/status/enabled 字段
    // 对应Python: _populate_table 的 status 计算
    job.state = obj.value(QStringLiteral("state"))
                    .toString(obj.value(QStringLiteral("status"))
                                  .toString(QStringLiteral("unknown")));
    job.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
    if (!job.enabled)
        job.state = QStringLiteral("paused");

    // 上次运行兼容 last_run_at/last_run
    job.lastRun = obj.value(QStringLiteral("last_run_at"))
                      .toString(obj.value(QStringLiteral("last_run")).toString());
    // 投递目标兼容 deliver/deliver_to
    job.deliver = obj.value(QStringLiteral("deliver"))
                      .toString(obj.value(QStringLiteral("deliver_to"))
                                    .toString(QStringLiteral("none")));
    job.prompt = obj.value(QStringLiteral("prompt")).toString();

    job.profileSource = obj.value(QStringLiteral("profile_source")).toString();
    if (job.profileSource.isEmpty())
        job.profileSource = profile;
    return job;
}

// ---------------------------------------------------------------------------
// HermesTaskModel
// ---------------------------------------------------------------------------

HermesTaskModel::HermesTaskModel(HermesBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
}

HermesTaskModel::~HermesTaskModel()
{
    for (QFuture<void> &f : m_futures)
        f.waitForFinished();
}

void HermesTaskModel::schedule(std::function<void()> job)
{
    for (int i = m_futures.size() - 1; i >= 0; --i) {
        if (m_futures.at(i).isFinished())
            m_futures.removeAt(i);
    }
    m_futures.append(QtConcurrent::run(std::move(job)));
}

QStringList HermesTaskModel::profileArgs(const QString &profile) const
{
    // 对应Python: args = ["-p", profile] + ... 的 profile 前缀
    if (profile.isEmpty())
        return {};
    return {QStringLiteral("-p"), profile};
}

// 对应Python: CronWorker._parse_jobs_file
QList<HermesCronJob> HermesTaskModel::parseJobsFile(const QString &content,
                                                    const QString &profileName)
{
    QList<HermesCronJob> jobs;
    if (content.trimmed().isEmpty())
        return jobs;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return jobs;

    QJsonArray arr;
    if (doc.isObject())
        arr = doc.object().value(QStringLiteral("jobs")).toArray();
    else if (doc.isArray())
        arr = doc.array();

    for (const QJsonValue &v : arr) {
        if (v.isObject())
            jobs.append(HermesCronJob::fromJson(v.toObject(), profileName));
    }
    return jobs;
}

// 对应Python: CronWorker._do_load_jobs
void HermesTaskModel::loadJobs()
{
    schedule([this]() {
        const QString home = m_backend->hermesHome();
        QList<HermesCronJob> allJobs;

        // 扫描所有 profile 目录下的 cron/jobs.json
        const QString profilesDir = home + QStringLiteral("/profiles");
        if (m_backend->fileExists(profilesDir)) {
            const QStringList profiles = m_backend->listDir(profilesDir);
            for (const QString &profileName : profiles) {
                const QString jobsPath = profilesDir + QLatin1Char('/')
                    + profileName + QStringLiteral("/cron/jobs.json");
                if (m_backend->fileExists(jobsPath))
                    allJobs += parseJobsFile(m_backend->readFile(jobsPath),
                                             profileName);
            }
        }
        // 也检查顶层 cron/jobs.json(default profile 可能存此处)
        const QString defaultJobsPath = home + QStringLiteral("/cron/jobs.json");
        if (m_backend->fileExists(defaultJobsPath))
            allJobs += parseJobsFile(m_backend->readFile(defaultJobsPath),
                                     QStringLiteral("default"));

        emit jobsLoaded(allJobs);
    });
}

// 对应Python: CronWorker._do_create_job
void HermesTaskModel::createJob(const QString &scheduleExpr, const QString &prompt,
                                const QString &name, const QString &skills,
                                const QString &deliverTo)
{
    schedule([this, scheduleExpr, prompt, name, skills, deliverTo]() {
        QStringList args{QStringLiteral("cron"), QStringLiteral("create"),
                         scheduleExpr, prompt};
        if (!name.isEmpty())
            args << QStringLiteral("--name") << name;
        if (!skills.isEmpty())
            args << QStringLiteral("--skills") << skills;
        if (!deliverTo.isEmpty() && deliverTo != QLatin1String("none"))
            args << QStringLiteral("--deliver") << deliverTo;
        const QString output = m_backend->execCli(args);
        emit commandDone(QStringLiteral("创建任务"), output);
    });
}

// 对应Python: CronWorker._do_delete_job
void HermesTaskModel::removeJob(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        QStringList args = profileArgs(profile);
        args << QStringLiteral("cron") << QStringLiteral("remove") << jobId;
        emit commandDone(QStringLiteral("删除任务"), m_backend->execCli(args));
    });
}

// 对应Python: CronWorker._do_pause_resume("pause")
void HermesTaskModel::pauseJob(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        QStringList args = profileArgs(profile);
        args << QStringLiteral("cron") << QStringLiteral("pause") << jobId;
        emit commandDone(QStringLiteral("暂停任务"), m_backend->execCli(args));
    });
}

// 对应Python: CronWorker._do_pause_resume("resume")
void HermesTaskModel::resumeJob(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        QStringList args = profileArgs(profile);
        args << QStringLiteral("cron") << QStringLiteral("resume") << jobId;
        emit commandDone(QStringLiteral("恢复任务"), m_backend->execCli(args));
    });
}

// 对应Python: CronWorker._do_run_job
void HermesTaskModel::runJobNow(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        // 1. 标记任务为立即触发
        QStringList args = profileArgs(profile);
        args << QStringLiteral("cron") << QStringLiteral("run") << jobId;
        const QString output = m_backend->execCli(args);
        // 2. 强制执行一次 tick,确保任务立即运行(不等 gateway 60s 轮询)
        QStringList tickArgs = profileArgs(profile);
        tickArgs << QStringLiteral("cron") << QStringLiteral("tick")
                 << QStringLiteral("--accept-hooks");
        m_backend->execCli(tickArgs, 120 * 1000);
        emit commandDone(QStringLiteral("立即执行"), output);
    });
}

// 对应Python: CronWorker._do_load_output
void HermesTaskModel::loadOutput(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        const QString home = m_backend->hermesHome();
        // 优先在 profile 目录下查找输出
        QString outputDir = profile.isEmpty()
            ? home + QStringLiteral("/cron/output/") + jobId
            : home + QStringLiteral("/profiles/") + profile
                  + QStringLiteral("/cron/output/") + jobId;
        if (!m_backend->fileExists(outputDir)) {
            // fallback: 尝试顶层目录
            outputDir = home + QStringLiteral("/cron/output/") + jobId;
            if (!m_backend->fileExists(outputDir)) {
                emit outputLoaded(QStringLiteral("暂无执行日志"));
                return;
            }
        }
        QStringList files = m_backend->listDir(outputDir);
        if (files.isEmpty()) {
            emit outputLoaded(QStringLiteral("暂无执行日志"));
            return;
        }
        // 按文件名倒序取最近的日志(假设文件名包含时间戳)
        std::sort(files.begin(), files.end(), std::greater<QString>());
        const QStringList recent = files.mid(0, 5);

        QString logContent;
        for (const QString &fname : recent) {
            const QString content =
                m_backend->readFile(outputDir + QLatin1Char('/') + fname);
            logContent += QStringLiteral("━━━ %1 ━━━\n%2\n\n").arg(fname, content);
        }
        emit outputLoaded(logContent.isEmpty()
                              ? QStringLiteral("暂无执行日志") : logContent);
    });
}

// 对应Python: CronWorker._do_clear_output
void HermesTaskModel::clearOutput(const QString &jobId, const QString &profile)
{
    schedule([this, jobId, profile]() {
        const QString home = m_backend->hermesHome();
        const QString outputDir = profile.isEmpty()
            ? home + QStringLiteral("/cron/output/") + jobId
            : home + QStringLiteral("/profiles/") + profile
                  + QStringLiteral("/cron/output/") + jobId;
        if (!m_backend->fileExists(outputDir)) {
            emit commandDone(QStringLiteral("清除日志"), QStringLiteral("无日志可清除"));
            return;
        }
        const QStringList files = m_backend->listDir(outputDir);
        int count = 0;
        for (const QString &fname : files) {
            if (m_backend->deleteFile(outputDir + QLatin1Char('/') + fname))
                ++count;
        }
        emit commandDone(QStringLiteral("清除日志"),
                         QStringLiteral("已清除 %1 条日志").arg(count));
    });
}

// ---------------------------------------------------------------------------
// cron expression validation
// ---------------------------------------------------------------------------

// 每个字段允许:* 或 数字/范围/列表,可带 /step。范围按字段位置校验。
static bool validateCronField(const QString &field, int minVal, int maxVal)
{
    static const QRegularExpression tokenRe(
        QStringLiteral("^(\\*|\\d+(-\\d+)?)(/\\d+)?$"));
    const QStringList tokens = field.split(QLatin1Char(','));
    if (tokens.isEmpty())
        return false;
    for (const QString &token : tokens) {
        const QRegularExpressionMatch m = tokenRe.match(token);
        if (!m.hasMatch())
            return false;
        if (token.startsWith(QLatin1Char('*')))
            continue;
        const QString base = token.section(QLatin1Char('/'), 0, 0);
        const QStringList range = base.split(QLatin1Char('-'));
        for (const QString &numStr : range) {
            bool ok = false;
            const int num = numStr.toInt(&ok);
            if (!ok || num < minVal || num > maxVal)
                return false;
        }
        if (range.size() == 2 && range.at(0).toInt() > range.at(1).toInt())
            return false;
    }
    return true;
}

bool HermesTaskModel::validateCronExpression(const QString &expr, QString *errorOut)
{
    const QString trimmed = expr.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("计划表达式不能为空");
        return false;
    }
    // @ 别名(hermes CLI 常见支持)
    static const QStringList aliases = {
        QStringLiteral("@yearly"), QStringLiteral("@annually"),
        QStringLiteral("@monthly"), QStringLiteral("@weekly"),
        QStringLiteral("@daily"), QStringLiteral("@hourly"),
    };
    if (aliases.contains(trimmed))
        return true;

    const QStringList fields =
        trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                      Qt::SkipEmptyParts);
    if (fields.size() != 5) {
        if (errorOut)
            *errorOut = QStringLiteral("cron 表达式应为 5 个字段(分 时 日 月 周)");
        return false;
    }
    struct { int minVal; int maxVal; const char *label; } limits[5] = {
        {0, 59, "分钟"}, {0, 23, "小时"}, {1, 31, "日"},
        {1, 12, "月"}, {0, 7, "星期"},
    };
    for (int i = 0; i < 5; ++i) {
        if (!validateCronField(fields.at(i), limits[i].minVal, limits[i].maxVal)) {
            if (errorOut)
                *errorOut = QStringLiteral("字段 %1(%2)非法: %3")
                                .arg(i + 1)
                                .arg(QString::fromUtf8(limits[i].label),
                                     fields.at(i));
            return false;
        }
    }
    return true;
}

} // namespace cubeshell
