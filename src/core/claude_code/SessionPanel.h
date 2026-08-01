#pragma once

// SessionPanel.h — Claude Code 会话 Tab：按项目(cwd)分组的会话树，
// 双击会话在终端中恢复（claude --resume <id>）。
// 对应Python: core/claude_code/session_widget.py::SessionWidget

#include <QWidget>

#include "claude_code/ClaudeCodeBackend.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class SessionPanel : public QWidget {
    Q_OBJECT
public:
    explicit SessionPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);

public slots:
    // 对应Python: SessionWidget.refresh（Tab 激活/模式切换时调用）
    void refresh();

signals:
    // 对应Python: SessionWidget.open_terminal_requested
    void openTerminalRequested(const QString &command);

private slots:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onSessionsLoaded(const QList<cubeshell::ClaudeSessionInfo> &sessions);

private:
    void buildUi();
    // 对应Python: _render_tree（按 cwd 分组，保留展开状态）
    void renderTree();
    QTreeWidgetItem *makeGroupItem(const QString &cwd,
                                   const QList<ClaudeSessionInfo> &sessions);
    QTreeWidgetItem *makeSessionItem(const ClaudeSessionInfo &session, int index);
    // 对应Python: _expanded_projects
    QSet<QString> expandedProjects() const;
    // 对应Python: _resume_session（cd 到会话 cwd 再 --resume）
    void resumeSession(const ClaudeSessionInfo &session);
    void setStatusText(const QString &text);
    // 对应Python: 判定 running 态的状态集合（running/busy/idle/active）
    static bool isRunningStatus(const QString &status);

    ClaudeCodeBackend *m_backend = nullptr; // not owned（Panel 持有）
    QList<ClaudeSessionInfo> m_sessions;
    bool m_pending = false;

    QPushButton *m_btnRefresh = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_statusLabel = nullptr;
};

} // namespace cubeshell
