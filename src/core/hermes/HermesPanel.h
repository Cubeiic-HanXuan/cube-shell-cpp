#pragma once

// HermesPanel.h — Hermes Agent management panel.
// 对应Python: core/hermes/hermes_panel.py + cron_widget.py + gateway_widget.py
//             + skills_widget.py 的 UI 部分(合并为单面板,QSplitter 布局)
//
// 顶部:连接模式选择(本地/远程 executor)
// 上半部:QTabWidget(Agent 管理 / 部署状态 / 配置管理 / Memory 浏览
//                    / 定时任务 / Skills 管理 / 消息网关)
// 下半部:执行状态 + 操作日志
// Tab 切换时才刷新当前页(懒加载),与 Python 各 widget.refresh 一致。
// The Python multi-widget layout collapses into one panel per the porting
// contract; all worker signals are consumed with Qt::QueuedConnection.

#include <QList>
#include <QMap>
#include <QStringList>
#include <QWidget>

#include "hermes/HermesAgentWidget.h"
#include "hermes/HermesConfigWidget.h"
#include "hermes/HermesGateway.h"
#include "hermes/HermesMemoryWidget.h"
#include "hermes/HermesStatusWidget.h"
#include "hermes/HermesTaskModel.h"

class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTextEdit;

namespace cubeshell {

class CommandExecutor;
class HermesBackend;

class HermesPanel : public QWidget {
    Q_OBJECT
public:
    explicit HermesPanel(QWidget *parent = nullptr);

    HermesBackend *backend() const { return m_backend; }
    HermesTaskModel *taskModel() const { return m_tasks; }
    HermesGateway *gateway() const { return m_gateway; }

    // 主窗口把已建立的 SSH 会话喂进连接模式下拉框(与 ClaudeCodePanel 的
    // addRemoteConnection 同源;index 0 恒为"本地")。
    void setAvailableConnections(const QStringList &hosts,
                                 const QList<CommandExecutor *> &executors);

public slots:
    // 对应Python: 各 widget 的 refresh(Tab 激活时调用,只刷新当前可见页)
    void refresh();

signals:
    // 对应Python: AgentWidget.open_terminal_requested(转发给主窗口开终端)
    void openTerminalRequested(const QString &profileName);

private slots:
    // --- cron ---
    void onCreateJobClicked();
    void onDeleteJobClicked();
    void onPauseResumeClicked();
    void onRunNowClicked();
    void onJobRowChanged();
    void onJobsLoaded(const QList<cubeshell::HermesCronJob> &jobs);
    void onOutputLoaded(const QString &logContent);
    // --- gateway ---
    void onStartGatewayClicked();
    void onStopGatewayClicked();
    void onConfigLoaded(const QList<cubeshell::GatewayPlatformConfig> &configs);
    void onStatusChecked(bool isRunning);
    // 平台卡片上的交互 对应Python: gateway_widget.py 的 _toggle_platform /
    // _toggle_config_form / _save_platform_config / _test_platform
    void onPlatformToggled(const QString &platformId, bool enabled);
    void onToggleConfigForm(const QString &platformId);
    void onSavePlatformConfigClicked(const QString &platformId);
    void onTestPlatformClicked(const QString &platformId);
    // --- skills ---
    void onSkillsLoaded(const QList<cubeshell::HermesSkillInfo> &skills);
    void onSkillRowChanged(int row);
    void onInstallSkillClicked();
    void onRemoveSkillClicked();
    // --- shared ---
    void onCommandDone(const QString &description, const QString &output);
    void onErrorOccurred(const QString &message);
    // --- mode / lazy loading ---
    void onModeChanged(int index);
    void onTabChanged(int index);

private:
    void buildUi();
    const HermesCronJob *selectedJob() const;

    // --- gateway tab helpers --- 对应Python: gateway_widget.GatewayWidget
    // 一次性为 PLATFORMS 全表建卡(3 列网格) 对应Python: _build_platform_cards
    void buildPlatformCards(QGridLayout *grid, QWidget *parent);
    // 对应Python: _create_platform_card
    QFrame *createPlatformCard(const cubeshell::GatewayPlatform &platform,
                               QWidget *parent);
    // 对应Python: _update_cards_from_config
    void updateCardsFromConfig(
        const QList<cubeshell::GatewayPlatformConfig> &configs);
    // 对应Python: _set_status_pending
    void setGatewayStatusPending(const QString &text);
    // 对应Python: _append_log(网关页独立的"操作日志"区域)
    void appendGatewayLog(const QString &text);

    // 对应Python: gateway_widget.GatewayWidget._platform_cards 的一项
    struct GatewayCard {
        QFrame *card = nullptr;
        QCheckBox *enableCb = nullptr;
        QLabel *tokenLabel = nullptr;
        QWidget *formWidget = nullptr;
        QMap<QString, QLineEdit *> fieldInputs; // field -> 输入框
    };

    HermesBackend *m_backend = nullptr;
    HermesTaskModel *m_tasks = nullptr;
    HermesGateway *m_gateway = nullptr;

    QList<HermesCronJob> m_jobs;
    QList<HermesSkillInfo> m_skills;

    // 连接模式 + 主 Tab(懒加载依赖 currentChanged)
    QComboBox *m_modeCombo = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    // 新增 4 个功能页
    HermesAgentWidget *m_agentWidget = nullptr;
    HermesStatusWidget *m_statusWidget = nullptr;
    HermesConfigWidget *m_configWidget = nullptr;
    HermesMemoryWidget *m_memoryWidget = nullptr;
    // 既有 3 个 Tab 的页面容器(onTabChanged 里按指针识别)
    QWidget *m_cronTab = nullptr;
    QWidget *m_gatewayTab = nullptr;
    QWidget *m_skillsTab = nullptr;

    // cron tab
    QTableWidget *m_jobTable = nullptr;
    QTextEdit *m_jobDetail = nullptr;
    QLineEdit *m_scheduleEdit = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_skillsEdit = nullptr;
    QComboBox *m_deliverCombo = nullptr;
    QPlainTextEdit *m_promptEdit = nullptr;
    // gateway tab
    QLabel *m_gatewayStatusLabel = nullptr;
    QPushButton *m_gatewayStartBtn = nullptr;
    QPushButton *m_gatewayStopBtn = nullptr;
    // 网关页独立日志 对应Python: gateway_widget._log_text
    QTextEdit *m_gatewayLogText = nullptr;
    // platform_id -> 卡片控件 / 表单展开状态
    // 对应Python: _platform_cards / _expanded
    QMap<QString, GatewayCard> m_gatewayCards;
    QMap<QString, bool> m_gatewayExpanded;
    // skills tab
    QListWidget *m_skillList = nullptr;
    QTextEdit *m_skillContent = nullptr;
    QLineEdit *m_skillNameEdit = nullptr;
    // shared log
    QTextEdit *m_logView = nullptr;
};

} // namespace cubeshell
