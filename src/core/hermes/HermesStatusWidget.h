#pragma once

// HermesStatusWidget.h — Hermes deployment status tab.
// 对应Python: core/hermes/status_widget.py（StatusWidget + CommandWorker +
// PipInstallWorker）
//
// Shows hermes version / install state / gateway state / API server state as
// 2x2 status cards, with quick action buttons and an operation log. All CLI
// calls run on QtConcurrent worker threads; update/install run through an
// async QProcess with live output streaming. Results are posted back to the
// UI thread via queued invocation.
//
// NOTE: Hermes Agent 不发布到 PyPI(`pip install hermes-agent` 永远失败)。
// 它以 git 检出的形式安装在 ~/.hermes/hermes-agent,官方升级入口是
// `hermes update`,官方安装入口是 install.sh / install.ps1 一键脚本。

#include <QFuture>
#include <QList>
#include <QProcess>
#include <QWidget>

class QLabel;
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
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    // 长任务类型——决定完成/失败日志的措辞
    enum class TaskKind { Update, Install };

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
    // 带时间戳的单行日志
    void appendStamped(const QString &text);
    // 进程输出：按行切分后写入，保留未完成的尾部
    void drainProcessOutput(bool flush);

    // 后台执行 hermes CLI，完成后在 UI 线程回调 handler(description, output)。
    // 对应Python: _run_command + CommandWorker
    void runCommand(const QStringList &args, const QString &description,
                    const char *handler, int timeoutMs = -1);

    // 启动一个流式输出的长任务进程（更新/安装）。已有任务在跑时直接返回。
    void startTask(TaskKind kind, const QString &program,
                   const QStringList &args, const QString &displayCommand);

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

    QProcess *m_taskProcess = nullptr;
    TaskKind m_taskKind = TaskKind::Update;
    QString m_taskBuffer;   // 未以换行结束的尾部输出
    bool m_taskFailed = false; // errorOccurred 已经报过错，避免 finished 再报一次
};

} // namespace cubeshell
