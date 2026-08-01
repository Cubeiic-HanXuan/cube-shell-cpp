// McpPanel.cpp — see McpPanel.h for the port map.
// 对应Python: core/claude_code/mcp_widget.py::McpWidget

#include "claude_code/McpPanel.h"

#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/McpEditDialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

McpPanel::McpPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
{
    buildUi();
    // backend 信号来自工作线程，必须 QueuedConnection 切回 UI 线程
    connect(m_backend, &ClaudeCodeBackend::mcpConfigLoaded,
            this, &McpPanel::onMcpLoaded, Qt::QueuedConnection);
    connect(m_backend, &ClaudeCodeBackend::mcpConfigSaved,
            this, &McpPanel::onMcpSaved, Qt::QueuedConnection);
}

// 对应Python: McpWidget._init_ui（行 162-234）
void McpPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // --- 顶部工具栏 ---
    auto *toolbar = new QHBoxLayout;

    // 作用域选择：用户全局 / 当前项目
    toolbar->addWidget(new QLabel(tr("作用域:"), this));
    m_scopeCombo = new QComboBox(this);
    m_scopeCombo->addItem(tr("用户全局"), QStringLiteral("user"));
    m_scopeCombo->addItem(tr("项目"), QStringLiteral("project"));
    connect(m_scopeCombo, &QComboBox::currentIndexChanged,
            this, &McpPanel::onScopeChanged);
    toolbar->addWidget(m_scopeCombo);

    // 项目路径（仅项目作用域可见）
    m_projectEdit = new QLineEdit(this);
    m_projectEdit->setPlaceholderText(tr("项目路径，写入 <路径>/.mcp.json"));
    m_projectEdit->setMinimumWidth(220);
    connect(m_projectEdit, &QLineEdit::editingFinished,
            this, &McpPanel::onRefresh);
    m_projectEdit->setVisible(false);
    toolbar->addWidget(m_projectEdit);

    m_refreshBtn = new QPushButton(tr("刷新"), this);
    connect(m_refreshBtn, &QPushButton::clicked, this, &McpPanel::onRefresh);
    toolbar->addWidget(m_refreshBtn);

    m_addBtn = new QPushButton(tr("添加"), this);
    connect(m_addBtn, &QPushButton::clicked, this, &McpPanel::onAdd);
    toolbar->addWidget(m_addBtn);

    m_editBtn = new QPushButton(tr("编辑"), this);
    connect(m_editBtn, &QPushButton::clicked, this, &McpPanel::onEdit);
    toolbar->addWidget(m_editBtn);

    m_deleteBtn = new QPushButton(tr("删除"), this);
    connect(m_deleteBtn, &QPushButton::clicked, this, &McpPanel::onDelete);
    toolbar->addWidget(m_deleteBtn);

    toolbar->addStretch();
    layout->addLayout(toolbar);

    // --- MCP Server 表格 ---
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
        {tr("名称"), tr("Command"), tr("Args"),
         tr("状态")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setSelectionMode(QTableWidget::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_table);

    // --- 底部说明 ---
    m_hintLabel = new QLabel(this);
    m_hintLabel->setStyleSheet(
        QStringLiteral("color: gray; font-size: 11px;"));
    layout->addWidget(m_hintLabel);
    updateHint();
}

// 对应Python: _current_scope（行 236-238）
QString McpPanel::currentScope() const
{
    const QString scope = m_scopeCombo->currentData().toString();
    return scope.isEmpty() ? QStringLiteral("user") : scope;
}

// 对应Python: _current_project_path（行 240-244）
QString McpPanel::currentProjectPath() const
{
    if (currentScope() != QLatin1String("project"))
        return QString();
    return m_projectEdit->text().trimmed();
}

// 对应Python: _update_hint（行 246-253）
void McpPanel::updateHint()
{
    QString text;
    if (currentScope() == QLatin1String("project")) {
        QString path = currentProjectPath();
        if (path.isEmpty())
            path = tr("<项目路径>");
        text = tr("MCP 配置文件: %1/.mcp.json  |  选中行后可编辑或删除")
                   .arg(path);
    } else {
        text = tr("MCP 配置文件: ~/.claude.json (mcpServers)  |  "
                  "选中行后可编辑或删除");
    }
    m_hintLabel->setText(text);
}

// 对应Python: _on_scope_changed（行 255-259）
void McpPanel::onScopeChanged()
{
    m_projectEdit->setVisible(currentScope() == QLatin1String("project"));
    updateHint();
    loadMcpConfig();
}

// 对应Python: refresh（行 265-267）
void McpPanel::refresh()
{
    loadMcpConfig();
}

// 对应Python: _load_mcp_config（行 269-283）
void McpPanel::loadMcpConfig()
{
    if (!m_backend || m_loadPending)
        return;
    m_loadPending = true;
    m_refreshBtn->setEnabled(false);
    m_backend->refreshMcpConfig(currentScope(), currentProjectPath());
}

// 对应Python: _on_mcp_loaded（行 285-291）
void McpPanel::onMcpLoaded(const QJsonObject &config)
{
    m_loadPending = false;
    m_refreshBtn->setEnabled(true);
    m_mcpData = config;
    populateTable();
}

// 对应Python: _populate_table（行 293-319）
void McpPanel::populateTable()
{
    m_table->setRowCount(0);
    const QJsonValue serversValue =
        m_mcpData.value(QStringLiteral("mcpServers"));
    if (!serversValue.isObject())
        return;
    const QJsonObject servers = serversValue.toObject();

    m_table->setRowCount(servers.size());
    int row = 0;
    for (auto it = servers.begin(); it != servers.end(); ++it, ++row) {
        const QJsonObject cfg = it.value().toObject();

        // 名称
        m_table->setItem(row, 0, new QTableWidgetItem(it.key()));

        // Command
        const QString command = cfg.value(QStringLiteral("command")).toString();
        m_table->setItem(row, 1, new QTableWidgetItem(command));

        // Args
        QStringList argsParts;
        const QJsonValue argsValue = cfg.value(QStringLiteral("args"));
        if (argsValue.isArray()) {
            const QJsonArray arr = argsValue.toArray();
            for (const QJsonValue &v : arr)
                argsParts.append(v.isString() ? v.toString()
                                              : v.toVariant().toString());
        }
        m_table->setItem(
            row, 2, new QTableWidgetItem(argsParts.join(QStringLiteral(", "))));

        // 状态
        const QString status = command.isEmpty() ? tr("未知") : tr("已配置");
        auto *statusItem = new QTableWidgetItem(status);
        statusItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 3, statusItem);
    }
}

// 对应Python: _on_refresh（行 329-332）
void McpPanel::onRefresh()
{
    updateHint();
    loadMcpConfig();
}

// 对应Python: _on_add（行 334-345）
void McpPanel::onAdd()
{
    McpEditDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString name = dialog.serverName();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("警告"), tr("Server 名称不能为空"));
        return;
    }
    QJsonObject servers = m_mcpData.value(QStringLiteral("mcpServers")).toObject();
    servers.insert(name, dialog.serverConfig());
    m_mcpData.insert(QStringLiteral("mcpServers"), servers);
    saveMcpConfig();
}

// 对应Python: _on_edit（行 347-366）
void McpPanel::onEdit()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        QMessageBox::information(this, tr("提示"), tr("请先选中一行"));
        return;
    }
    QTableWidgetItem *nameItem = m_table->item(row, 0);
    if (!nameItem)
        return;
    const QString serverName = nameItem->text();
    QJsonObject servers = m_mcpData.value(QStringLiteral("mcpServers")).toObject();
    const QJsonObject serverConfig = servers.value(serverName).toObject();

    McpEditDialog dialog(this, serverName, serverConfig);
    dialog.setNameReadOnly(true); // 编辑模式下锁定名称
    if (dialog.exec() != QDialog::Accepted)
        return;
    servers.insert(serverName, dialog.serverConfig());
    m_mcpData.insert(QStringLiteral("mcpServers"), servers);
    saveMcpConfig();
}

// 对应Python: _on_delete（行 368-389）
void McpPanel::onDelete()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        QMessageBox::information(this, tr("提示"), tr("请先选中一行"));
        return;
    }
    QTableWidgetItem *nameItem = m_table->item(row, 0);
    if (!nameItem)
        return;
    const QString serverName = nameItem->text();

    const auto reply = QMessageBox::question(
        this, tr("确认删除"),
        tr("确定要删除 MCP Server \"%1\" 吗？").arg(serverName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;
    QJsonObject servers = m_mcpData.value(QStringLiteral("mcpServers")).toObject();
    servers.remove(serverName);
    m_mcpData.insert(QStringLiteral("mcpServers"), servers);
    saveMcpConfig();
}

// 对应Python: _save_mcp_config（行 391-408）
void McpPanel::saveMcpConfig()
{
    if (!m_backend) {
        QMessageBox::warning(this, tr("警告"), tr("未连接后端"));
        return;
    }
    if (currentScope() == QLatin1String("project")
        && currentProjectPath().isEmpty()) {
        QMessageBox::warning(this, tr("警告"), tr("请先填写项目路径"));
        return;
    }
    if (m_savePending)
        return;
    m_savePending = true;
    m_backend->saveMcpConfig(m_mcpData, currentScope(), currentProjectPath());
}

// 对应Python: _on_mcp_saved（行 410-419）
void McpPanel::onMcpSaved(bool ok, const QString &message)
{
    m_savePending = false;
    if (ok) {
        // 刷新表格显示
        populateTable();
    } else {
        QMessageBox::critical(this, tr("错误"),
                              tr("保存 MCP 配置失败: %1").arg(message));
    }
}

} // namespace cubeshell
