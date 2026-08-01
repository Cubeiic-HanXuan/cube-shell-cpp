#pragma once

// ClaudeCodePanel.h — Claude Code 集成管理面板：主面板容器，
// 提供连接模式切换（本地 / 远程 SSH）和四个功能 Tab（状态/会话/设置/MCP）。
// 对应Python: core/claude_code/claude_code_panel.py::ClaudeCodePanel

#include <QList>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QShowEvent;
class QTabWidget;

namespace cubeshell {

class ClaudeCodeBackend;
class CommandExecutor;
class McpPanel;
class SessionPanel;
class SettingsPanel;
class StatusPanel;

class ClaudeCodePanel : public QWidget {
    Q_OBJECT
public:
    explicit ClaudeCodePanel(QWidget *parent = nullptr);

    ClaudeCodeBackend *backend() const { return m_backend; }

    // 注册一个活跃 SSH 连接到连接模式下拉框。
    // 对应Python: _refresh_connections（从 main_dialog.ssh_clients 取；
    // C++ 由 MainWindow 在打开面板时喂入，QPointer 防 tab 关闭后悬挂）
    void addRemoteConnection(const QString &hostname,
                             CommandExecutor *executor);

public slots:
    // 刷新当前可见 Tab（懒加载入口）
    void refresh();

signals:
    // 对应Python: open_terminal_requested（在新本机终端执行 claude 命令）
    void openTerminalRequested(const QString &command);

protected:
    // 对应Python: showEvent（面板首次显示时初始化 backend 并加载第一个 Tab）
    void showEvent(QShowEvent *event) override;

private slots:
    // 对应Python: _on_mode_changed（切换 backend 执行路径，刷新当前 Tab）
    void onModeChanged(int index);
    // 对应Python: _on_tab_changed（Tab 切换时懒加载数据）
    void onTabChanged(int index);

private:
    void buildUi();
    // 对应Python: _setup_cursors（按钮/Tab/下拉框为手指光标）
    void setupCursors();

    ClaudeCodeBackend *m_backend = nullptr; // owned (child QObject)
    bool m_initialized = false;             // 对应Python: _initialized
    // 远程连接列表；combo itemData 为该列表索引，-1 表示本地
    QList<QPointer<CommandExecutor>> m_executors;

    QComboBox *m_modeCombo = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    StatusPanel *m_statusPanel = nullptr;
    SessionPanel *m_sessionPanel = nullptr;
    SettingsPanel *m_settingsPanel = nullptr;
    McpPanel *m_mcpPanel = nullptr;
};

} // namespace cubeshell
