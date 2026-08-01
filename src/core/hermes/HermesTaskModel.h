#pragma once

// HermesTaskModel.h — Hermes cron job CRUD + schedule validation.
// 对应Python: core/hermes/cron_widget.py 的 CronWorker(非 UI 逻辑部分)
//
// - job scanning       对应Python: CronWorker._do_load_jobs/_parse_jobs_file
// - create/remove/...  对应Python: _do_create_job/_do_delete_job/_do_pause_resume
// - run now + tick     对应Python: _do_run_job(cron run + cron tick --accept-hooks)
// - output logs        对应Python: _do_load_output/_do_clear_output
// - cron validation    5 字段 cron 表达式基础校验(C++ 侧新增,便于 UI 提示)
//
// All operations run on the global thread pool (QtConcurrent); signals are
// emitted from worker threads — consumers MUST connect with
// Qt::QueuedConnection.

#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace cubeshell {

class HermesBackend;

// One cron job entry from jobs.json.
// 对应Python: cron_widget 中 job dict 的常用字段(兼容多种命名)
struct HermesCronJob {
    QString id;
    QString name;
    QString schedule;      // display 或 expr,已展平为字符串
    QString state;         // active / scheduled / paused / error / unknown
    bool enabled = true;
    QString lastRun;       // last_run_at / last_run
    QString deliver;       // deliver / deliver_to
    QString prompt;
    QString profileSource; // 来源 profile 目录名
    QJsonObject raw;       // 原始 JSON(详情展示用)

    static HermesCronJob fromJson(const QJsonObject &obj, const QString &profile);
};

class HermesTaskModel : public QObject {
    Q_OBJECT
public:
    explicit HermesTaskModel(HermesBackend *backend, QObject *parent = nullptr);
    ~HermesTaskModel() override;

    // Scan profiles/*/cron/jobs.json + top-level cron/jobs.json.
    // 对应Python: CronWorker._do_load_jobs
    void loadJobs();

    // 对应Python: CronWorker._do_create_job
    void createJob(const QString &schedule, const QString &prompt,
                   const QString &name = QString(),
                   const QString &skills = QString(),
                   const QString &deliverTo = QString());
    // 对应Python: CronWorker._do_delete_job
    void removeJob(const QString &jobId, const QString &profile = QString());
    // 对应Python: CronWorker._do_pause_resume
    void pauseJob(const QString &jobId, const QString &profile = QString());
    void resumeJob(const QString &jobId, const QString &profile = QString());
    // 对应Python: CronWorker._do_run_job(run + tick --accept-hooks,120s)
    void runJobNow(const QString &jobId, const QString &profile = QString());

    // 对应Python: CronWorker._do_load_output(最近 5 个日志文件拼接)
    void loadOutput(const QString &jobId, const QString &profile = QString());
    // 对应Python: CronWorker._do_clear_output
    void clearOutput(const QString &jobId, const QString &profile = QString());

    // --- pure helpers (unit-testable) ---

    // 对应Python: CronWorker._parse_jobs_file(兼容 {jobs:[...]} 与 [...])
    static QList<HermesCronJob> parseJobsFile(const QString &content,
                                              const QString &profileName);
    // Basic 5-field cron expression check (also accepts @hourly 等别名).
    static bool validateCronExpression(const QString &expr,
                                       QString *errorOut = nullptr);

signals:
    // Emitted from worker threads — connect with Qt::QueuedConnection.
    void jobsLoaded(const QList<cubeshell::HermesCronJob> &jobs);
    void commandDone(const QString &description, const QString &output);
    void outputLoaded(const QString &logContent);
    void errorOccurred(const QString &message);

private:
    void schedule(std::function<void()> job);
    QStringList profileArgs(const QString &profile) const;

    HermesBackend *m_backend = nullptr; // not owned
    QVector<QFuture<void>> m_futures;   // joined in destructor
};

} // namespace cubeshell
