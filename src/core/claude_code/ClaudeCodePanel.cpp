// ClaudeCodePanel.cpp — see ClaudeCodePanel.h for the port map.
// 对应Python: core/claude_code/claude_code_panel.py

#include "claude_code/ClaudeCodePanel.h"

#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/McpPanel.h"
#include "claude_code/SessionPanel.h"
#include "claude_code/SettingsPanel.h"
#include "claude_code/StatusPanel.h"
// QPointer<CommandExecutor> 需要完整类型
#include "ssh/CommandExecutor.h"

#include <QComboBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace cubeshell {

ClaudeCodePanel::ClaudeCodePanel(QWidget *parent)
    : QWidget(parent)
{
    // Python 每次切换模式重建 LocalBackend/RemoteBackend；C++ 用单实例
    // ClaudeCodeBackend + setRemoteExecutor 切换执行路径（子面板共享引用，
    // 等价于 Python 的 _notify_backend_changed）。
    m_backend = new ClaudeCodeBackend(this);
    buildUi();
}

// 对应Python: ClaudeCodePanel._init_ui（行 31-64）
void ClaudeCodePanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    // 顶部：连接模式选择
    auto *topLayout = new QHBoxLayout;
    topLayout->addWidget(new QLabel(tr("连接模式："), this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modeCombo->setMinimumWidth(200);
    m_modeCombo->addItem(tr("本地"), -1); // itemData=-1 表示本地
    // 远程连接项由 MainWindow 通过 addRemoteConnection 动态添加
    topLayout->addWidget(m_modeCombo);
    topLayout->addStretch();
    layout->addLayout(topLayout);

    // 主体：QTabWidget 切换各功能模块
    m_tabWidget = new QTabWidget(this);
    layout->addWidget(m_tabWidget);

    // 对应Python: _add_tabs（行 66-104）
    m_statusPanel = new StatusPanel(m_backend, this);
    connect(m_statusPanel, &StatusPanel::openTerminalRequested,
            this, &ClaudeCodePanel::openTerminalRequested);
    m_tabWidget->addTab(m_statusPanel, tr("状态"));

    m_sessionPanel = new SessionPanel(m_backend, this);
    connect(m_sessionPanel, &SessionPanel::openTerminalRequested,
            this, &ClaudeCodePanel::openTerminalRequested);
    m_tabWidget->addTab(m_sessionPanel, tr("会话"));

    m_settingsPanel = new SettingsPanel(m_backend, this);
    m_tabWidget->addTab(m_settingsPanel, tr("设置"));

    m_mcpPanel = new McpPanel(m_backend, this);
    m_tabWidget->addTab(m_mcpPanel, tr("MCP"));

    // 连接 Tab 切换信号 — 切换时懒加载当前 Tab 数据
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, &ClaudeCodePanel::onTabChanged);

    // 连接模式切换信号 — 用户手动切换时才触发
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, &ClaudeCodePanel::onModeChanged);

    // 设置鼠标样式：默认箭头，按钮/Tab/下拉框为手指
    setupCursors();
}

// 对应Python: _refresh_connections（行 106-115，"远程: {hostname}" 文案）
void ClaudeCodePanel::addRemoteConnection(const QString &hostname,
                                          CommandExecutor *executor)
{
    if (!executor)
        return;
    m_executors.append(QPointer<CommandExecutor>(executor));
    const int executorIndex = m_executors.size() - 1;
    m_modeCombo->addItem(tr("远程: %1").arg(hostname), executorIndex);
    // SSH 会话关闭时移除残留下拉条目；若移除的是当前选中项，
    // currentIndexChanged 会自动触发 onModeChanged 回退（此时 QPointer
    // 已置空，backend 安全切回本地）。m_executors 条目保留占位，
    // 保证其余 itemData 索引不变。
    connect(executor, &QObject::destroyed, this, [this, executorIndex]() {
        const int idx = m_modeCombo->findData(executorIndex);
        if (idx >= 0)
            m_modeCombo->removeItem(idx);
    });
}

// 对应Python: showEvent（行 117-122）
void ClaudeCodePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_initialized) {
        m_initialized = true;
        onModeChanged(m_modeCombo->currentIndex());
    }
}

// 对应Python: _on_mode_changed（行 124-147）
void ClaudeCodePanel::onModeChanged(int index)
{
    if (index < 0)
        return;
    const int executorIndex = m_modeCombo->itemData(index).toInt();
    if (executorIndex < 0) {
        m_backend->setRemoteExecutor(nullptr); // LocalBackend 行为
    } else {
        CommandExecutor *executor =
            (executorIndex < m_executors.size())
                ? m_executors.at(executorIndex).data()
                : nullptr;
        if (executor) {
            m_backend->setRemoteExecutor(executor); // RemoteBackend 行为
        } else {
            // 对应Python: 找不到 SSH 连接时回退到本地模式
            qWarning() << "ClaudeCodePanel: SSH connection gone, "
                          "falling back to local mode";
            m_backend->setRemoteExecutor(nullptr);
        }
    }
    // 只刷新当前可见的 Tab（子面板共享同一 backend 实例）
    refresh();
}

// 对应Python: _on_tab_changed（行 149-153）
void ClaudeCodePanel::onTabChanged(int index)
{
    if (!m_initialized)
        return;
    QWidget *widget = m_tabWidget->widget(index);
    if (widget == m_statusPanel)
        m_statusPanel->refresh();
    else if (widget == m_sessionPanel)
        m_sessionPanel->refresh();
    else if (widget == m_settingsPanel)
        m_settingsPanel->refresh();
    else if (widget == m_mcpPanel)
        m_mcpPanel->refresh();
}

// 刷新当前可见 Tab（对应Python: _on_mode_changed 尾部的 current refresh）
void ClaudeCodePanel::refresh()
{
    onTabChanged(m_tabWidget->currentIndex());
}

// 对应Python: _setup_cursors（行 171-180）
void ClaudeCodePanel::setupCursors()
{
    setCursor(Qt::ArrowCursor);
    // Tab 栏设置手指光标
    m_tabWidget->tabBar()->setCursor(Qt::PointingHandCursor);
    // 下拉框设置手指光标
    m_modeCombo->setCursor(Qt::PointingHandCursor);
    // 递归设置所有 QPushButton 为手指光标
    const QList<QPushButton *> buttons = findChildren<QPushButton *>();
    for (QPushButton *btn : buttons)
        btn->setCursor(Qt::PointingHandCursor);
}

} // namespace cubeshell
