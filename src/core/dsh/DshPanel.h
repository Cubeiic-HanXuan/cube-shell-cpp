#pragma once

// DshPanel.h — DeepSeek Harness 管理面板（多 Tab 容器）。
//
// 作为一个标签页打开（与 ClaudeCodePanel 同款，见 MainWindow::showDshPanel）。
// 自包含：面板持有自己的 DshManager，各子页共用它。
//
// Tab 组成（与 Claude Code 面板的多页形态对齐，但按 dsh 的真实能力取舍）：
//   * 状态   —— 运行状态/版本/启停/Web 与 CLI 入口/运行日志
//   * 插件   —— profile 的插件安装与卸载（dsh 的核心概念是「一切皆插件」）
//   * 会话   —— <DSH_HOME>/sessions 历史会话，可在终端 --resume 或删除
//   * 设置   —— <DSH_HOME>/settings.yaml 摘要与全文编辑
// 不设 MCP 页：dsh 官方无内置 MCP（不存在 @deepseek-ai/dsh-mcp），MCP 由第三方
// 插件（如 dsh-mcp-adapter）提供，装它就是在「插件」页装一个插件。
//
// Tab 切换时才刷新当前页（懒加载），与 ClaudeCodePanel::onTabChanged 一致。
//
// dsh web 以浏览器界面为交互主体，本面板只做生命周期与配置管理；「打开 Web 界面」
// 用 QDesktopServices::openUrl 调系统浏览器（不内嵌 WebEngine，见设计说明）。

#include <QWidget>

#include "dsh/DshManager.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace cubeshell {

class DshPluginPanel;
class DshSessionPanel;
class DshSettingsPanel;

class DshPanel : public QWidget {
    Q_OBJECT
public:
    explicit DshPanel(QWidget *parent = nullptr);

signals:
    // 请求在本机终端运行 dsh CLI（命令由面板按 模式/任务 构造，或由会话页构造
    // 的 --resume 命令）。主窗口接到后新开一个本机 PTY 终端并发送该命令。
    // workingDir：终端的启动目录，空=用默认目录。恢复会话时给会话原本的工作
    // 目录，让 agent 的文件操作落在对的项目上（与能否找到会话无关）。
    void openCliRequested(const QString &command, const QString &workingDir);

private slots:
    void onStartClicked();
    void onStopClicked();
    void onRestartClicked();
    void onOpenWebClicked();
    void onInstallGlobalClicked();
    void onUpdateClicked();
    void onCheckUpdateClicked();
    void onRefreshEnvClicked();
    void onClearLogClicked();
    void onOpenCliClicked();
    void onCliModeChanged(int index);
    void onTabChanged(int index);

    // --- DshManager 信号 ---
    void onStatusChanged(cubeshell::DshManager::Status status);
    void onStarted(qint64 pid);
    void onStopped(int exitCode);
    void onWebReady(const QString &url);
    void onLogLine(const QString &line);
    void onError(const QString &message);
    void onInstallLog(const QString &line);
    void onInstallFinished(bool ok, const QString &message);
    void onLatestVersionChecked(bool ok, const QString &latestVersion);
    void onEnvironmentDetected(const cubeshell::DshManager::Environment &env);

private:
    void buildUi();
    QWidget *buildStatusTab();   // 「状态」页内容（原单页面板的全部内容）
    // 按钮 / Tab 栏 / 下拉框设为手指光标（与 ClaudeCodePanel::setupCursors 同源）。
    // 在各子页构建完成后调用一次，findChildren 会递归覆盖所有 Tab 内的控件。
    void setupCursors();
    void loadSettings();
    void saveSettings();
    // 发起异步环境检测（不阻塞 UI）；结果到达走 onEnvironmentDetected。
    void refreshEnvironment();
    void updateStatusUi();       // 按当前 status 刷新状态灯/按钮可用性
    void refreshVersionUi();     // 按 已装/最新 版本刷新版本行与安装/更新按钮
    QString buildCliCommand() const; // 按 CLI 模式/任务构造终端命令
    void appendLog(const QString &text);

    DshManager *m_manager = nullptr; // owned（作为子对象）
    bool m_envDetecting = false;     // 环境检测在途（此时 m_envOk 尚不可信）
    bool m_envOk = false;            // 最近一次环境检测：npx 是否可用
    bool m_dshInstalled = false;     // 是否已全局安装 dsh
    QString m_dshVersion;            // 已装 dsh 版本
    QString m_latestVersion;         // 查询到的 latest 版本（空=未查询/查询失败）
    QString m_lastCliDir;            // 上次「终端运行 CLI」选的项目文件夹（持久化）

    // Tab 容器与各子页
    QTabWidget *m_tabWidget = nullptr;
    DshPluginPanel *m_pluginPanel = nullptr;
    DshSessionPanel *m_sessionPanel = nullptr;
    DshSettingsPanel *m_settingsPanel = nullptr;

    // 运行状态区
    QLabel *m_statusDot = nullptr;
    QLabel *m_statusText = nullptr;
    QLabel *m_pidLabel = nullptr;
    QLabel *m_urlLabel = nullptr;
    QLabel *m_envLabel = nullptr;
    QLabel *m_versionLabel = nullptr; // 已装/最新版本
    // 监听配置区
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    // CLI 区（与监听配置同一分组）
    QComboBox *m_cliModeCombo = nullptr;
    QLineEdit *m_cliTaskEdit = nullptr;
    QPushButton *m_openCliBtn = nullptr;
    // 操作按钮
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_restartBtn = nullptr;
    QPushButton *m_openWebBtn = nullptr;
    QPushButton *m_installBtn = nullptr;
    QPushButton *m_updateBtn = nullptr;
    QPushButton *m_checkUpdateBtn = nullptr;
    QPushButton *m_refreshEnvBtn = nullptr;
    // 日志
    QPlainTextEdit *m_logView = nullptr;
    QPushButton *m_clearLogBtn = nullptr;
};

} // namespace cubeshell
