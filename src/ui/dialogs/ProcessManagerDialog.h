#pragma once

#include <QDateTime>
#include <QDialog>
#include <QVector>
#include <memory>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class QToolButton;

namespace cubeshell {
class CommandExecutor;
class SshClient;
class ProcessTableModel;
struct RemoteProcess;
struct ProcessManagerDialogTestAccess;

// 对话框持有当前连接的执行器。表格使用模型/代理提供排序筛选，完整原文在详情区查看。
class ProcessManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProcessManagerDialog(QWidget *parent = nullptr);
    ~ProcessManagerDialog() override;
    void setClient(std::shared_ptr<SshClient> client);

public slots:
    void refresh();

protected:
    void changeEvent(QEvent *event) override;

private slots:
    void onKillClicked();
    void onFilterChanged(const QString &text);
    void onStreamFinished(int exitCode, const QString &stdoutText, const QString &stderrText);
    void onStreamError(const QString &message);
    void onTableContextMenu(const QPoint &pos);
    void updateDetails();
    void copyCommand();
    void copyPath();
    void copySelectedRows();

private:
    friend struct ProcessManagerDialogTestAccess;
    enum class Task { None, Refresh, Kill };
    struct ViewState {
        QVector<quint64> selected;
        quint64 current = 0;
        quint64 top = 0;
        int topOffset = 0;
        int vertical = 0;
        int horizontal = 0;
    };
    ViewState captureViewState() const;
    void restoreViewState(const ViewState &state);
    const RemoteProcess *currentProcess() const;
    void applyFilter();
    void updateStatus();
    void markStale(const QString &message);
    void releaseExecutor();

    CommandExecutor *m_executor = nullptr;
    quint64 m_generation = 0;
    quint64 m_snapshotRevision = 0; // 确认终止期间如果快照变化，不能继续使用旧选择
    Task m_task = Task::None;
    bool m_taskFailed = false;
    QDateTime m_lastUpdated;
    bool m_stale = true;
    QString m_notice;

    ProcessTableModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QTimer *m_filterTimer = nullptr;
    QLineEdit *m_search = nullptr;
    QTableView *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_killButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_detailsSummary = nullptr;
    QLineEdit *m_pathDetails = nullptr;
    QPlainTextEdit *m_commandDetails = nullptr;
    QToolButton *m_copyCommandButton = nullptr;
    QToolButton *m_copyPathButton = nullptr;
};
} // namespace cubeshell
