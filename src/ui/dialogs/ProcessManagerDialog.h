#pragma once

// 远程进程管理：对话框独占执行器，并以共享所有权保持其 SSH 连接存活。
// 更换实际连接时先撤销旧回调、取消并回收工作线程，再创建新执行器；
// 不能按标签页里的对象名复用重连前的执行器。

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace cubeshell {

class CommandExecutor;
class SshClient;
struct ProcessManagerDialogTestAccess;

class ProcessManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProcessManagerDialog(QWidget *parent = nullptr);
    ~ProcessManagerDialog() override;

    // 同一连接重复设置不打断任务；nullptr 表示断线，保留列表但明确标记为旧数据。
    // 换到新的非空连接则清空旧列表，避免跨服务器展示或终止旧 PID。
    void setClient(std::shared_ptr<SshClient> client);

public slots:
    void refresh();

private slots:
    void onKillClicked();
    void onFilterChanged(const QString &text);
    void onStreamFinished(int exitCode, const QString &stdoutText, const QString &stderrText);
    void onStreamError(const QString &message);
    void onTableContextMenu(const QPoint &pos);

private:
    friend struct ProcessManagerDialogTestAccess;
    struct RemoteProcess {
        QString pid;
        QString user;
        QString cpu;
        QString memory;
        QString port;
        QString command;
    };

    enum class Task { None, Refresh, Kill };
    QHash<QString, QStringList> parseSsOutput(const QString &ssOutput);
    // 全部解析成功才提交新快照，失败时不覆盖上次成功的列表。
    bool parseProcessList(const QString &psOutput,
                          const QHash<QString, QStringList> &pidPorts = {});
    void applyFilter();
    void updateStatus(); // 只更新单行提示和按钮状态，不改动表格或滚动位置
    void markStale(const QString &message);
    void releaseExecutor();

    CommandExecutor *m_executor = nullptr; // owned，先 join 再销毁
    quint64 m_generation = 0; // 隔离已进入 Qt 事件队列的旧连接回调
    Task m_task = Task::None;
    bool m_taskFailed = false; // streamError 后等待配对的 streamFinished，再允许下一次任务
    QList<RemoteProcess> m_all;
    QDateTime m_lastUpdated;
    bool m_stale = true;
    QString m_notice;

    QLineEdit *m_search = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_killButton = nullptr;
};

} // namespace cubeshell
