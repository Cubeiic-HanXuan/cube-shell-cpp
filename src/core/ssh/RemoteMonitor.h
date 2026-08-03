#pragma once

// RemoteMonitor.h — periodic remote system metrics collection over SSH.
//
// C++ port of the monitoring part of function/ssh_func.py::SshClient
// (get_datas / get_cpu_stats / get_memory_stats / get_disk_stats /
// get_network_stats). Differences from the Python original, per the porting
// plan:
//   - All collection commands are batched into ONE SSH exec call per cycle
//     (the Python side issues one exec per metric); the individual commands
//     are identical to the Python ones (cat /proc/stat, free -m, df -h,
//     iostat, cat /proc/uptime, cat /proc/net/dev, uptime).
//   - CPU usage and network speed are derived from the deltas between two
//     consecutive cycles (the Python side sleeps monitor_interval between two
//     snapshots inside one call — same interval, half the SSH round-trips).
//   - Raw values are emitted as-is; EMA smoothing (ssh_func.py::_smooth_value)
//     is left to the UI layer.
//
// Threading: start() spawns a dedicated QThread; statsUpdated() /
// systemInfoReady() are emitted from that thread — consumers MUST connect
// with Qt::QueuedConnection. stop() REQUESTS the loop to stop and waits only
// briefly (bounded) for it to finish; if the thread is stuck in a blocking
// SSH call it is left to finish in the background and self-deletes via
// deleteLater, so the UI thread is never parked waiting on network I/O.

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>

#include <atomic>
#include <memory>

#include "util/DataParser.h"

class QThread;

namespace cubeshell {

class SshClient;

// One cycle of raw (unsmoothed) monitoring data.
// 对应Python: ssh_func.py::get_datas 循环里写入 conn.* 的那组值
struct RemoteStats {
    bool cpuValid = false;                     // false on the first cycle (no delta yet)
    DataParser::CpuUsage cpu;                  // 对应 get_cpu_stats()
    DataParser::MemoryStats memory;            // 对应 get_memory_stats()
    QList<DataParser::DiskPartition> diskPartitions; // 对应 get_disk_stats()['partitions']
    QHash<QString, DataParser::IoStat> diskIo; // 对应 get_disk_stats()['io']
    double diskRootUsage = 0.0;                // 对应 get_disk_stats()['root_usage']
    double diskTotalUsage = 0.0;               // 对应 get_disk_stats()['total_usage']
    bool networkValid = false;                 // false on the first cycle
    QList<DataParser::NetworkInterfaceStat> networkInterfaces; // 对应 get_network_stats()
    double rxSpeed = 0.0;                      // main interface, bytes/s
    double txSpeed = 0.0;
    QList<double> loadAverage;                 // 对应 parse_data.parse_load_average
    QString uptimeText;                        // 对应 get_datas 里的 conn.uptime_str（如 "3d 4h"）
};

class RemoteMonitor : public QObject {
    Q_OBJECT
public:
    // 对应Python: ssh_func.py::SshClient.monitor_interval = 2.0
    static constexpr int kDefaultIntervalMs = 2000;

    // client 以 shared_ptr 持有：监控线程运行期间 SshClient 必然存活，即便
    // 本对象/SshTerminalWidget 已先开始析构（极端兜底路径），也不会 UAF。
    explicit RemoteMonitor(std::shared_ptr<SshClient> client, QObject *parent = nullptr);
    ~RemoteMonitor() override;

    // Collection period; takes effect from the next cycle.
    void setIntervalMs(int intervalMs);
    int intervalMs() const;

    bool isRunning() const { return m_running.load(); }

    // Start the monitoring thread. No-op when already running.
    // 对应Python: get_datas 的启动（后台线程）
    void start();

    // Request the loop to stop. Waits only a bounded time for the thread to
    // finish; if it is still blocked in an SSH call it is left running to
    // self-delete, so this never parks the UI thread on network I/O.
    // Safe to call multiple times / when not running.
    // 对应Python: conn.active = False + close_sig
    void stop();

signals:
    // Emitted once after start, with the parsed `hostnamectl` output.
    // 对应Python: get_datas 开头的 conn.system_info_dict
    void systemInfoReady(const QHash<QString, QString> &info);

    // Emitted every cycle from the monitor thread (raw values, no smoothing).
    void statsUpdated(const cubeshell::RemoteStats &stats);

    // A cycle failed (transport error etc.); the loop keeps retrying.
    // 对应Python: get_datas 的 except 分支（记日志后 sleep(5) 重试）
    void monitorError(const QString &message);

private:
    void monitorLoop();
    // Interruptible sleep; returns false when stop() was requested meanwhile.
    bool sleepInterruptible(int ms);

    // 监控线程访问的 SshClient；以 shared_ptr 持有，保证线程存活期间对象有效。
    std::shared_ptr<SshClient> m_client;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelFlag{false}; // aborts an in-flight SSH call on stop()
    std::atomic<int> m_intervalMs{kDefaultIntervalMs};

    QMutex m_sleepMutex;
    QWaitCondition m_sleepCond;
};

} // namespace cubeshell

Q_DECLARE_METATYPE(cubeshell::RemoteStats)
