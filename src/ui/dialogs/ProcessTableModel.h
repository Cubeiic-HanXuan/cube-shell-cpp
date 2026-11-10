#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QString>
#include <QVector>

namespace cubeshell {

// 一次远端快照中的原始值。命令行和可执行文件路径完整保存，省略仅发生在绘制层。
struct RemoteProcess {
    quint64 pid = 0;
    QString user;
    QString cpuText;
    QString memoryText;
    double cpu = 0;
    double memory = 0;
    QString port;
    QString command;
    QString executablePath; // /proc/<pid>/exe；不能用 argv[0] 代替
    QString searchText;     // 预先组合搜索字段，筛选时不重复生成字符串
};

struct ProcessSnapshot {
    QVector<RemoteProcess> processes;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// 只为可见单元格提供数据，不为每个字段创建 QTableWidgetItem。
// PID/CPU/内存的排序角色返回数值，避免出现 100 排在 9 前的字符串排序。
class ProcessTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Pid, User, Memory, Cpu, Port, ExecutablePath, Command, ColumnCount };
    enum Role { SortRole = Qt::UserRole + 1, SearchRole, FullTextRole, PidRole };

    explicit ProcessTableModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    const RemoteProcess *processAt(int row) const;
    int rowForPid(quint64 pid) const;
    void replaceSnapshot(QVector<RemoteProcess> processes);
    int unavailablePathCount() const { return m_unavailablePaths; }

    // 单个 exec 请求批量取得 ps、ss、/proc 符号链接。GNU find 直接 readlink，
    // 不为每个 PID 启动一个 readlink 子进程，也不依赖远端 Python。
    static QString collectionCommand();
    static ProcessSnapshot parseSnapshot(const QString &output);

private:
    QVector<RemoteProcess> m_processes;
    QHash<quint64, int> m_rowsByPid;
    int m_unavailablePaths = 0;
};

} // namespace cubeshell
