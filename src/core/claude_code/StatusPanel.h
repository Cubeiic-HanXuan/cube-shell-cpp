#pragma once

// StatusPanel.h — Claude Code 状态 Tab：版本/认证/守护进程/安装路径卡片
// + 快速操作按钮 + 日志输出区。
// 对应Python: core/claude_code/status_widget.py::StatusWidget

#include <QWidget>

class QFrame;
class QJsonObject;
class QLabel;
class QPushButton;
class QTextEdit;

namespace cubeshell {

class ClaudeCodeBackend;

class StatusPanel : public QWidget {
    Q_OBJECT
public:
    explicit StatusPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);

public slots:
    // 对应Python: StatusWidget.refresh（Tab 激活/模式切换时调用）
    void refresh();

signals:
    // 对应Python: StatusWidget.open_terminal_requested
    void openTerminalRequested(const QString &command);

private slots:
    void onOpenTerminal();
    void onAgentView();
    void onUpdate();
    void onInstall();
    void onStatusLoaded(const QString &version, const QJsonObject &auth,
                        const QJsonObject &daemon, const QString &binPath);
    void onUpdateFinished(const QString &output);

private:
    // 对应Python: _create_card（QFrame + 粗体标题 + 值标签）
    struct Card {
        QFrame *frame = nullptr;
        QLabel *title = nullptr;
        QLabel *value = nullptr;
    };

    void buildUi();
    Card createCard(const QString &titleText, const QString &valueText);
    // 对应Python: _set_card_status
    void setCardStatus(const Card &card, const QString &text,
                       const QString &color);
    // 对应Python: _open_in_selected_dir（QFileDialog 选目录 → cd && claude）
    void openInSelectedDir(const QString &claudeCmd, const QString &caption);
    void setButtonsEnabled(bool enabled);
    // 对应Python: _update_install_button_visibility
    void updateInstallButtonVisibility(bool installed);
    void logAppend(const QString &message);

    ClaudeCodeBackend *m_backend = nullptr; // not owned（Panel 持有）
    QString m_lastDir;                      // 记住上次选择的项目文件夹
    bool m_statusPending = false;           // 对应Python: worker.isRunning 防重入
    bool m_updatePending = false;

    Card m_cardVersion;
    Card m_cardAuth;
    Card m_cardDaemon;
    Card m_cardPath;

    QPushButton *m_btnRefresh = nullptr;
    QPushButton *m_btnOpenTerminal = nullptr;
    QPushButton *m_btnAgentView = nullptr;
    QPushButton *m_btnUpdate = nullptr;
    QPushButton *m_btnInstall = nullptr;
    QTextEdit *m_logOutput = nullptr;
};

} // namespace cubeshell
