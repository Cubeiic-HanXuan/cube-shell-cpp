#pragma once

// McpEditDialog.h — MCP Server 编辑对话框（名称/Command/Args/Env）。
// 对应Python: core/claude_code/mcp_widget.py::McpEditDialog

#include <QDialog>
#include <QJsonObject>

class QLineEdit;
class QTextEdit;

namespace cubeshell {

class McpEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit McpEditDialog(QWidget *parent = nullptr,
                           const QString &serverName = QString(),
                           const QJsonObject &serverConfig = QJsonObject());

    // 对应Python: get_server_name
    QString serverName() const;
    // 对应Python: get_server_config（command/args/env 三字段，空则省略）
    QJsonObject serverConfig() const;
    // 对应Python: set_name_readonly（编辑模式下锁定名称）
    void setNameReadOnly(bool readOnly);

private:
    void buildUi(const QString &serverName, const QJsonObject &config);

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QLineEdit *m_argsEdit = nullptr;
    QTextEdit *m_envEdit = nullptr;
};

} // namespace cubeshell
