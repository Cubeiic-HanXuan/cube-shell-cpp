#pragma once

// HermesStatusWidget.h — Hermes deployment status tab.
// 对应Python: core/hermes/status_widget.py（StatusWidget + CommandWorker +
// PipInstallWorker）
//
// Shows hermes version / install state / gateway state / API server state as
// 2x2 status cards, with quick action buttons and an operation log. All CLI
// calls run on QtConcurrent worker threads; pip install/upgrade runs through
// QProcess. Results are posted back to the UI thread via queued invocation.

#include <QFuture>
#include <QList>
#include <QWidget>

class QLabel;
class QProcess;
class QPushButton;
class QTextEdit;

namespace cubeshell {

class HermesBackend;

class HermesStatusWidget : public QWidget {
    Q_OBJECT
public:
    explicit HermesStatusWidget(QWidget *parent = nullptr);
    ~HermesStatusWidget() override;

    // 设置后端引用（不拥有、不触发加载）对应Python: set_backend
    void setBackend(HermesBackend *backend);

    // Tab 被选中时调用，触发数据加载 对应Python: refresh
    void refresh();

    // 调用 CLI 获取各项状态信息 对应Python: refresh_status
    void refreshStatus();

private slots:
    void onStartGateway();      // 对应Python: _start_gateway
    void onStopGateway();       // 对应Python: _stop_gateway
    void onRunDoctor();         // 对应Python: _run_doctor
    void onUpdateHermes();      // 对应Python: _on_update
    void onInstallHermes();     // 对应Python: _on_install
    void onPipFinished(int exitCode);

private:
    struct StatusCard {
        QLabel *title = nullptr;
        QLabel *value = nullptr;
    };

    void buildUi();
    StatusCard createCard(const QString &titleText, const QString &valueText,
                          QWidget **frameOut);
    void setCardStatus(const StatusCard &card, const QString &text,
                       const QString &color);
    void setButtonsEnabled(bool enabled);
    void updateInstallButtonVisibility(bool installed);
    void appendLog(const QString &description, const QString &output);

    // 后台执行 hermes CLI，完成后在 UI 线程回调 handler(description, output)。
    // 对应Python: _run_command + CommandWorker
    void runCommand(const QStringList &args, const QString &description,
                    const char *handler);

    // pip install [--upgrade] hermes-agent 对应Python: PipInstallWorker
    void startPip(bool upgrade);

    // 结果解析回调（在 UI 线程执行）
    Q_INVOKABLE void handleVersionResult(const QString &description,
                                         const QString &output);
    Q_INVOKABLE void handleStatusResult(const QString &description,
                                        const QString &output);
    Q_INVOKABLE void handleCommandDone(const QString &description,
                                       const QString &output);

    HermesBackend *m_backend = nullptr; // not owned
    QList<QFuture<void>> m_futures;

    StatusCard m_cardVersion;
    StatusCard m_cardInstall;
    StatusCard m_cardGateway;
    StatusCard m_cardApi;

    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_startGwBtn = nullptr;
    QPushButton *m_stopGwBtn = nullptr;
    QPushButton *m_doctorBtn = nullptr;
    QPushButton *m_updateBtn = nullptr;
    QPushButton *m_installBtn = nullptr;
    QTextEdit *m_logOutput = nullptr;

    QProcess *m_pipProcess = nullptr;
    bool m_pipUpgrade = true;
};

} // namespace cubeshell
