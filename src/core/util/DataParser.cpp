// DataParser.cpp — 远程命令输出解析与 JSON 帮助函数实现。
// 对应Python: function/parse_data.py + function/util.py（read_json/write_json）

#include "DataParser.h"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace cubeshell {
namespace DataParser {

// 对应Python: function/parse_data.py::parse_network_data
QHash<QString, NetworkInterfaceData> parseNetworkData(const QString &output)
{
    QHash<QString, NetworkInterfaceData> interfaces;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));

    // 跳过前两行（标题行）
    for (int i = 2; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;

        const QString name = line.left(colon).trimmed();
        // 跳过lo接口
        if (name == QStringLiteral("lo"))
            continue;

        const QStringList values = line.mid(colon + 1).split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (values.size() >= 16) {
            NetworkInterfaceData data;
            data.rxBytes = values[0].toLongLong();
            data.rxPackets = values[1].toLongLong();
            data.rxErrors = values[2].toLongLong();
            data.rxDropped = values[3].toLongLong();
            data.txBytes = values[8].toLongLong();
            data.txPackets = values[9].toLongLong();
            data.txErrors = values[10].toLongLong();
            data.txDropped = values[11].toLongLong();
            interfaces.insert(name, data);
        }
    }
    return interfaces;
}

// 对应Python: function/parse_data.py::calculate_network_speed
QList<NetworkInterfaceStat> calculateNetworkSpeed(
    const QHash<QString, NetworkInterfaceData> &prevData,
    const QHash<QString, NetworkInterfaceData> &currData,
    double intervalSeconds)
{
    if (intervalSeconds <= 0)
        intervalSeconds = 0.1; // 防止除零错误

    QList<NetworkInterfaceStat> stats;
    for (auto it = currData.constBegin(); it != currData.constEnd(); ++it) {
        const auto prevIt = prevData.constFind(it.key());
        if (prevIt == prevData.constEnd())
            continue;
        const NetworkInterfaceData &curr = it.value();
        const NetworkInterfaceData &prev = prevIt.value();

        NetworkInterfaceStat stat;
        stat.name = it.key();
        stat.rxSpeed = double(curr.rxBytes - prev.rxBytes) / intervalSeconds;
        stat.txSpeed = double(curr.txBytes - prev.txBytes) / intervalSeconds;
        stat.rxBytes = curr.rxBytes;
        stat.txBytes = curr.txBytes;
        stat.rxErrors = curr.rxErrors;
        stat.txErrors = curr.txErrors;
        stats.append(stat);
    }
    return stats;
}

// 对应Python: function/parse_data.py::get_main_interface
bool getMainInterface(const QList<NetworkInterfaceStat> &interfaces,
                      NetworkInterfaceStat *out)
{
    if (interfaces.isEmpty() || !out)
        return false;
    // 按 rx_speed + tx_speed 取最活跃的接口
    const auto it = std::max_element(
        interfaces.constBegin(), interfaces.constEnd(),
        [](const NetworkInterfaceStat &a, const NetworkInterfaceStat &b) {
            return (a.rxSpeed + a.txSpeed) < (b.rxSpeed + b.txSpeed);
        });
    *out = *it;
    return true;
}

// 对应Python: function/parse_data.py::parse_cpu_data
CpuData parseCpuData(const QString &output)
{
    CpuData result;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    static const QRegularExpression ws(QStringLiteral("\\s+"));

    for (const QString &line : lines) {
        const QStringList parts = line.split(ws, Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;

        auto parseCounters = [&parts]() {
            // user, nice, system, idle, iowait, irq, softirq, steal
            QVector<qint64> counters;
            for (int i = 1; i < parts.size() && i < 9; ++i)
                counters.append(parts[i].toLongLong());
            return counters;
        };

        if (parts[0] == QStringLiteral("cpu")) { // 总CPU行
            result.total = parseCounters();
        } else if (parts[0].startsWith(QStringLiteral("cpu"))) { // CPU核心
            bool isDigit = !parts[0].mid(3).isEmpty();
            for (const QChar &ch : parts[0].mid(3)) {
                if (!ch.isDigit()) {
                    isDigit = false;
                    break;
                }
            }
            if (isDigit)
                result.cores.append(parseCounters());
        }
    }
    return result;
}

// 对应Python: function/parse_data.py::calculate_cpu_usage
CpuUsage calculateCpuUsage(const CpuData &prevData, const CpuData &currData)
{
    // 对应Python: calculate_cpu_usage 内嵌 calculate_usage
    auto calculateUsage = [](const QVector<qint64> &prev, const QVector<qint64> &curr,
                             double *totalOut, double *userOut, double *systemOut,
                             double *iowaitOut) {
        *totalOut = *userOut = *systemOut = *iowaitOut = 0.0;
        if (prev.size() < 8 || curr.size() < 8)
            return;
        qint64 prevTotal = 0, currTotal = 0;
        for (qint64 v : prev)
            prevTotal += v;
        for (qint64 v : curr)
            currTotal += v;

        const qint64 deltaTotal = currTotal - prevTotal;
        const qint64 deltaIdle = curr[3] - prev[3];
        const qint64 deltaIowait = curr[4] - prev[4];
        const qint64 deltaUser = (curr[0] + curr[1]) - (prev[0] + prev[1]); // user + nice
        const qint64 deltaSystem = curr[2] - prev[2];                       // system

        // 避免除零错误
        if (deltaTotal == 0)
            return;

        *totalOut = 100.0 * (1.0 - double(deltaIdle) / double(deltaTotal));
        *userOut = 100.0 * double(deltaUser) / double(deltaTotal);
        *systemOut = 100.0 * double(deltaSystem) / double(deltaTotal);
        *iowaitOut = 100.0 * double(deltaIowait) / double(deltaTotal);
    };

    CpuUsage usage;
    calculateUsage(prevData.total, currData.total,
                   &usage.totalUsage, &usage.userUsage, &usage.systemUsage, &usage.iowait);

    const int coreCount = qMin(prevData.cores.size(), currData.cores.size());
    for (int i = 0; i < coreCount; ++i) {
        double coreTotal = 0.0, u = 0.0, s = 0.0, w = 0.0;
        calculateUsage(prevData.cores[i], currData.cores[i], &coreTotal, &u, &s, &w);
        usage.coresUsage.append(coreTotal);
    }
    return usage;
}

// 对应Python: function/parse_data.py::parse_size_value
double parseSizeValue(const QString &sizeStr)
{
    // 去除字符串中可能的颜色代码
    static const QRegularExpression colorCode(QStringLiteral("\\x1b\\[[0-9;]*m"));
    QString clean = sizeStr;
    clean.remove(colorCode);

    // 尝试直接解析浮点数
    bool ok = false;
    const double direct = clean.toDouble(&ok);
    if (ok)
        return direct;

    // 带单位的解析（K/M/G/T/P，可带 i/B 后缀）
    static const QRegularExpression re(
        QStringLiteral("^([\\d.]+)([KMGTP])?i?[Bb]?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(clean);
    if (match.hasMatch()) {
        double value = match.captured(1).toDouble();
        const QString unit = match.captured(2).toUpper();
        if (unit == QStringLiteral("K"))
            value /= 1024.0;
        else if (unit == QStringLiteral("G"))
            value *= 1024.0;
        else if (unit == QStringLiteral("T"))
            value *= 1024.0 * 1024.0;
        else if (unit == QStringLiteral("P"))
            value *= 1024.0 * 1024.0 * 1024.0;
        // M 或无单位视为 MB，保持原值
        return value;
    }
    return 0.0;
}

// 对应Python: function/parse_data.py::parse_disk_data
QList<DiskPartition> parseDiskData(const QString &output)
{
    QList<DiskPartition> partitions;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    static const QRegularExpression ws(QStringLiteral("\\s+"));

    // 跳过标题行
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList parts = lines.at(i).trimmed().split(ws, Qt::SkipEmptyParts);
        if (parts.size() < 6)
            continue;

        const QString filesystem = parts[0];
        // 过滤伪文件系统 (tmpfs, devtmpfs, overlay, squashfs, loop等)
        if (filesystem == QStringLiteral("tmpfs") || filesystem == QStringLiteral("devtmpfs")
            || filesystem == QStringLiteral("overlay") || filesystem == QStringLiteral("squashfs")
            || filesystem.contains(QStringLiteral("loop")) || filesystem == QStringLiteral("none")
            || filesystem == QStringLiteral("udev")) {
            continue;
        }

        // 从百分比中提取数字
        QString usageStr = parts[4];
        while (usageStr.endsWith(QLatin1Char('%')))
            usageStr.chop(1);
        bool ok = false;
        const double usagePercent = usageStr.toDouble(&ok);
        if (!ok)
            continue;

        DiskPartition partition;
        partition.filesystem = filesystem;
        partition.size = parts[1];
        partition.sizeMb = parseSizeValue(parts[1]);
        partition.used = parts[2];
        partition.usedMb = parseSizeValue(parts[2]);
        partition.available = parts[3];
        partition.availableMb = parseSizeValue(parts[3]);
        partition.usagePercent = usagePercent;
        partition.mountPoint = parts[5];
        partitions.append(partition);
    }
    return partitions;
}

// 对应Python: function/parse_data.py::parse_io_data
QHash<QString, IoStat> parseIoData(const QString &output)
{
    QHash<QString, IoStat> ioStats;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    static const QRegularExpression ws(QStringLiteral("\\s+"));

    for (const QString &line : lines) {
        const QStringList parts = line.trimmed().split(ws, Qt::SkipEmptyParts);
        if (parts.size() < 6)
            continue;

        bool ok2 = false, ok3 = false, okLast = false;
        const double readsPerSec = parts[2].toDouble(&ok2);
        const double writesPerSec = parts[3].toDouble(&ok3);
        // Python 侧优先取 '%util' 列，否则回退最后一列
        const int utilIdx = parts.indexOf(QStringLiteral("%util"));
        const double ioPercent =
            (utilIdx >= 0 ? parts[utilIdx] : parts.last()).toDouble(&okLast);
        if (!ok2 || !ok3 || !okLast)
            continue;

        IoStat stat;
        stat.readsPerSec = readsPerSec;
        stat.writesPerSec = writesPerSec;
        stat.ioPercent = ioPercent;
        ioStats.insert(parts[0], stat);
    }
    return ioStats;
}

// 对应Python: function/parse_data.py::parse_load_average
QList<double> parseLoadAverage(const QString &output)
{
    // 负载格式: load average: 0.52, 0.58, 0.59
    static const QRegularExpression re(QStringLiteral(
        "load average:\\s*([\\d.]+),\\s*([\\d.]+),\\s*([\\d.]+)"));
    const QRegularExpressionMatch match = re.match(output);
    if (match.hasMatch()) {
        return {match.captured(1).toDouble(),
                match.captured(2).toDouble(),
                match.captured(3).toDouble()};
    }
    return {0.0, 0.0, 0.0};
}

// 对应Python: function/parse_data.py::parse_memory_data
MemoryStats parseMemoryData(const QString &output)
{
    MemoryStats stats;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    if (lines.size() < 2)
        return stats;

    // 'free -m' 第二行: Mem: total used free shared buff/cache available
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList memParts = lines[1].trimmed().split(ws, Qt::SkipEmptyParts);
    if (memParts.size() < 7)
        return stats;

    stats.total = parseSizeValue(memParts[1]);
    stats.used = parseSizeValue(memParts[2]);
    stats.free = parseSizeValue(memParts[3]);
    stats.shared = parseSizeValue(memParts[4]);
    stats.cache = parseSizeValue(memParts[5]);
    stats.available = parseSizeValue(memParts[6]);

    // 计算实际使用率(不包括缓存)
    if (stats.total > 0)
        stats.usagePercent = ((stats.total - stats.available) / stats.total) * 100.0;
    return stats;
}

// 对应Python: function/parse_data.py::parse_hostnamectl_output
QHash<QString, QString> parseHostnamectlOutput(const QString &output)
{
    QHash<QString, QString> result;
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        result.insert(key, value);
    }
    return result;
}

// 对应Python: function/util.py::read_json / read_json_file
QJsonObject readJsonFile(const QString &filePath, QString *errorOut)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("The file '%1' was not found.").arg(filePath);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("The file '%1' is not a valid JSON file: %2")
                            .arg(filePath, parseError.errorString());
        return {};
    }
    return doc.object();
}

// 对应Python: function/util.py::write_json（indent=4 让文件具有可读性）
bool writeJsonFile(const QString &filePath, const QJsonObject &data, QString *errorOut)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("cannot open %1 for writing").arg(filePath);
        return false;
    }
    file.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to write %1").arg(filePath);
        return false;
    }
    return true;
}

} // namespace DataParser
} // namespace cubeshell
