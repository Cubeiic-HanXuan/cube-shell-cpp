#pragma once

// ProcessManagerDialog.h — 远程进程管理对话框。
// 对应Python: cube-shell.py::_ensure_process_manager_dialog / showProcessManagerDialog
//           + processInitUI / update_process_list / apply_filter / kill_selected_process
//
// 数据来源为 CommandExecutor 的流式执行：`ps aux --no-headers` 拉取进程列表，
// `kill -15 <pid...>` 终止进程。streamFinished()/streamError() 来自工作线程 ——
// 以 Qt::QueuedConnection 接入；executor 可能被其它组件复用，通过 m_task
// 标记只消费本对话框发起的那次执行。

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QTableWidget;

namespace cubeshell {

class CommandExecutor;

class ProcessManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProcessManagerDialog(CommandExecutor *executor = nullptr,
                                  QWidget *parent = nullptr);

    // 连接切换时替换 executor（nullptr = 未连接）。executor 不被持有。
    void setExecutor(CommandExecutor *executor);

public slots:
    // 对应Python: cube-shell.py::update_process_list
    void refresh();

private slots:
    void onKillClicked();
    void onFilterChanged(const QString &text);
    void onStreamFinished(int exitCode, const QString &stdoutText, const QString &stderrText);
    void onStreamError(const QString &message);
    // 对应Python: cube-shell.py::showContextMenu
    void onTableContextMenu(const QPoint &pos);

private:
    // ps aux 一行解析后的进程信息。
    // 对应Python: cube-shell.py::get_filtered_process_list 的 process dict
    struct RemoteProcess {
        QString pid;
        QString user;
        QString cpu;
        QString memory;
        QString port;
        QString command;
    };

    // 本对话框正在等待的流式命令（None = 空闲）。
    enum class Task { None, Refresh, Kill };

    // 对应Python: get_filtered_process_list 的 ss -tulnpe 解析（pid -> 端口列表）
    QHash<QString, QStringList> parseSsOutput(const QString &ssOutput);
    void parseProcessList(const QString &psOutput,
                          const QHash<QString, QStringList> &pidPorts = {});
    void applyFilter();

    CommandExecutor *m_executor; // not owned
    Task m_task = Task::None;
    QList<RemoteProcess> m_all;

    QLineEdit *m_search = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace cubeshell
