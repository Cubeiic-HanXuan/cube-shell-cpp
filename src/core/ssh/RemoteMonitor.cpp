// RemoteMonitor.cpp — periodic remote metrics collection. See RemoteMonitor.h.

#include "RemoteMonitor.h"

#include "CommandExecutor.h"
#include "SshClient.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

Q_DECLARE_LOGGING_CATEGORY(monitorLog)
Q_LOGGING_CATEGORY(monitorLog, "cubeshell.ssh.monitor")

namespace cubeshell {

// Marker separating the batched command outputs (unlikely in real output).
static const QString &monitorSep()
{
    static const QString sep = QStringLiteral("__CUBESHELL_MON_SEP__");
    return sep;
}

// 对应Python: get_datas 里的失败重试间隔 time.sleep(5)
static constexpr int kErrorRetryMs = 5000;

RemoteMonitor::RemoteMonitor(SshClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    // Queued cross-thread delivery of the custom payload types. Register the
    // bare name too — moc records the parameter type as written in-namespace.
    qRegisterMetaType<RemoteStats>("cubeshell::RemoteStats");
    qRegisterMetaType<RemoteStats>("RemoteStats");
    qRegisterMetaType<QHash<QString, QString>>("QHash<QString,QString>");
}

RemoteMonitor::~RemoteMonitor()
{
    // Join before destruction — the loop only emits while the thread lives,
    // so no signal can fire on a dead object (PySide6 线程安全经验的 C++ 等价).
    stop();
}

void RemoteMonitor::setIntervalMs(int intervalMs)
{
    m_intervalMs.store(qMax(200, intervalMs));
}

int RemoteMonitor::intervalMs() const
{
    return m_intervalMs.load();
}

void RemoteMonitor::start()
{
    if (m_running.load())
        return;
    if (m_thread) { // reap a previous, already-stopped thread
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    m_running.store(true);
    m_cancelFlag.store(false);
    m_thread = QThread::create([this]() { monitorLoop(); });
    m_thread->setObjectName(QStringLiteral("cubeshell-remote-monitor"));
    m_thread->start();
}

void RemoteMonitor::stop()
{
    m_running.store(false);
    m_cancelFlag.store(true); // abort an in-flight runCommand promptly
    m_sleepCond.wakeAll();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
}

bool RemoteMonitor::sleepInterruptible(int ms)
{
    QMutexLocker locker(&m_sleepMutex);
    QElapsedTimer t;
    t.start();
    while (m_running.load() && t.elapsed() < ms)
        m_sleepCond.wait(&m_sleepMutex, quint64(qMax<qint64>(1, ms - t.elapsed())));
    return m_running.load();
}

// Format seconds of uptime as "3d 4h" / "4h 12m" / "12m".
// 对应Python: ssh_func.py::get_datas 的 uptime_str 组装
static QString formatUptime(double secs)
{
    const int days = int(secs / 86400);
    const int hours = int((qint64(secs) % 86400) / 3600);
    const int mins = int((qint64(secs) % 3600) / 60);
    if (days > 0)
        return QStringLiteral("%1d %2h").arg(days).arg(hours);
    if (hours > 0)
        return QStringLiteral("%1h %2m").arg(hours).arg(mins);
    return QStringLiteral("%1m").arg(mins);
}

// 对应Python: ssh_func.py::get_datas（监控主循环）
void RemoteMonitor::monitorLoop()
{
    // --- one-shot host info (对应Python: conn.exec('hostnamectl')) ---
    {
        const ExecResult host = CommandExecutor::runCommand(
            m_client, QStringLiteral("hostnamectl"), false,
            CommandExecutor::kDefaultTimeoutMs, QByteArray(), &m_cancelFlag);
        if (m_running.load())
            emit systemInfoReady(DataParser::parseHostnamectlOutput(host.stdoutText));
    }

    // Batched collection command — the individual pieces are exactly the
    // Python-side commands, glued with echo markers so one SSH round-trip
    // returns every metric:
    //   cat /proc/stat                          对应 get_cpu_stats
    //   free -m                                 对应 get_memory_stats
    //   df -h                                   对应 get_disk_stats
    //   iostat -d -x 1 2 | tail -n +4           对应 get_disk_stats（可能不存在）
    //   cat /proc/uptime                        对应 get_datas 的运行时长
    //   cat /proc/net/dev                       对应 get_network_stats
    //   uptime                                  对应 parse_load_average
    const QString batchCmd =
        QStringLiteral("cat /proc/stat; echo %1; "
                       "free -m; echo %1; "
                       "df -h; echo %1; "
                       "(iostat -d -x 1 2 | tail -n +4) 2>/dev/null; echo %1; "
                       "cat /proc/uptime; echo %1; "
                       "cat /proc/net/dev; echo %1; "
                       "uptime")
            .arg(monitorSep());

    DataParser::CpuData prevCpu;
    QHash<QString, DataParser::NetworkInterfaceData> prevNet;
    QElapsedTimer snapshotTimer; // interval between consecutive net/cpu snapshots

    while (m_running.load()) {
        const ExecResult res = CommandExecutor::runCommand(
            m_client, batchCmd, false, CommandExecutor::kDefaultTimeoutMs,
            QByteArray(), &m_cancelFlag);

        if (!m_running.load())
            break;

        if (!res.ok()) {
            // 对应Python: get_datas 的 except 分支 — 报告并延迟重试
            qCWarning(monitorLog) << "monitor cycle failed:" << res.errorMessage;
            emit monitorError(res.errorMessage.isEmpty()
                                  ? QStringLiteral("monitor command timed out")
                                  : res.errorMessage);
            if (!sleepInterruptible(kErrorRetryMs))
                break;
            continue;
        }

        const QStringList sections = res.stdoutText.split(monitorSep());
        if (sections.size() < 7) {
            emit monitorError(QStringLiteral("unexpected monitor output (%1 sections)")
                                  .arg(sections.size()));
            if (!sleepInterruptible(kErrorRetryMs))
                break;
            continue;
        }

        RemoteStats stats;

        // --- CPU (delta vs. previous cycle) 对应Python: get_cpu_stats ---
        const DataParser::CpuData cpuNow = DataParser::parseCpuData(sections.at(0));
        if (cpuNow.isValid() && prevCpu.isValid()) {
            stats.cpu = DataParser::calculateCpuUsage(prevCpu, cpuNow);
            stats.cpuValid = true;
        }

        // --- memory 对应Python: get_memory_stats ---
        stats.memory = DataParser::parseMemoryData(sections.at(1));

        // --- disk 对应Python: get_disk_stats ---
        stats.diskPartitions = DataParser::parseDiskData(sections.at(2));
        stats.diskIo = DataParser::parseIoData(sections.at(3));
        double totalSize = 0.0, totalUsed = 0.0;
        for (const DataParser::DiskPartition &p : stats.diskPartitions) {
            totalSize += p.sizeMb;
            totalUsed += p.usedMb;
            if (p.mountPoint == QStringLiteral("/"))
                stats.diskRootUsage = p.usagePercent;
        }
        stats.diskTotalUsage = totalSize > 0 ? (totalUsed / totalSize) * 100.0 : 0.0;

        // --- uptime 对应Python: get_datas 的 cat /proc/uptime ---
        const QStringList uptimeParts =
            sections.at(4).trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!uptimeParts.isEmpty()) {
            bool okSecs = false;
            const double secs = uptimeParts.first().toDouble(&okSecs);
            if (okSecs)
                stats.uptimeText = formatUptime(secs);
        }

        // --- network (delta vs. previous cycle) 对应Python: get_network_stats ---
        const QHash<QString, DataParser::NetworkInterfaceData> netNow =
            DataParser::parseNetworkData(sections.at(5));
        if (!netNow.isEmpty() && !prevNet.isEmpty() && snapshotTimer.isValid()) {
            const double interval = double(snapshotTimer.elapsed()) / 1000.0;
            if (interval > 0.0) {
                stats.networkInterfaces =
                    DataParser::calculateNetworkSpeed(prevNet, netNow, interval);
                DataParser::NetworkInterfaceStat main;
                if (DataParser::getMainInterface(stats.networkInterfaces, &main)) {
                    stats.rxSpeed = main.rxSpeed;
                    stats.txSpeed = main.txSpeed;
                }
                stats.networkValid = true;
            }
        }

        // --- load average 对应Python: parse_data.parse_load_average(uptime) ---
        stats.loadAverage = DataParser::parseLoadAverage(sections.at(6));

        prevCpu = cpuNow;
        prevNet = netNow;
        snapshotTimer.restart();

        if (!m_running.load())
            break;
        emit statsUpdated(stats);

        if (!sleepInterruptible(m_intervalMs.load()))
            break;
    }

    qCInfo(monitorLog) << "remote monitor stopped"; // 对应Python: "系统监控已停止"
}

} // namespace cubeshell
