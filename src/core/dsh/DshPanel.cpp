// DshPanel.cpp — DeepSeek Harness 管理面板。见 DshPanel.h 的布局说明。

#include "dsh/DshPanel.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "dsh/DshPluginPanel.h"
#include "dsh/DshSessionPanel.h"
#include "dsh/DshSettingsPanel.h"

namespace cubeshell {

DshPanel::DshPanel(QWidget *parent)
    : QWidget(parent)
{
    m_manager = new DshManager(this);
    buildUi();
    loadSettings();

    connect(m_manager, &DshManager::statusChanged, this, &DshPanel::onStatusChanged);
    connect(m_manager, &DshManager::started, this, &DshPanel::onStarted);
    connect(m_manager, &DshManager::stopped, this, &DshPanel::onStopped);
    connect(m_manager, &DshManager::webReady, this, &DshPanel::onWebReady);
    connect(m_manager, &DshManager::logOutput, this, &DshPanel::onLogLine);
    connect(m_manager, &DshManager::errorOccurred, this, &DshPanel::onError);
    connect(m_manager, &DshManager::installLog, this, &DshPanel::onInstallLog);
    connect(m_manager, &DshManager::installFinished, this, &DshPanel::onInstallFinished);
    connect(m_manager, &DshManager::latestVersionChecked,
            this, &DshPanel::onLatestVersionChecked);
    connect(m_manager, &DshManager::environmentDetected,
            this, &DshPanel::onEnvironmentDetected);

    // 环境检测起 4 个子进程（登录 shell 取 PATH 一项就近 1 秒），必须异步，
    // 否则首次打开面板会卡住 UI 线程。这里只把界面置成「检测中」。
    refreshEnvironment();
    updateStatusUi();
}

// ---------------------------------------------------------------------------
// UI 构建
// ---------------------------------------------------------------------------

// 面板骨架：QTabWidget 容器 + 四个功能页（切换时懒加载，见 onTabChanged）。
void DshPanel::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->addTab(buildStatusTab(), tr("状态"));

    m_pluginPanel = new DshPluginPanel(m_manager, this);
    m_tabWidget->addTab(m_pluginPanel, tr("插件"));

    m_sessionPanel = new DshSessionPanel(m_manager, this);
    m_tabWidget->addTab(m_sessionPanel, tr("会话"));
    // 会话页的「在终端恢复」与状态页共用同一个出口。
    connect(m_sessionPanel, &DshSessionPanel::openCliRequested,
            this, &DshPanel::openCliRequested);

    m_settingsPanel = new DshSettingsPanel(m_manager, this);
    m_tabWidget->addTab(m_settingsPanel, tr("设置"));

    outer->addWidget(m_tabWidget);

    connect(m_tabWidget, &QTabWidget::currentChanged, this, &DshPanel::onTabChanged);

    // 所有子页都已建好，这里统一设手指光标（递归覆盖各 Tab 内控件）。
    setupCursors();
}

// 对应 ClaudeCodePanel::setupCursors：按钮/Tab 栏/下拉框为手指光标。
void DshPanel::setupCursors()
{
    setCursor(Qt::ArrowCursor);
    // Tab 栏
    m_tabWidget->tabBar()->setCursor(Qt::PointingHandCursor);
    // 递归设置所有按钮（含插件/会话/设置页内的按钮）
    const QList<QPushButton *> buttons = findChildren<QPushButton *>();
    for (QPushButton *btn : buttons)
        btn->setCursor(Qt::PointingHandCursor);
    // 递归设置所有下拉框（CLI 模式、profile 选择等）
    const QList<QComboBox *> combos = findChildren<QComboBox *>();
    for (QComboBox *combo : combos)
        combo->setCursor(Qt::PointingHandCursor);
}

// Tab 激活时刷新当前页（懒加载，避免打开面板就扫全部目录）。
void DshPanel::onTabChanged(int index)
{
    QWidget *w = m_tabWidget->widget(index);
    if (w == m_pluginPanel)
        m_pluginPanel->refresh();
    else if (w == m_sessionPanel)
        m_sessionPanel->refresh();
    else if (w == m_settingsPanel)
        m_settingsPanel->refresh();
}

// 「状态」页：运行状态 + 运行配置（同一行）+ 操作按钮 + 运行日志。
QWidget *DshPanel::buildStatusTab()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // --- 运行状态区 ---
    auto *statusBox = new QGroupBox(tr("运行状态"), page);
    auto *statusLay = new QFormLayout(statusBox);

    auto *statusRow = new QHBoxLayout();
    m_statusDot = new QLabel(this);
    m_statusDot->setFixedSize(12, 12);
    m_statusText = new QLabel(this);
    statusRow->addWidget(m_statusDot);
    statusRow->addWidget(m_statusText);
    statusRow->addStretch();
    statusLay->addRow(tr("状态:"), statusRow);

    m_pidLabel = new QLabel(QStringLiteral("-"), this);
    statusLay->addRow(tr("进程 PID:"), m_pidLabel);

    m_urlLabel = new QLabel(QStringLiteral("-"), this);
    m_urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLay->addRow(tr("Web 地址:"), m_urlLabel);

    m_envLabel = new QLabel(this);
    m_envLabel->setWordWrap(true);
    m_envLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLay->addRow(tr("Node 环境:"), m_envLabel);

    m_versionLabel = new QLabel(this);
    m_versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLay->addRow(tr("dsh 版本:"), m_versionLabel);

    // --- 运行配置区（与运行状态同行并排；含 web 监听 + CLI） ---
    auto *confBox = new QGroupBox(tr("运行配置"), page);
    auto *confLay = new QFormLayout(confBox);
    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setPlaceholderText(QStringLiteral("127.0.0.1"));
    m_hostEdit->setMaximumWidth(220);
    confLay->addRow(tr("监听地址:"), m_hostEdit);
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(0, 65535);
    m_portSpin->setSpecialValueText(tr("自动")); // 0 = 让 OS 选空闲端口
    m_portSpin->setValue(DshManager::kDefaultPort);
    m_portSpin->setMaximumWidth(220);
    confLay->addRow(tr("监听端口:"), m_portSpin);

    // CLI 模式：headless（一次性问答，内置）/ tui（交互式，需先装插件包）。
    m_cliModeCombo = new QComboBox(this);
    m_cliModeCombo->addItem(tr("一次性问答 (headless)"), QStringLiteral("headless"));
    m_cliModeCombo->addItem(tr("交互式界面 (tui)"), QStringLiteral("tui"));
    m_cliModeCombo->setMaximumWidth(220);
    confLay->addRow(tr("CLI 模式:"), m_cliModeCombo);

    m_cliTaskEdit = new QLineEdit(this);
    m_cliTaskEdit->setPlaceholderText(tr("交给 dsh 的任务（仅 headless 需要）"));
    m_cliTaskEdit->setMaximumWidth(220);
    confLay->addRow(tr("CLI 任务:"), m_cliTaskEdit);

    connect(m_cliModeCombo, &QComboBox::currentIndexChanged,
            this, &DshPanel::onCliModeChanged);

    // 运行状态 + 监听配置 放同一行（左右并排，等宽拉伸）。
    auto *topRow = new QHBoxLayout();
    topRow->addWidget(statusBox, 1);
    topRow->addWidget(confBox, 1);
    root->addLayout(topRow);

    // --- 操作按钮行 ---
    auto *btnRow = new QHBoxLayout();
    m_startBtn = new QPushButton(tr("启动"), this);
    m_stopBtn = new QPushButton(tr("停止"), this);
    m_restartBtn = new QPushButton(tr("重启"), this);
    m_openWebBtn = new QPushButton(tr("打开 Web 界面"), this);
    m_openCliBtn = new QPushButton(tr("终端运行 CLI"), this);
    m_openCliBtn->setToolTip(tr("先选择项目文件夹，再在该目录下新开终端运行 dsh CLI"));
    m_installBtn = new QPushButton(tr("全局安装 dsh"), this);
    m_updateBtn = new QPushButton(tr("更新 dsh"), this);
    m_checkUpdateBtn = new QPushButton(tr("检查更新"), this);
    m_refreshEnvBtn = new QPushButton(tr("刷新环境"), this);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_stopBtn);
    btnRow->addWidget(m_restartBtn);
    btnRow->addSpacing(12);
    btnRow->addWidget(m_openWebBtn);
    btnRow->addWidget(m_openCliBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_installBtn);
    btnRow->addWidget(m_updateBtn);
    btnRow->addWidget(m_checkUpdateBtn);
    btnRow->addWidget(m_refreshEnvBtn);
    root->addLayout(btnRow);

    connect(m_startBtn, &QPushButton::clicked, this, &DshPanel::onStartClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &DshPanel::onStopClicked);
    connect(m_restartBtn, &QPushButton::clicked, this, &DshPanel::onRestartClicked);
    connect(m_openWebBtn, &QPushButton::clicked, this, &DshPanel::onOpenWebClicked);
    connect(m_openCliBtn, &QPushButton::clicked, this, &DshPanel::onOpenCliClicked);
    connect(m_installBtn, &QPushButton::clicked, this, &DshPanel::onInstallGlobalClicked);
    connect(m_updateBtn, &QPushButton::clicked, this, &DshPanel::onUpdateClicked);
    connect(m_checkUpdateBtn, &QPushButton::clicked, this, &DshPanel::onCheckUpdateClicked);
    connect(m_refreshEnvBtn, &QPushButton::clicked, this, &DshPanel::onRefreshEnvClicked);

    // --- 日志区 ---
    auto *logBox = new QGroupBox(tr("运行日志"), page);
    auto *logLay = new QVBoxLayout(logBox);
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000); // 限制行数，避免长时间运行占内存
    logLay->addWidget(m_logView);
    auto *logBtnRow = new QHBoxLayout();
    logBtnRow->addStretch();
    m_clearLogBtn = new QPushButton(tr("清空日志"), this);
    logBtnRow->addWidget(m_clearLogBtn);
    logLay->addLayout(logBtnRow);
    root->addWidget(logBox, 1); // 日志区吃剩余空间

    connect(m_clearLogBtn, &QPushButton::clicked, this, &DshPanel::onClearLogClicked);

    return page;
}

// ---------------------------------------------------------------------------
// 配置持久化（QSettings，org=CubeShell app=cube-shell）
// ---------------------------------------------------------------------------

void DshPanel::loadSettings()
{
    const QSettings s;
    m_hostEdit->setText(s.value(QStringLiteral("dsh/host"),
                                QLatin1String(DshManager::kDefaultHost)).toString());
    m_portSpin->setValue(s.value(QStringLiteral("dsh/port"),
                                 DshManager::kDefaultPort).toInt());
    m_lastCliDir = s.value(QStringLiteral("dsh/lastCliDir")).toString();
}

void DshPanel::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("dsh/host"), m_hostEdit->text().trimmed());
    s.setValue(QStringLiteral("dsh/port"), m_portSpin->value());
    s.setValue(QStringLiteral("dsh/lastCliDir"), m_lastCliDir);
}

// ---------------------------------------------------------------------------
// 环境检测
// ---------------------------------------------------------------------------

// 发起异步环境检测；结果到达时走 onEnvironmentDetected。
// 检测期间不清空已有结果（刷新时界面不闪回“未知”），只把提示置为检测中。
void DshPanel::refreshEnvironment()
{
    m_envDetecting = true;
    m_envLabel->setText(tr("正在检测 Node 环境…"));
    m_manager->detectEnvironmentAsync();
    refreshVersionUi();
}

void DshPanel::onEnvironmentDetected(const DshManager::Environment &env)
{
    m_envDetecting = false;
    m_envOk = env.canRun();
    m_dshInstalled = env.dshGlobalInstalled;
    m_dshVersion = env.dshVersion;
    if (!env.canRun()) {
        m_envLabel->setText(tr("未检测到 Node.js。请先安装 Node.js（自带 npm/npx）："
                               "https://nodejs.org"));
    } else {
        m_envLabel->setText(tr("Node %1 · npm %2")
                                .arg(env.nodeVersion.isEmpty() ? tr("未知") : env.nodeVersion,
                                     env.npmVersion.isEmpty() ? tr("未知") : env.npmVersion));
    }
    refreshVersionUi();
    updateStatusUi(); // 启动按钮可用性依赖 m_envOk
}

// 按 已装/最新 版本刷新版本行与安装/更新按钮。
void DshPanel::refreshVersionUi()
{
    // 检测未回来之前不要断言“缺 Node.js”——那时 m_envOk 只是还不知道。
    if (m_envDetecting) {
        m_versionLabel->setText(tr("检测中…"));
        m_installBtn->setEnabled(false);
        m_updateBtn->setEnabled(false);
        m_checkUpdateBtn->setEnabled(false);
        m_openCliBtn->setEnabled(false);
        m_startBtn->setEnabled(false);
        return;
    }

    // 版本行：已装版本 + 最新版本（若已查询）。
    QString installedText;
    if (!m_envOk)
        installedText = tr("不可用（缺 Node.js）");
    else if (m_dshInstalled)
        installedText = m_dshVersion.isEmpty() ? tr("已安装（版本未知）")
                                               : tr("已安装 %1").arg(m_dshVersion);
    else
        installedText = tr("未全局安装（将经 npx 运行最新版）");

    if (m_latestVersion.isEmpty()) {
        m_versionLabel->setText(installedText);
    } else {
        QString cmp;
        if (m_dshInstalled && !m_dshVersion.isEmpty()) {
            cmp = (m_dshVersion == m_latestVersion)
                      ? tr("已是最新")
                      : tr("可更新 → %1").arg(m_latestVersion);
        } else {
            cmp = tr("最新 %1").arg(m_latestVersion);
        }
        m_versionLabel->setText(tr("%1 · %2").arg(installedText, cmp));
    }

    // 安装按钮：仅当「能跑 npx + 未安装 + 无安装进行中」可用；已安装则禁用。
    const bool busy = m_manager->isInstalling();
    m_installBtn->setEnabled(m_envOk && !m_dshInstalled && !busy);
    m_installBtn->setText(m_dshInstalled ? tr("已安装") : tr("全局安装 dsh"));

    // 更新按钮：仅当「已安装 + 查到更新（版本不同）+ 无安装进行中」可用。
    const bool updateAvailable = m_dshInstalled && !m_latestVersion.isEmpty()
                                 && m_dshVersion != m_latestVersion;
    m_updateBtn->setEnabled(updateAvailable && !busy);
    // 检查更新按钮：能跑 npm 即可用。
    m_checkUpdateBtn->setEnabled(m_envOk);
    // 终端运行 CLI：能跑 npx/dsh 即可用。
    m_openCliBtn->setEnabled(m_envOk);

    // 启动按钮也依赖环境。
    if (!m_manager->isRunning())
        m_startBtn->setEnabled(m_envOk && m_manager->status() != DshManager::Status::Starting);
}

// ---------------------------------------------------------------------------
// 状态刷新
// ---------------------------------------------------------------------------

void DshPanel::updateStatusUi()
{
    using Status = DshManager::Status;
    const Status st = m_manager->status();

    QString color;
    QString text;
    switch (st) {
    case Status::Stopped:  color = QStringLiteral("#9e9e9e"); text = tr("未运行");   break;
    case Status::Starting: color = QStringLiteral("#fb8c00"); text = tr("启动中…"); break;
    case Status::Running:  color = QStringLiteral("#43a047"); text = tr("运行中");   break;
    case Status::Failed:   color = QStringLiteral("#e53935"); text = tr("失败");     break;
    }
    m_statusDot->setStyleSheet(
        QStringLiteral("background:%1;border-radius:6px;").arg(color));
    m_statusText->setText(text);

    const bool running = (st == Status::Starting || st == Status::Running);
    m_startBtn->setEnabled(!running && m_envOk && !m_envDetecting);
    m_stopBtn->setEnabled(running);
    m_restartBtn->setEnabled(running);
    m_openWebBtn->setEnabled(st == Status::Running);
    // 运行/启动期间锁定监听配置，避免与已起进程不一致。
    m_hostEdit->setEnabled(!running);
    m_portSpin->setEnabled(!running);

    if (st == Status::Running)
        m_urlLabel->setText(m_manager->webUrl());
    else if (st == Status::Stopped || st == Status::Failed) {
        m_pidLabel->setText(QStringLiteral("-"));
        if (st == Status::Stopped)
            m_urlLabel->setText(QStringLiteral("-"));
    }
}

// ---------------------------------------------------------------------------
// 操作按钮
// ---------------------------------------------------------------------------

void DshPanel::onStartClicked()
{
    saveSettings();
    m_manager->setListen(m_hostEdit->text(), m_portSpin->value());
    appendLog(tr("▶ 启动 dsh web（%1）…").arg(m_manager->webUrl()));
    m_manager->start();
}

void DshPanel::onStopClicked()
{
    appendLog(tr("■ 停止 dsh web…"));
    m_manager->stop();
}

void DshPanel::onRestartClicked()
{
    appendLog(tr("↻ 重启 dsh web…"));
    saveSettings();
    m_manager->setListen(m_hostEdit->text(), m_portSpin->value());
    m_manager->stop();
    m_manager->start();
}

void DshPanel::onOpenWebClicked()
{
    QDesktopServices::openUrl(QUrl(m_manager->webUrl()));
}

void DshPanel::onCliModeChanged(int)
{
    // tui 是交互式界面，不需要任务文本；headless 才需要。
    const bool isTui = (m_cliModeCombo->currentData().toString() == QStringLiteral("tui"));
    m_cliTaskEdit->setEnabled(!isTui);
    if (isTui) {
        m_cliTaskEdit->setPlaceholderText(
            tr("tui 为交互式界面，无需任务（需先 dsh plugin --profile tui add <包>）"));
    } else {
        m_cliTaskEdit->setPlaceholderText(tr("交给 dsh 的任务（仅 headless 需要）"));
    }
}

// 构造要在本机终端运行的 dsh CLI 命令。
// 终端走用户登录 shell 的 PATH，dsh/npx 正常解析（无需 GUI 那套路径兜底）。
// headless 会把多个词用空格拼接成任务，故任务文本不加引号、原样拼接。
QString DshPanel::buildCliCommand() const
{
    const QString base = m_dshInstalled
                             ? QStringLiteral("dsh")
                             : QStringLiteral("npx -y @deepseek-ai/dsh");
    const bool isTui = (m_cliModeCombo->currentData().toString() == QStringLiteral("tui"));
    if (isTui)
        return base + QStringLiteral(" --profile tui");
    const QString task = m_cliTaskEdit->text().trimmed();
    return base + QStringLiteral(" --profile headless ") + task;
}

void DshPanel::onOpenCliClicked()
{
    const bool isTui = (m_cliModeCombo->currentData().toString() == QStringLiteral("tui"));
    if (!isTui && m_cliTaskEdit->text().trimmed().isEmpty()) {
        QMessageBox::information(this, tr("DeepSeek Harness"),
                                 tr("请先在「CLI 任务」里填写要交给 dsh 的任务。"));
        return;
    }
    // 先选项目文件夹（与 Claude Code 的「打开终端」同一交互）。
    // 对 dsh 这一步是有语义的：工作目录即会话所属工作区——会话按 cwd 分组，
    // --continue 也只在当前工作区里找最近一条，agent 的文件操作同样以此为根。
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择项目文件夹"),
        m_lastCliDir.isEmpty() ? QDir::homePath() : m_lastCliDir);
    if (dir.isEmpty())
        return; // 用户取消
    m_lastCliDir = dir;
    saveSettings(); // 记住这次选择，下次对话框从这里起

    if (isTui) {
        appendLog(tr("提示：tui 非内置，未安装时终端会提示 "
                     "`dsh plugin --profile tui add <包>` 创建。"));
    }
    const QString command = buildCliCommand();
    appendLog(tr("▶ 终端运行 CLI（%1）：%2").arg(dir, command));
    emit openCliRequested(command, dir);
}

void DshPanel::onInstallGlobalClicked()
{
    m_installBtn->setEnabled(false);
    appendLog(tr("开始全局安装 dsh…"));
    m_manager->installGlobal();
}

void DshPanel::onUpdateClicked()
{
    m_updateBtn->setEnabled(false);
    appendLog(tr("开始更新 dsh 到最新版本…"));
    m_manager->updateGlobal();
}

void DshPanel::onCheckUpdateClicked()
{
    m_checkUpdateBtn->setEnabled(false);
    m_versionLabel->setText(tr("正在查询最新版本…"));
    m_manager->checkLatestVersion();
}

void DshPanel::onRefreshEnvClicked()
{
    refreshEnvironment();
    appendLog(tr("正在重新检测 Node 环境…"));
}

void DshPanel::onClearLogClicked()
{
    m_logView->clear();
}

// ---------------------------------------------------------------------------
// DshManager 信号
// ---------------------------------------------------------------------------

void DshPanel::onStatusChanged(DshManager::Status)
{
    updateStatusUi();
}

void DshPanel::onStarted(qint64 pid)
{
    m_pidLabel->setText(QString::number(pid));
}

void DshPanel::onStopped(int exitCode)
{
    appendLog(tr("进程已退出(code=%1)。").arg(exitCode));
    updateStatusUi();
}

void DshPanel::onWebReady(const QString &url)
{
    appendLog(tr("✓ Web 服务已就绪：%1（点「打开 Web 界面」在浏览器中访问）").arg(url));
    updateStatusUi();
}

void DshPanel::onLogLine(const QString &line)
{
    appendLog(line);
}

void DshPanel::onError(const QString &message)
{
    appendLog(tr("✗ %1").arg(message));
    updateStatusUi();
}

void DshPanel::onInstallLog(const QString &line)
{
    appendLog(line);
}

void DshPanel::onInstallFinished(bool ok, const QString &message)
{
    appendLog(ok ? tr("✓ %1").arg(message) : tr("✗ %1").arg(message));
    refreshEnvironment(); // 重新检测（dsh 安装状态/版本可能变了）
    // 安装/更新成功后再校准一次 latest，让“可更新”状态立刻收敛。
    if (ok)
        m_manager->checkLatestVersion();
}

void DshPanel::onLatestVersionChecked(bool ok, const QString &latestVersion)
{
    m_latestVersion = ok ? latestVersion : QString();
    if (ok) {
        if (m_dshInstalled && !m_dshVersion.isEmpty() && m_dshVersion != latestVersion)
            appendLog(tr("发现新版本：%1（当前 %2），可点「更新 dsh」。")
                          .arg(latestVersion, m_dshVersion));
        else
            appendLog(tr("最新版本：%1。").arg(latestVersion));
    } else {
        appendLog(tr("查询最新版本失败（网络或 npm registry 不可达）。"));
    }
    refreshVersionUi();
}

void DshPanel::appendLog(const QString &text)
{
    m_logView->appendPlainText(text);
}

} // namespace cubeshell
