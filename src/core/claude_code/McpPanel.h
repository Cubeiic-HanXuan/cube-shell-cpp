#pragma once

// McpPanel.h — Claude Code MCP Tab：MCP Server 的可视化增删改查
//（用户全局 ~/.claude.json 或项目 <path>/.mcp.json）。
// 对应Python: core/claude_code/mcp_widget.py::McpWidget

#include <QJsonObject>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace cubeshell {

class ClaudeCodeBackend;

class McpPanel : public QWidget {
    Q_OBJECT
public:
    explicit McpPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);

public slots:
    // 对应Python: McpWidget.refresh（Tab 激活/模式切换时调用）
    void refresh();

private slots:
    void onScopeChanged();
    void onRefresh();
    void onAdd();
    void onEdit();
    void onDelete();
    void onMcpLoaded(const QJsonObject &config);
    void onMcpSaved(bool ok, const QString &message);

private:
    void buildUi();
    // 对应Python: _current_scope / _current_project_path
    QString currentScope() const;
    QString currentProjectPath() const;
    // 对应Python: _update_hint（底部配置文件路径说明）
    void updateHint();
    void loadMcpConfig();
    // 对应Python: _populate_table
    void populateTable();
    // 对应Python: _save_mcp_config
    void saveMcpConfig();

    ClaudeCodeBackend *m_backend = nullptr; // not owned（Panel 持有）
    QJsonObject m_mcpData;
    bool m_loadPending = false;
    bool m_savePending = false;

    QComboBox *m_scopeCombo = nullptr;
    QLineEdit *m_projectEdit = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_addBtn = nullptr;
    QPushButton *m_editBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_hintLabel = nullptr;
};

} // namespace cubeshell
