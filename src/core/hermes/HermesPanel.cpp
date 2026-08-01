// HermesPanel.cpp — see HermesPanel.h for the port map.
// 对应Python: core/hermes/hermes_panel.py + cron_widget.py + gateway_widget.py
//             + skills_widget.py 的 UI 部分

#include "hermes/HermesPanel.h"

#include "hermes/HermesBackend.h"
#include "ssh/CommandExecutor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVariant>

namespace cubeshell {

namespace {

// Python 的 GatewayWidget / SkillsWidget 各自持有独立的 worker,日志互不干扰。
// C++ 侧 HermesGateway 一个对象同时服务这两页,commandDone/errorOccurred 是共用
// 信号,因此往网关页的独立日志里转发前要按描述剔除 Skills 的条目。
// 对应Python: gateway_widget.py:534-543 只接收 GatewayWorker 自己的信号
bool isSkillsMessage(const QString &text)
{
    return text.startsWith(QStringLiteral("安装 Skill"))
        || text.startsWith(QStringLiteral("删除 Skill"))
        || text.contains(QStringLiteral("Skill 名称"));
}

} // namespace

HermesPanel::HermesPanel(QWidget *parent)
    : QWidget(parent)
{
    m_backend = new HermesBackend(this);
    m_tasks = new HermesTaskModel(m_backend, this);
    m_gateway = new HermesGateway(m_backend, this);

    buildUi();

    // Worker 线程发出的信号,必须显式 Qt::QueuedConnection 回到 UI 线程
    connect(m_tasks, &HermesTaskModel::jobsLoaded,
            this, &HermesPanel::onJobsLoaded, Qt::QueuedConnection);
    connect(m_tasks, &HermesTaskModel::outputLoaded,
            this, &HermesPanel::onOutputLoaded, Qt::QueuedConnection);
    connect(m_tasks, &HermesTaskModel::commandDone,
            this, &HermesPanel::onCommandDone, Qt::QueuedConnection);
    connect(m_tasks, &HermesTaskModel::errorOccurred,
            this, &HermesPanel::onErrorOccurred, Qt::QueuedConnection);

    connect(m_gateway, &HermesGateway::configLoaded,
            this, &HermesPanel::onConfigLoaded, Qt::QueuedConnection);
    connect(m_gateway, &HermesGateway::statusChecked,
            this, &HermesPanel::onStatusChecked, Qt::QueuedConnection);
    connect(m_gateway, &HermesGateway::skillsLoaded,
            this, &HermesPanel::onSkillsLoaded, Qt::QueuedConnection);
    connect(m_gateway, &HermesGateway::commandDone,
            this, &HermesPanel::onCommandDone, Qt::QueuedConnection);
    connect(m_gateway, &HermesGateway::errorOccurred,
            this, &HermesPanel::onErrorOccurred, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void HermesPanel::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    // --- 连接模式选择 --- 对应Python: hermes_panel 顶部的连接下拉框
    //(与 ClaudeCodePanel 的连接模式同款;index 0 恒为"本地")
    auto *topLayout = new QHBoxLayout();
    topLayout->addWidget(new QLabel(tr("连接模式："), this));
    m_modeCombo = new QComboBox(this);
    m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modeCombo->setMinimumWidth(200);
    m_modeCombo->addItem(tr("本地"), QStringLiteral("local"));
    topLayout->addWidget(m_modeCombo);
    topLayout->addStretch();
    rootLayout->addLayout(topLayout);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    rootLayout->addWidget(splitter);

    auto *tabs = new QTabWidget(splitter);
    m_tabWidget = tabs;

    // --- Agent 管理 Tab --- 对应Python: agent_widget.AgentWidget
    m_agentWidget = new HermesAgentWidget(tabs);
    m_agentWidget->setBackend(m_backend);
    tabs->addTab(m_agentWidget, QStringLiteral("Agent 管理"));

    // --- 部署状态 Tab --- 对应Python: status_widget.StatusWidget
    m_statusWidget = new HermesStatusWidget(tabs);
    m_statusWidget->setBackend(m_backend);
    tabs->addTab(m_statusWidget, QStringLiteral("部署状态"));

    // --- 配置管理 Tab --- 对应Python: config_widget.ConfigWidget
    m_configWidget = new HermesConfigWidget(tabs);
    m_configWidget->setBackend(m_backend);
    tabs->addTab(m_configWidget, QStringLiteral("配置管理"));

    // --- Memory 浏览 Tab --- 对应Python: memory_widget.MemoryWidget
    m_memoryWidget = new HermesMemoryWidget(tabs);
    m_memoryWidget->setBackend(m_backend);
    tabs->addTab(m_memoryWidget, QStringLiteral("Memory 浏览"));

    // --- 定时任务 Tab --- 对应Python: cron_widget.CronWidget
    auto *cronTab = new QWidget(tabs);
    m_cronTab = cronTab;
    auto *cronLayout = new QVBoxLayout(cronTab);

    m_jobTable = new QTableWidget(0, 6, cronTab);
    m_jobTable->setHorizontalHeaderLabels(
        {QStringLiteral("名称"), QStringLiteral("计划"),
         QStringLiteral("状态"), QStringLiteral("上次运行"),
         QStringLiteral("投递"), QStringLiteral("Profile")});
    m_jobTable->horizontalHeader()->setStretchLastSection(true);
    m_jobTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_jobTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_jobTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_jobTable, &QTableWidget::itemSelectionChanged,
            this, &HermesPanel::onJobRowChanged);
    cronLayout->addWidget(m_jobTable, 2);

    m_jobDetail = new QTextEdit(cronTab);
    m_jobDetail->setReadOnly(true);
    m_jobDetail->setPlaceholderText(QStringLiteral("选择任务查看详情"));
    cronLayout->addWidget(m_jobDetail, 1);

    // 新建任务表单 对应Python: CronWidget 的创建区域
    auto *formRow1 = new QHBoxLayout();
    m_scheduleEdit = new QLineEdit(cronTab);
    m_scheduleEdit->setPlaceholderText(
        QStringLiteral("Cron 表达式,如 0 9 * * * 或 @hourly"));
    m_nameEdit = new QLineEdit(cronTab);
    m_nameEdit->setPlaceholderText(QStringLiteral("任务名称(可选)"));
    formRow1->addWidget(new QLabel(QStringLiteral("计划:"), cronTab));
    formRow1->addWidget(m_scheduleEdit, 2);
    formRow1->addWidget(new QLabel(QStringLiteral("名称:"), cronTab));
    formRow1->addWidget(m_nameEdit, 1);
    cronLayout->addLayout(formRow1);

    auto *formRow2 = new QHBoxLayout();
    m_skillsEdit = new QLineEdit(cronTab);
    m_skillsEdit->setPlaceholderText(QStringLiteral("技能列表,逗号分隔(可选)"));
    m_deliverCombo = new QComboBox(cronTab);
    m_deliverCombo->addItems({QStringLiteral("none"), QStringLiteral("telegram"),
                              QStringLiteral("discord"), QStringLiteral("slack"),
                              QStringLiteral("feishu"), QStringLiteral("wecom"),
                              QStringLiteral("dingtalk")});
    formRow2->addWidget(new QLabel(QStringLiteral("技能:"), cronTab));
    formRow2->addWidget(m_skillsEdit, 2);
    formRow2->addWidget(new QLabel(QStringLiteral("投递:"), cronTab));
    formRow2->addWidget(m_deliverCombo, 1);
    cronLayout->addLayout(formRow2);

    m_promptEdit = new QPlainTextEdit(cronTab);
    m_promptEdit->setPlaceholderText(QStringLiteral("任务提示词"));
    m_promptEdit->setMaximumHeight(72);
    cronLayout->addWidget(m_promptEdit);

    auto *cronBtnRow = new QHBoxLayout();
    auto *refreshJobsBtn = new QPushButton(QStringLiteral("刷新"), cronTab);
    auto *createJobBtn = new QPushButton(QStringLiteral("创建任务"), cronTab);
    auto *deleteJobBtn = new QPushButton(QStringLiteral("删除"), cronTab);
    auto *pauseResumeBtn = new QPushButton(QStringLiteral("暂停/恢复"), cronTab);
    auto *runNowBtn = new QPushButton(QStringLiteral("立即运行"), cronTab);
    connect(refreshJobsBtn, &QPushButton::clicked,
            m_tasks, &HermesTaskModel::loadJobs);
    connect(createJobBtn, &QPushButton::clicked,
            this, &HermesPanel::onCreateJobClicked);
    connect(deleteJobBtn, &QPushButton::clicked,
            this, &HermesPanel::onDeleteJobClicked);
    connect(pauseResumeBtn, &QPushButton::clicked,
            this, &HermesPanel::onPauseResumeClicked);
    connect(runNowBtn, &QPushButton::clicked,
            this, &HermesPanel::onRunNowClicked);
    cronBtnRow->addWidget(refreshJobsBtn);
    cronBtnRow->addWidget(createJobBtn);
    cronBtnRow->addWidget(deleteJobBtn);
    cronBtnRow->addWidget(pauseResumeBtn);
    cronBtnRow->addWidget(runNowBtn);
    cronBtnRow->addStretch(1);
    cronLayout->addLayout(cronBtnRow);

    tabs->addTab(cronTab, QStringLiteral("定时任务"));

    // --- 网关 Tab --- 对应Python: gateway_widget.GatewayWidget
    //(addTab 挪到技能 Tab 之后,保持与 Python 的 Tab 顺序一致)
    auto *gatewayTab = new QWidget(tabs);
    m_gatewayTab = gatewayTab;
    // 对应Python: core/hermes/gateway_widget.py:273-275
    auto *gatewayLayout = new QVBoxLayout(gatewayTab);
    gatewayLayout->setContentsMargins(8, 8, 8, 8);
    gatewayLayout->setSpacing(10);

    // 顶部:网关状态栏 对应Python: core/hermes/gateway_widget.py:277-296
    //(顺序 = 状态标签 → 伸缩 → 启动 → 停止 → 刷新)
    auto *gwStatusRow = new QHBoxLayout();
    m_gatewayStatusLabel = new QLabel(tr("网关状态：未知"), gatewayTab);
    m_gatewayStatusLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    gwStatusRow->addWidget(m_gatewayStatusLabel);
    gwStatusRow->addStretch();

    m_gatewayStartBtn = new QPushButton(tr("启动网关"), gatewayTab);
    connect(m_gatewayStartBtn, &QPushButton::clicked,
            this, &HermesPanel::onStartGatewayClicked);
    gwStatusRow->addWidget(m_gatewayStartBtn);

    m_gatewayStopBtn = new QPushButton(tr("停止网关"), gatewayTab);
    connect(m_gatewayStopBtn, &QPushButton::clicked,
            this, &HermesPanel::onStopGatewayClicked);
    gwStatusRow->addWidget(m_gatewayStopBtn);

    auto *gwRefreshBtn = new QPushButton(tr("刷新状态"), gatewayTab);
    connect(gwRefreshBtn, &QPushButton::clicked,
            m_gateway, &HermesGateway::checkStatus);
    gwStatusRow->addWidget(gwRefreshBtn);
    gatewayLayout->addLayout(gwStatusRow);

    // 主体:平台卡片网格(3 列,可滚动)
    // 对应Python: core/hermes/gateway_widget.py:298-311
    auto *gwScroll = new QScrollArea(gatewayTab);
    gwScroll->setWidgetResizable(true);
    gwScroll->setFrameShape(QFrame::NoFrame);
    auto *gwScrollContent = new QWidget(gwScroll);
    auto *gwGrid = new QGridLayout(gwScrollContent);
    gwGrid->setSpacing(10);
    gwGrid->setContentsMargins(0, 0, 0, 0);
    buildPlatformCards(gwGrid, gwScrollContent);
    gwScroll->setWidget(gwScrollContent);
    gatewayLayout->addWidget(gwScroll, 1);

    // 底部:网关页独立的操作日志
    // 对应Python: core/hermes/gateway_widget.py:313-320
    auto *gwLogGroup = new QGroupBox(tr("操作日志"), gatewayTab);
    auto *gwLogLayout = new QVBoxLayout(gwLogGroup);
    m_gatewayLogText = new QTextEdit(gwLogGroup);
    m_gatewayLogText->setReadOnly(true);
    m_gatewayLogText->setMaximumHeight(150);
    gwLogLayout->addWidget(m_gatewayLogText);
    gatewayLayout->addWidget(gwLogGroup);

    // --- 技能 Tab --- 对应Python: skills_widget.SkillsWidget
    auto *skillsTab = new QWidget(tabs);
    m_skillsTab = skillsTab;
    auto *skillsLayout = new QHBoxLayout(skillsTab);

    auto *skillLeft = new QVBoxLayout();
    m_skillList = new QListWidget(skillsTab);
    connect(m_skillList, &QListWidget::currentRowChanged,
            this, &HermesPanel::onSkillRowChanged);
    skillLeft->addWidget(m_skillList, 1);

    auto *skillOpRow = new QHBoxLayout();
    m_skillNameEdit = new QLineEdit(skillsTab);
    m_skillNameEdit->setPlaceholderText(QStringLiteral("技能名称"));
    auto *installSkillBtn = new QPushButton(QStringLiteral("安装"), skillsTab);
    auto *removeSkillBtn = new QPushButton(QStringLiteral("删除"), skillsTab);
    auto *refreshSkillsBtn = new QPushButton(QStringLiteral("刷新"), skillsTab);
    connect(installSkillBtn, &QPushButton::clicked,
            this, &HermesPanel::onInstallSkillClicked);
    connect(removeSkillBtn, &QPushButton::clicked,
            this, &HermesPanel::onRemoveSkillClicked);
    connect(refreshSkillsBtn, &QPushButton::clicked,
            m_gateway, &HermesGateway::loadSkills);
    skillOpRow->addWidget(m_skillNameEdit, 1);
    skillOpRow->addWidget(installSkillBtn);
    skillOpRow->addWidget(removeSkillBtn);
    skillOpRow->addWidget(refreshSkillsBtn);
    skillLeft->addLayout(skillOpRow);
    skillsLayout->addLayout(skillLeft, 1);

    m_skillContent = new QTextEdit(skillsTab);
    m_skillContent->setReadOnly(true);
    m_skillContent->setPlaceholderText(QStringLiteral("选择技能查看 SKILL.md"));
    skillsLayout->addWidget(m_skillContent, 2);

    tabs->addTab(skillsTab, QStringLiteral("Skills 管理"));
    tabs->addTab(gatewayTab, QStringLiteral("消息网关"));

    // --- 下半部:共享日志 ---
    m_logView = new QTextEdit(splitter);
    m_logView->setReadOnly(true);
    m_logView->setPlaceholderText(QStringLiteral("执行状态与操作日志"));

    splitter->addWidget(tabs);
    splitter->addWidget(m_logView);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    // Tab 切换懒加载 + 连接模式切换 + Agent 页"在终端中打开"转发。
    // 所有 Tab 加完后再连,避免 addTab 过程触发多余的 currentChanged 刷新。
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, &HermesPanel::onTabChanged);
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &HermesPanel::onModeChanged);
    connect(m_agentWidget, &HermesAgentWidget::openTerminalRequested,
            this, &HermesPanel::openTerminalRequested);
}

// ---------------------------------------------------------------------------
// gateway platform cards
// ---------------------------------------------------------------------------

// 对应Python: core/hermes/gateway_widget.py:322-328 _build_platform_cards
// Python 在 _init_ui 里一次性为 PLATFORMS 全表建卡(8 张,3 列),配置加载完成后
// 只更新卡片内容而不重建,这里照搬同一时机与顺序。
void HermesPanel::buildPlatformCards(QGridLayout *grid, QWidget *parent)
{
    const QList<GatewayPlatform> all = HermesGateway::platforms();
    for (int idx = 0; idx < all.size(); ++idx)
        grid->addWidget(createPlatformCard(all.at(idx), parent), idx / 3, idx % 3);
}

// 对应Python: core/hermes/gateway_widget.py:330-405 _create_platform_card
QFrame *HermesPanel::createPlatformCard(const GatewayPlatform &platform,
                                       QWidget *parent)
{
    const QString platformId = platform.id;

    auto *card = new QFrame(parent);
    card->setFrameShape(QFrame::StyledPanel);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setSpacing(6);

    // 平台名称(中文名来自 HermesGateway::platforms(),如 飞书/企业微信/钉钉)
    auto *nameLabel = new QLabel(platform.name, card);
    nameLabel->setStyleSheet(
        QStringLiteral("font-weight: bold; font-size: 14px; border: none;"));
    cardLayout->addWidget(nameLabel);

    // 启用/禁用。Python 用 stateChanged,该信号自 Qt 6.7 起已废弃,这里用语义
    // 等价的 toggled(bool)(非三态复选框下两者一致),避免引入废弃告警。
    auto *enableCb = new QCheckBox(tr("启用"), card);
    connect(enableCb, &QCheckBox::toggled, this,
            [this, platformId](bool checked) {
                onPlatformToggled(platformId, checked);
            });
    cardLayout->addWidget(enableCb);

    // Token 状态
    auto *tokenLabel = new QLabel(tr("Token: 未配置"), card);
    tokenLabel->setStyleSheet(QStringLiteral("color: #999; border: none;"));
    cardLayout->addWidget(tokenLabel);

    // 按钮行
    auto *btnRow = new QHBoxLayout();
    auto *configBtn = new QPushButton(tr("配置"), card);
    connect(configBtn, &QPushButton::clicked, this,
            [this, platformId]() { onToggleConfigForm(platformId); });
    btnRow->addWidget(configBtn);

    auto *testBtn = new QPushButton(tr("测试"), card);
    connect(testBtn, &QPushButton::clicked, this,
            [this, platformId]() { onTestPlatformClicked(platformId); });
    btnRow->addWidget(testBtn);
    cardLayout->addLayout(btnRow);

    // 配置表单(默认隐藏)
    auto *formWidget = new QWidget(card);
    auto *formLayout = new QFormLayout(formWidget);
    formLayout->setContentsMargins(0, 6, 0, 0);

    GatewayCard info;
    for (const QString &field : platform.fields) {
        auto *lineEdit = new QLineEdit(formWidget);
        // Token/Secret 类字段使用密码模式 对应Python: _SECRET_KEYWORDS
        if (HermesGateway::isSecretField(field))
            lineEdit->setEchoMode(QLineEdit::Password);
        lineEdit->setPlaceholderText(field);
        formLayout->addRow(new QLabel(field + QLatin1Char(':'), formWidget),
                           lineEdit);
        info.fieldInputs.insert(field, lineEdit);
    }

    auto *saveBtn = new QPushButton(tr("保存配置"), formWidget);
    connect(saveBtn, &QPushButton::clicked, this,
            [this, platformId]() { onSavePlatformConfigClicked(platformId); });
    formLayout->addRow(QString(), saveBtn);

    formWidget->setVisible(false);
    cardLayout->addWidget(formWidget);

    // 记录卡片组件引用 对应Python: _platform_cards / _expanded
    info.card = card;
    info.enableCb = enableCb;
    info.tokenLabel = tokenLabel;
    info.formWidget = formWidget;
    m_gatewayCards.insert(platformId, info);
    m_gatewayExpanded.insert(platformId, false);

    return card;
}

// ---------------------------------------------------------------------------
// refresh / mode / lazy loading
// ---------------------------------------------------------------------------

// 对应Python: 各 widget 的 refresh(只刷新当前可见 Tab,懒加载)
void HermesPanel::refresh()
{
    onTabChanged(m_tabWidget->currentIndex());
}

// 主窗口注入已建立的 SSH 会话(index 0 恒为"本地",其余重建)。
void HermesPanel::setAvailableConnections(const QStringList &hosts,
                                          const QList<CommandExecutor *> &executors)
{
    while (m_modeCombo->count() > 1)
        m_modeCombo->removeItem(1);
    for (int i = 0; i < hosts.size() && i < executors.size(); ++i) {
        m_modeCombo->addItem(tr("远程: %1").arg(hosts.at(i)),
                             QVariant::fromValue(executors.at(i)));
    }
}

// index 0 = 本地(executor 置空);index > 0 = 远程(itemData 里的 executor)
void HermesPanel::onModeChanged(int index)
{
    if (index <= 0) {
        m_backend->setRemoteExecutor(nullptr);
    } else {
        auto *executor = m_modeCombo->itemData(index).value<CommandExecutor *>();
        m_backend->setRemoteExecutor(executor);
    }
    // 切换后端后重新加载当前可见 Tab
    onTabChanged(m_tabWidget->currentIndex());
}

// Tab 激活时才加载对应数据。对应Python: 各 widget.refresh(Tab 切换触发)
void HermesPanel::onTabChanged(int index)
{
    QWidget *w = m_tabWidget->widget(index);
    if (w == m_agentWidget) {
        m_agentWidget->refresh();
    } else if (w == m_statusWidget) {
        m_statusWidget->refresh();
    } else if (w == m_configWidget) {
        m_configWidget->refresh();
    } else if (w == m_memoryWidget) {
        m_memoryWidget->refresh();
    } else if (w == m_cronTab) {
        m_tasks->loadJobs();
    } else if (w == m_gatewayTab) {
        // 对应Python: gateway_widget.refresh 只触发 load_config,
        // 状态检查由 _on_config_loaded 串起来(避免重复的 CLI 调用)
        m_gateway->loadConfig();
    } else if (w == m_skillsTab) {
        m_gateway->loadSkills();
    }
}

// ---------------------------------------------------------------------------
// cron slots
// ---------------------------------------------------------------------------

const HermesCronJob *HermesPanel::selectedJob() const
{
    const int row = m_jobTable->currentRow();
    if (row < 0 || row >= m_jobs.size())
        return nullptr;
    return &m_jobs.at(row);
}

void HermesPanel::onCreateJobClicked()
{
    const QString schedule = m_scheduleEdit->text().trimmed();
    const QString prompt = m_promptEdit->toPlainText().trimmed();
    if (schedule.isEmpty() || prompt.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
                             QStringLiteral("计划表达式与提示词不能为空"));
        return;
    }
    QString cronError;
    if (!HermesTaskModel::validateCronExpression(schedule, &cronError)) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
                             QStringLiteral("Cron 表达式无效:%1").arg(cronError));
        return;
    }
    m_tasks->createJob(schedule, prompt, m_nameEdit->text().trimmed(),
                       m_skillsEdit->text().trimmed(),
                       m_deliverCombo->currentText());
}

void HermesPanel::onDeleteJobClicked()
{
    const HermesCronJob *job = selectedJob();
    if (!job)
        return;
    if (QMessageBox::question(this, QStringLiteral("Hermes"),
                              QStringLiteral("确定删除任务 %1 ?")
                                  .arg(job->name.isEmpty() ? job->id : job->name))
        != QMessageBox::Yes)
        return;
    m_tasks->removeJob(job->id, job->profileSource);
}

void HermesPanel::onPauseResumeClicked()
{
    const HermesCronJob *job = selectedJob();
    if (!job)
        return;
    // 对应Python: CronWidget 依 enabled/state 决定 pause 还是 resume
    const bool paused = !job->enabled
        || job->state.compare(QLatin1String("paused"), Qt::CaseInsensitive) == 0;
    if (paused)
        m_tasks->resumeJob(job->id, job->profileSource);
    else
        m_tasks->pauseJob(job->id, job->profileSource);
}

void HermesPanel::onRunNowClicked()
{
    const HermesCronJob *job = selectedJob();
    if (!job)
        return;
    m_logView->append(QStringLiteral("[%1] 触发运行:%2 ...")
                          .arg(QDateTime::currentDateTime()
                                   .toString(QStringLiteral("HH:mm:ss")),
                               job->name.isEmpty() ? job->id : job->name));
    m_tasks->runJobNow(job->id, job->profileSource);
}

void HermesPanel::onJobRowChanged()
{
    const HermesCronJob *job = selectedJob();
    if (!job) {
        m_jobDetail->clear();
        return;
    }
    // 详情 = 原始 JSON 美化输出
    const QJsonDocument doc(job->raw);
    m_jobDetail->setPlainText(
        QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
    m_tasks->loadOutput(job->id, job->profileSource);
}

void HermesPanel::onJobsLoaded(const QList<HermesCronJob> &jobs)
{
    m_jobs = jobs;
    m_jobTable->setRowCount(jobs.size());
    for (int i = 0; i < jobs.size(); ++i) {
        const HermesCronJob &job = jobs.at(i);
        const QString state = job.enabled
            ? (job.state.isEmpty() ? QStringLiteral("active") : job.state)
            : QStringLiteral("paused");
        m_jobTable->setItem(i, 0, new QTableWidgetItem(
            job.name.isEmpty() ? job.id : job.name));
        m_jobTable->setItem(i, 1, new QTableWidgetItem(job.schedule));
        m_jobTable->setItem(i, 2, new QTableWidgetItem(state));
        m_jobTable->setItem(i, 3, new QTableWidgetItem(job.lastRun));
        m_jobTable->setItem(i, 4, new QTableWidgetItem(job.deliver));
        m_jobTable->setItem(i, 5, new QTableWidgetItem(job.profileSource));
    }
    m_logView->append(QStringLiteral("已加载 %1 个定时任务").arg(jobs.size()));
}

void HermesPanel::onOutputLoaded(const QString &logContent)
{
    if (logContent.isEmpty())
        return;
    m_jobDetail->append(QStringLiteral("\n--- 最近输出 ---\n") + logContent);
}

// ---------------------------------------------------------------------------
// gateway slots
// ---------------------------------------------------------------------------

// 对应Python: core/hermes/gateway_widget.py:431-437 _start_gateway
void HermesPanel::onStartGatewayClicked()
{
    appendGatewayLog(tr("正在启动网关..."));
    setGatewayStatusPending(tr("网关状态：启动中..."));
    m_gateway->startGateway();
}

// 对应Python: core/hermes/gateway_widget.py:439-445 _stop_gateway
void HermesPanel::onStopGatewayClicked()
{
    appendGatewayLog(tr("正在停止网关..."));
    setGatewayStatusPending(tr("网关状态：停止中..."));
    m_gateway->stopGateway();
}

// 对应Python: core/hermes/gateway_widget.py:447-452 _set_status_pending
// 启停过程中显示过渡状态,并禁用启停按钮避免重复操作
void HermesPanel::setGatewayStatusPending(const QString &text)
{
    m_gatewayStatusLabel->setText(text);
    m_gatewayStatusLabel->setStyleSheet(
        QStringLiteral("font-weight: bold; color: #e0a800;"));
    m_gatewayStartBtn->setEnabled(false);
    m_gatewayStopBtn->setEnabled(false);
}

// 对应Python: core/hermes/gateway_widget.py:454-459 _toggle_platform
// Python 把 enabled 写进内存里的 _gateway_config,但自身从不读取该值(保存时另
// 取复选框状态,worker 也丢弃 enabled),所以这里只保留等价的日志输出,不引入
// 一份没人消费的状态。
void HermesPanel::onPlatformToggled(const QString &platformId, bool enabled)
{
    appendGatewayLog(tr("平台 %1 %2")
                         .arg(platformId,
                              enabled ? tr("已启用") : tr("已禁用")));
}

// 对应Python: core/hermes/gateway_widget.py:461-468 _toggle_config_form
void HermesPanel::onToggleConfigForm(const QString &platformId)
{
    auto it = m_gatewayCards.find(platformId);
    if (it == m_gatewayCards.end())
        return;
    const bool expanded = !m_gatewayExpanded.value(platformId, false);
    m_gatewayExpanded.insert(platformId, expanded);
    it->formWidget->setVisible(expanded);
}

// 对应Python: core/hermes/gateway_widget.py:470-488 _save_platform_config
void HermesPanel::onSavePlatformConfigClicked(const QString &platformId)
{
    auto it = m_gatewayCards.find(platformId);
    if (it == m_gatewayCards.end())
        return;

    // 只提交非空值(与 Python 一致:空输入不写入 .env)
    QMap<QString, QString> fields;
    for (auto fit = it->fieldInputs.cbegin(); fit != it->fieldInputs.cend();
         ++fit) {
        const QString value = fit.value()->text().trimmed();
        if (!value.isEmpty())
            fields.insert(fit.key(), value);
    }

    appendGatewayLog(tr("正在保存 %1 配置...").arg(platformId));
    m_gateway->savePlatformConfig(platformId, fields);
}

// 对应Python: core/hermes/gateway_widget.py:490-496 _test_platform
//(hermes send --to <platform> "Test from CubeShell")
void HermesPanel::onTestPlatformClicked(const QString &platformId)
{
    appendGatewayLog(tr("正在测试 %1 ...").arg(platformId));
    m_gateway->testPlatform(platformId);
}

// 对应Python: core/hermes/gateway_widget.py:514-520 _on_config_loaded
void HermesPanel::onConfigLoaded(const QList<GatewayPlatformConfig> &configs)
{
    updateCardsFromConfig(configs);
    appendGatewayLog(tr("网关配置已加载"));
    // 加载完配置后自动检查状态
    m_gateway->checkStatus();
}

// 对应Python: core/hermes/gateway_widget.py:547-579 _update_cards_from_config
void HermesPanel::updateCardsFromConfig(
    const QList<GatewayPlatformConfig> &configs)
{
    // configs 只包含 .env 里有值的平台;其余平台按"空配置"处理
    QMap<QString, const GatewayPlatformConfig *> byId;
    for (const GatewayPlatformConfig &cfg : configs)
        byId.insert(cfg.id, &cfg);

    for (auto it = m_gatewayCards.begin(); it != m_gatewayCards.end(); ++it) {
        const GatewayPlatformConfig *cfg = byId.value(it.key(), nullptr);
        const bool connected = cfg && cfg->connected;
        bool hasAnyValue = false;
        if (cfg) {
            for (auto fit = it->fieldInputs.cbegin();
                 fit != it->fieldInputs.cend(); ++fit) {
                if (!cfg->values.value(fit.key()).isEmpty()) {
                    hasAnyValue = true;
                    break;
                }
            }
        }

        // 更新启用状态(有配置且已连接 → 启用);blockSignals 防止误触发日志
        it->enableCb->blockSignals(true);
        it->enableCb->setChecked(connected);
        it->enableCb->blockSignals(false);

        // 更新 Token 状态显示
        if (connected) {
            it->tokenLabel->setText(tr("状态: 已连接"));
            it->tokenLabel->setStyleSheet(
                QStringLiteral("color: green; border: none;"));
        } else if (hasAnyValue) {
            it->tokenLabel->setText(tr("Token: 已配置"));
            it->tokenLabel->setStyleSheet(
                QStringLiteral("color: orange; border: none;"));
        } else {
            it->tokenLabel->setText(tr("Token: 未配置"));
            it->tokenLabel->setStyleSheet(
                QStringLiteral("color: #999; border: none;"));
        }

        // 填充字段值
        for (auto fit = it->fieldInputs.cbegin(); fit != it->fieldInputs.cend();
             ++fit) {
            fit.value()->setText(cfg ? cfg->values.value(fit.key()) : QString());
        }
    }
}

// 对应Python: core/hermes/gateway_widget.py:522-532 _on_status_checked
void HermesPanel::onStatusChecked(bool isRunning)
{
    if (isRunning) {
        m_gatewayStatusLabel->setText(tr("网关状态：运行中"));
        m_gatewayStatusLabel->setStyleSheet(
            QStringLiteral("font-weight: bold; color: green;"));
    } else {
        m_gatewayStatusLabel->setText(tr("网关状态：已停止"));
        m_gatewayStatusLabel->setStyleSheet(
            QStringLiteral("font-weight: bold; color: red;"));
    }
    // 运行中:仅允许停止;已停止:仅允许启动
    m_gatewayStartBtn->setEnabled(!isRunning);
    m_gatewayStopBtn->setEnabled(isRunning);
}

// 对应Python: core/hermes/gateway_widget.py:581-583 _append_log
void HermesPanel::appendGatewayLog(const QString &text)
{
    m_gatewayLogText->append(text);
}

// ---------------------------------------------------------------------------
// skills slots
// ---------------------------------------------------------------------------

void HermesPanel::onSkillsLoaded(const QList<HermesSkillInfo> &skills)
{
    m_skills = skills;
    m_skillList->clear();
    for (const HermesSkillInfo &s : skills) {
        QString label = s.name;
        if (!s.version.isEmpty())
            label += QStringLiteral(" (%1)").arg(s.version);
        m_skillList->addItem(label);
    }
    m_logView->append(QStringLiteral("已加载 %1 个技能").arg(skills.size()));
}

void HermesPanel::onSkillRowChanged(int row)
{
    if (row < 0 || row >= m_skills.size()) {
        m_skillContent->clear();
        return;
    }
    const HermesSkillInfo &s = m_skills.at(row);
    m_skillContent->setPlainText(s.content);
    m_skillNameEdit->setText(s.name);
}

void HermesPanel::onInstallSkillClicked()
{
    const QString name = m_skillNameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Hermes"),
                             QStringLiteral("请输入技能名称"));
        return;
    }
    m_gateway->installSkill(name);
}

void HermesPanel::onRemoveSkillClicked()
{
    const int row = m_skillList->currentRow();
    if (row < 0 || row >= m_skills.size())
        return;
    const HermesSkillInfo &s = m_skills.at(row);
    if (QMessageBox::question(this, QStringLiteral("Hermes"),
                              QStringLiteral("确定删除技能 %1 ?").arg(s.name))
        != QMessageBox::Yes)
        return;
    m_gateway->removeSkill(s.dirName.isEmpty() ? s.name : s.dirName);
}

// ---------------------------------------------------------------------------
// shared slots
// ---------------------------------------------------------------------------

void HermesPanel::onCommandDone(const QString &description, const QString &output)
{
    m_logView->append(QStringLiteral("[%1] %2\n%3")
                          .arg(QDateTime::currentDateTime()
                                   .toString(QStringLiteral("HH:mm:ss")),
                               description, output.trimmed()));
    // 任务操作完成后自动刷新列表
    if (description.contains(QStringLiteral("任务")))
        m_tasks->loadJobs();
    // 网关自身的输出再进网关页的独立日志
    // 对应Python: core/hermes/gateway_widget.py:534-536 _on_command_done
    if (sender() == m_gateway && !isSkillsMessage(description)) {
        appendGatewayLog(
            QStringLiteral("[%1] %2").arg(description, output.trimmed()));
    }
}

void HermesPanel::onErrorOccurred(const QString &message)
{
    m_logView->append(QStringLiteral("[错误] %1").arg(message));
    // 对应Python: core/hermes/gateway_widget.py:538-543 _on_error
    // 出错后按真实状态恢复按钮,避免卡在过渡态或误开两个按钮。只处理网关自身
    // 的错误(Python 每个页面有独立 worker;C++ 这里按 sender + 描述过滤)。
    if (sender() == m_gateway && !isSkillsMessage(message)) {
        appendGatewayLog(QStringLiteral("[错误] %1").arg(message));
        m_gateway->checkStatus();
    }
}

} // namespace cubeshell
