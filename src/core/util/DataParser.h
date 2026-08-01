#pragma once

// DataParser.h — 远程命令输出解析（/proc/net/dev、/proc/stat、df、iostat、
// uptime、free、hostnamectl）与 JSON 读写帮助函数。
// 对应Python: function/parse_data.py + function/util.py（read_json/write_json）

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>

namespace cubeshell {
namespace DataParser {

// 单个网络接口的累计计数
// 对应Python: function/parse_data.py::parse_network_data 返回的 dict 值
struct NetworkInterfaceData {
    qint64 rxBytes = 0;
    qint64 rxPackets = 0;
    qint64 rxErrors = 0;
    qint64 rxDropped = 0;
    qint64 txBytes = 0;
    qint64 txPackets = 0;
    qint64 txErrors = 0;
    qint64 txDropped = 0;
};

// 接口速率统计
// 对应Python: function/parse_data.py::calculate_network_speed 返回的 interfaces 元素
struct NetworkInterfaceStat {
    QString name;
    double rxSpeed = 0.0;   // bytes/s
    double txSpeed = 0.0;   // bytes/s
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
    qint64 rxErrors = 0;
    qint64 txErrors = 0;
};

// 解析 /proc/net/dev 输出（跳过前两行标题与 lo 接口）
// 对应Python: function/parse_data.py::parse_network_data
QHash<QString, NetworkInterfaceData> parseNetworkData(const QString &output);

// 根据两次网络快照计算速率
// 对应Python: function/parse_data.py::calculate_network_speed
QList<NetworkInterfaceStat> calculateNetworkSpeed(
    const QHash<QString, NetworkInterfaceData> &prevData,
    const QHash<QString, NetworkInterfaceData> &currData,
    double intervalSeconds);

// 识别主要网络接口（rx+tx 速率最大者）；接口为空时返回 false
// 对应Python: function/parse_data.py::get_main_interface
bool getMainInterface(const QList<NetworkInterfaceStat> &interfaces,
                      NetworkInterfaceStat *out);

// CPU 快照：total 为总行 8 个计数，cores 为每核 8 个计数
// 对应Python: function/parse_data.py::parse_cpu_data 返回的 dict
struct CpuData {
    QVector<qint64> total;            // user,nice,system,idle,iowait,irq,softirq,steal
    QList<QVector<qint64>> cores;
    bool isValid() const { return total.size() == 8; }
};

// CPU 使用率结果
// 对应Python: function/parse_data.py::calculate_cpu_usage 返回的 dict
struct CpuUsage {
    double totalUsage = 0.0;
    double userUsage = 0.0;
    double systemUsage = 0.0;
    double iowait = 0.0;
    QList<double> coresUsage;
};

// 解析 /proc/stat 输出
// 对应Python: function/parse_data.py::parse_cpu_data
CpuData parseCpuData(const QString &output);

// 根据两次 CPU 快照计算使用率
// 对应Python: function/parse_data.py::calculate_cpu_usage
CpuUsage calculateCpuUsage(const CpuData &prevData, const CpuData &currData);

// 解析带单位的大小值（如 "4.9G"、"550M"），返回以 MB 为单位的值
// 对应Python: function/parse_data.py::parse_size_value
double parseSizeValue(const QString &sizeStr);

// df 输出中的单个分区
// 对应Python: function/parse_data.py::parse_disk_data 返回的 dict 元素
struct DiskPartition {
    QString filesystem;
    QString size;
    double sizeMb = 0.0;
    QString used;
    double usedMb = 0.0;
    QString available;
    double availableMb = 0.0;
    double usagePercent = 0.0;
    QString mountPoint;
};

// 解析 df 命令输出（过滤 tmpfs/devtmpfs/overlay/squashfs/loop/none/udev）
// 对应Python: function/parse_data.py::parse_disk_data
QList<DiskPartition> parseDiskData(const QString &output);

// 单设备 IO 统计
// 对应Python: function/parse_data.py::parse_io_data 返回的 dict 值
struct IoStat {
    double readsPerSec = 0.0;
    double writesPerSec = 0.0;
    double ioPercent = 0.0;
};

// 解析 iostat 命令输出
// 对应Python: function/parse_data.py::parse_io_data
QHash<QString, IoStat> parseIoData(const QString &output);

// 解析 uptime 输出中的负载平均值，失败返回 {0,0,0}
// 对应Python: function/parse_data.py::parse_load_average
QList<double> parseLoadAverage(const QString &output);

// 内存统计（各字段单位 MB，usagePercent 为百分比）
// 对应Python: function/parse_data.py::parse_memory_data 返回的 dict
struct MemoryStats {
    double total = 0.0;
    double used = 0.0;
    double free = 0.0;
    double shared = 0.0;
    double cache = 0.0;
    double available = 0.0;
    double usagePercent = 0.0;
};

// 解析 free 命令输出
// 对应Python: function/parse_data.py::parse_memory_data
MemoryStats parseMemoryData(const QString &output);

// 解析 hostnamectl 输出为键值对
// 对应Python: function/parse_data.py::parse_hostnamectl_output
QHash<QString, QString> parseHostnamectlOutput(const QString &output);

// 读取 JSON 文件为对象；失败返回空对象（errorOut 携带原因）
// 对应Python: function/util.py::read_json / read_json_file
QJsonObject readJsonFile(const QString &filePath, QString *errorOut = nullptr);

// 写入 JSON 对象到文件（indent=4 与 Python 侧一致）
// 对应Python: function/util.py::write_json
bool writeJsonFile(const QString &filePath, const QJsonObject &data,
                   QString *errorOut = nullptr);

} // namespace DataParser
} // namespace cubeshell
