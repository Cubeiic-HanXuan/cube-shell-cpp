#include "ProcessTableModel.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace cubeshell {
namespace {
const QString kPsSeparator = QStringLiteral("\n===PS_SS_SEP===\n");
const QString kPathSeparator = QStringLiteral("\n===PS_EXE_SEP===\n");
const QString kEnd = QStringLiteral("\0===PS_SNAPSHOT_END===\0");

QString fieldText(const RemoteProcess &process, int column)
{
    switch (column) {
    case ProcessTableModel::Pid: return QString::number(process.pid);
    case ProcessTableModel::User: return process.user;
    case ProcessTableModel::Memory: return process.memoryText;
    case ProcessTableModel::Cpu: return process.cpuText;
    case ProcessTableModel::Port: return process.port;
    case ProcessTableModel::ExecutablePath: return process.executablePath;
    case ProcessTableModel::Command: return process.command;
    default: return {};
    }
}

QHash<quint64, QString> parsePorts(const QString &text)
{
    QHash<quint64, QStringList> ports;
    static const QRegularExpression pidRe(QStringLiteral("pid=(\\d+)"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("pid="))) continue;
        const QStringList fields = line.simplified().split(QLatin1Char(' '));
        if (fields.size() < 5) continue;
        const QString address = fields.at(4);
        const int colon = address.lastIndexOf(QLatin1Char(':'));
        if (colon < 0) continue;
        const QString port = address.mid(colon + 1);
        auto matches = pidRe.globalMatch(line);
        while (matches.hasNext()) {
            const quint64 pid = matches.next().captured(1).toULongLong();
            if (!ports[pid].contains(port)) ports[pid].append(port);
        }
    }
    QHash<quint64, QString> result;
    for (auto it = ports.begin(); it != ports.end(); ++it) {
        std::sort(it->begin(), it->end(), [](const QString &a, const QString &b) {
            return a.toUInt() < b.toUInt();
        });
        result.insert(it.key(), it->join(QLatin1Char(',')));
    }
    return result;
}
} // namespace

ProcessTableModel::ProcessTableModel(QObject *parent) : QAbstractTableModel(parent) {}

int ProcessTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_processes.size());
}

int ProcessTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProcessTableModel::data(const QModelIndex &index, int role) const
{
    const auto *process = processAt(index.row());
    if (!index.isValid() || !process || index.column() < 0 || index.column() >= ColumnCount)
        return {};
    if (role == PidRole) return QVariant::fromValue(process->pid);
    if (role == SearchRole) return process->searchText;
    if (role == Qt::TextAlignmentRole) {
        return int((index.column() <= Cpu && index.column() != User ? Qt::AlignRight : Qt::AlignLeft)
                   | Qt::AlignVCenter);
    }
    if (role == SortRole) {
        if (index.column() == Pid) return QVariant::fromValue(process->pid);
        if (index.column() == Cpu) return process->cpu;
        if (index.column() == Memory) return process->memory;
        return fieldText(*process, index.column());
    }
    if (role == FullTextRole) return fieldText(*process, index.column());
    if (role == Qt::DisplayRole) {
        QString text = fieldText(*process, index.column());
        if (text.isEmpty())
            return index.column() == ExecutablePath ? tr("不可读取") : QStringLiteral("—");
        // 行高固定，路径中的真实换行只在表格中可视化，复制/详情仍使用原文。
        return text.replace(QLatin1Char('\n'), QStringLiteral("\\n"))
                   .replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    }
    if (role == Qt::ToolTipRole) {
        QString text = fieldText(*process, index.column());
        if (index.column() == ExecutablePath && text.isEmpty())
            text = tr("无法读取 /proc/%1/exe：可能无权限、进程已退出、属于内核线程，或远端不支持路径查询。")
                       .arg(process->pid);
        if (text.size() > 2000) text = text.left(2000) + tr("\n…完整内容请在下方详情区查看或复制。");
        // 命令中的 <...> 是普通文本，不能被 tooltip 当成 HTML 标签吞掉。
        return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    }
    return {};
}

QVariant ProcessTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= ColumnCount) return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Pid: return QStringLiteral("PID");
        case User: return tr("用户");
        case Memory: return tr("内存 (%)");
        case Cpu: return QStringLiteral("CPU (%)");
        case Port: return tr("监听端口");
        case ExecutablePath: return tr("可执行文件路径");
        case Command: return tr("命令行");
        }
    }
    if (role == Qt::ToolTipRole) {
        if (section == ExecutablePath)
            return tr("来自 /proc/<pid>/exe 的实际可执行文件；脚本进程通常显示解释器路径。拖动列边界调整宽度。");
        if (section == Command) return tr("完整命令行由 ps auxww 获取；选择进程后在下方查看、选择和复制全文。");
        if (section == Cpu) return tr("ps 报告的 CPU 使用率，多线程进程可能超过 100%；点击按数值排序。");
        if (section == Memory) return tr("进程驻留内存占物理内存的百分比；点击按数值排序。");
        return tr("点击排序，拖动列边界调整宽度。");
    }
    return {};
}

const RemoteProcess *ProcessTableModel::processAt(int row) const
{
    return row >= 0 && row < m_processes.size() ? &m_processes.at(row) : nullptr;
}

int ProcessTableModel::rowForPid(quint64 pid) const { return m_rowsByPid.value(pid, -1); }

void ProcessTableModel::replaceSnapshot(QVector<RemoteProcess> processes)
{
    beginResetModel();
    m_processes = std::move(processes);
    m_rowsByPid.clear();
    m_rowsByPid.reserve(m_processes.size());
    m_unavailablePaths = 0;
    for (int row = 0; row < m_processes.size(); ++row) {
        m_rowsByPid.insert(m_processes.at(row).pid, row);
        if (m_processes.at(row).executablePath.isEmpty()) ++m_unavailablePaths;
    }
    endResetModel();
}

QString ProcessTableModel::collectionCommand()
{
    // -ww 禁止按终端宽度截断 args。路径用 NUL 分隔，支持空格、制表符、换行。
    // ss/find 可能因权限或缺少工具失败，仍保留可用的进程列表，不自动提权。
    // 只固定数字小数点，沿用字符集；不能用 LC_ALL=C 将中文参数变成问号。
    return QStringLiteral(
        "LC_CTYPE=\"${LC_ALL:-${LC_CTYPE:-${LANG:-C.UTF-8}}}\" LC_ALL= LC_NUMERIC=C "
        "ps auxww --no-headers && { "
        "printf '\\n===PS_SS_SEP===\\n'; ss -tulnpe 2>/dev/null || :; "
        "printf '\\n===PS_EXE_SEP===\\n'; "
        "find /proc -maxdepth 2 -name exe -printf '%h\\0%l\\0' 2>/dev/null || :; "
        "printf '\\0===PS_SNAPSHOT_END===\\0'; }");
}

ProcessSnapshot ProcessTableModel::parseSnapshot(const QString &output)
{
    ProcessSnapshot result;
    const int psEnd = output.indexOf(kPsSeparator);
    const int pathsStart = output.indexOf(kPathSeparator, psEnd < 0 ? 0 : psEnd + kPsSeparator.size());
    if (psEnd < 0 || pathsStart < 0 || !output.endsWith(kEnd)) {
        result.error = tr("返回数据不完整");
        return result;
    }
    const auto ports = parsePorts(output.mid(psEnd + kPsSeparator.size(),
                                            pathsStart - psEnd - kPsSeparator.size()));
    const int pathOffset = pathsStart + kPathSeparator.size();
    const QString pathText = output.mid(pathOffset, output.size() - kEnd.size() - pathOffset);
    QHash<quint64, QString> paths;
    if (!pathText.isEmpty()) {
        QStringList fields = pathText.split(QChar::Null);
        if (!fields.last().isEmpty() || fields.size() % 2 != 1) {
            result.error = tr("无法解析进程路径数据");
            return result;
        }
        static const QRegularExpression procDir(QStringLiteral("^/proc/([0-9]+)$"));
        for (int i = 0; i + 1 < fields.size(); i += 2) {
            const auto match = procDir.match(fields.at(i));
            if (match.hasMatch()) paths.insert(match.captured(1).toULongLong(), fields.at(i + 1));
        }
    }

    // 只解析前十个元数据字段，末尾命令行整体捕获；不能 simplified() 后再拼接。
    static const QRegularExpression psLine(QStringLiteral(
        "^[ \\t]*(\\S+)[ \\t]+([0-9]+)[ \\t]+(\\S+)[ \\t]+(\\S+)[ \\t]+"
        "(?:\\S+[ \\t]+){6}(.*)$"));
    const auto lines = output.left(psEnd).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    result.processes.reserve(lines.size());
    QHash<quint64, bool> seen;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) continue;
        const auto match = psLine.match(line);
        bool pidOk = false, cpuOk = false, memOk = false;
        RemoteProcess process;
        process.pid = match.captured(2).toULongLong(&pidOk);
        process.cpu = match.captured(3).toDouble(&cpuOk);
        process.memory = match.captured(4).toDouble(&memOk);
        if (!match.hasMatch() || !pidOk || !process.pid || seen.contains(process.pid)
            || !cpuOk || !memOk || !std::isfinite(process.cpu) || !std::isfinite(process.memory)
            || process.cpu < 0 || process.memory < 0) {
            result.processes.clear();
            result.error = tr("无法解析进程数据");
            return result;
        }
        seen.insert(process.pid, true);
        process.user = match.captured(1);
        process.cpuText = match.captured(3);
        process.memoryText = match.captured(4);
        process.command = match.captured(5);
        process.executablePath = paths.value(process.pid);
        process.port = ports.value(process.pid);
        process.searchText = QStringList{QString::number(process.pid), process.user, process.cpuText,
            process.memoryText, process.port, process.executablePath, process.command}.join(QLatin1Char('\n'));
        result.processes.append(std::move(process));
    }
    return result;
}

} // namespace cubeshell
