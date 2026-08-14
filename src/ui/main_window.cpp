#include "main_window.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

#include "add_device_dialog.h"
#include "device_list_widget.h"
#include "local_file_browser_widget.h"
#include "sftp_browser_widget.h"
#include "ssh_session_tab.h"
#include "ssh_terminal_widget.h"
#include "status_box_item.h"
#include "terminal_tab_widget.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/AddTunnelDialog.h"
#include "dialogs/AiSettingsDialog.h"
#ifdef CUBESHELL_WITH_LOCALPROC
#include "dialogs/DockerManagerDialog.h"
#include "dialogs/DockerSoftDialog.h"
#endif
#include "dialogs/LinuxCommandsDialog.h"
#include "dialogs/ProcessManagerDialog.h"
#ifdef CUBESHELL_WITH_LOCALPROC
#include "dialogs/NatDialog.h"
#include "forwarder/FrpManager.h"
#endif
#include "dialogs/SettingsDialog.h"
#include "dialogs/TunnelConfigWidget.h"

#include "config/GlobalState.h"
#include "config/SecretMigration.h"
#include "ai/AiChatPanel.h"
#include "ai/CommandConfirmDialog.h"
#include "ai/ServerProfileBuilder.h"
#include "ai/AiChatWorker.h"
#include "ai/SshAiAgent.h"
#include "ai/TerminalExecutor.h"
#include "claude_code/ClaudeCodePanel.h"
#ifdef CUBESHELL_WITH_LOCALPROC
#include "docker/DockerManager.h"
#endif
#include "hermes/HermesPanel.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SshBridge.h"
#include "ssh/RemoteMonitor.h"
#include "ssh/TunnelPool.h"
#include "update/UpdateChecker.h"
#include "url_dispatch/BastionClient.h"
#ifdef CUBESHELL_WITH_RDP
#include "rdp/RdpPanel.h"
#include <QGuiApplication>
#include <QScreen>
#endif
#ifdef CUBESHELL_WITH_SERIAL
#include "serial_terminal_widget.h"
#include "dialogs/SerialConnectDialog.h"
#endif
// TCP/Telnet：无条件编译（Qt6::Network 是顶层必需组件，鸿蒙上同样可用）。
#include "net_terminal_widget.h"
#include "dialogs/NetConnectDialog.h"
#ifdef Q_OS_MACOS
#include "platform/FinderIntegration.h"
#endif
#ifdef Q_OS_WIN
#include "platform/WindowsIntegration.h"
#endif

#include "qtermwidget.h"
#include "terminal_theme_util.h"

namespace cubeshell {

namespace {

// Windows Terminal 风格标签页 QSS（主题自适应）。
// 参考样式：深色条形栏 + 圆角“悬浮”标签，激活标签比栏更亮、底部与终端内容
// 无缝衔接，非激活标签融入栏内、hover 时微微抬升。与 qdarktheme 默认的
// “透明选中 + 蓝色下划线”不同，这里改为实心浅色块 + 圆角。
// 颜色按 GlobalState 当前 appearance（dark/light）取值，切主题时重建。
QString windowsTerminalTabStyle()
{
    const bool light =
        GlobalState::instance().appearance().trimmed().compare(
            QStringLiteral("light"), Qt::CaseInsensitive) == 0;

    QString bar;        // 标签栏底色（与窗口一致，标签“浮”在上面）
    QString active;     // 激活标签实心色（比栏亮）
    QString hover;      // 非激活标签 hover 抬升色（介于栏与激活之间）
    QString textActive; // 激活标签文字
    QString textIdle;   // 非激活标签文字
    if (light) {
        bar = QStringLiteral("#F8F9FA");
        active = QStringLiteral("#FFFFFF");
        hover = QStringLiteral("#ECECEC");
        textActive = QStringLiteral("#1F2123");
        textIdle = QStringLiteral("#5F6368");
    } else {
        bar = QStringLiteral("#202124");
        active = QStringLiteral("#2B2D30");
        hover = QStringLiteral("#33373B");
        textActive = QStringLiteral("#E4E7EB");
        textIdle = QStringLiteral("#9AA0A6");
    }

    // 说明：
    //  - pane 去边框：激活标签底部与终端内容连为一体（Windows Terminal 的“连通”感）
    //  - tab 顶部圆角 6px、底部直角，上下 margin 让标签“浮”离栏底
    //  - 选中：实心浅色块、无边框/无下划线（覆盖 qdarktheme 的蓝色下划线）
    return QStringLiteral(
               "QTabWidget::pane {"
               "    border: none;"
               "    top: 0px;"
               "    background-color: %2;"
               "}"
               "QTabWidget::tab-bar { left: 0px; }"
               "QTabBar {"
               "    qproperty-drawBase: 0;"
               "    background-color: %1;"
               "    border: none;"
               "}"
               "QTabBar::tab:top {"
               "    background-color: transparent;"
               "    color: %5;"
               "    border: none;"
               "    border-top-left-radius: 6px;"
               "    border-top-right-radius: 6px;"
               "    border-bottom-left-radius: 0px;"
               "    border-bottom-right-radius: 0px;"
               "    padding: 4px 10px;"
               "    margin-top: 2px;"
               "    margin-bottom: 0px;"
               "    margin-left: 1px;"
               "    margin-right: 0px;"
               "}"
               "QTabBar::tab:top:selected {"
               "    background-color: %3;"
               "    color: %4;"
               "}"
               "QTabBar::tab:top:!selected:hover {"
               "    background-color: %3;"
               "}")
        .arg(bar, active, hover, textActive, textIdle);
}

// OHOS 为触屏设备：菜单项上显示快捷键文本（如 “主题设置  Ctrl+Shift+T”）
// 既是视觉噪音也没有键盘可用，统一不在菜单 QAction 上设置快捷键；
// 桌面平台（macOS/Windows/Linux）行为保持不变。
void setMenuShortcut(QAction *action, const QKeySequence &key)
{
#ifndef CUBESHELL_PLATFORM_OHOS
    action->setShortcut(key);
#else
    Q_UNUSED(action);
    Q_UNUSED(key);
#endif
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupMenus();
    setupToolbar();
    setupStatusBar();
    setupShortcuts();
    setupAiDock();
    setupTunnels();
    setupBastion();
    loadDevices();
}

MainWindow::~MainWindow()
{
    // 置位析构标志：~QWidget 删子对象时各 tab 的 destroyed lambda 会检查它，
    // 避免在 hash 销毁中状态下访问 m_aiAgents（退出时 hash 随本对象整体销毁）。
    m_destroying = true;
}

// ---------------------------------------------------------------------------
// UI 搭建
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py::MainWindow.__init__（主窗组装部分）
void MainWindow::setupUi()
{
    // Python 侧标题为空，窗口默认 1370x777。对应Python: ui/main.py
    setWindowTitle(QString());
    resize(1370, 777);

    m_deviceList = new DeviceListWidget(this);

    // 首个 pane 常驻（承载首页），后续 pane 由 splitTab 按需创建。
    TerminalTabWidget *firstPane = createPane();

    // 首页：固定在首个 pane 的 index 0，home 图标 + “首页”标题。
    // 对应Python: ShellTab.setTabText(indexOf(self.index), translate("首页"))
    //           + setTabIcon(0, ":icons8-home-48")
    m_homePage = createHomePage();
    firstPane->addTab(m_homePage, tr("首页"));
    firstPane->setTabIcon(0, QIcon(QStringLiteral(":/icons8-home-48.png")));
    firstPane->setCurrentIndex(0);

    // 分屏容器：顶层 splitter，子控件可以是 pane 或嵌套的子 splitter。
    // 嵌套让水平/垂直可以自由混排（如左侧一个终端，右侧上下再分两个）。
    m_termSplitter = new QSplitter(Qt::Horizontal, this);
    m_termSplitter->setChildrenCollapsible(false);
    m_termSplitter->addWidget(firstPane);

    // 左栏：未连接时显示设备列表；连接后文件浏览器替换设备列表，
    // 设备控件仅保留底部两个复选框（跟随终端目录/远程监控）。
    // 对应Python: 连接成功后左侧 treeWidget 改展示 SFTP/本地文件目录
    m_browserStack = new QStackedWidget(this);
    m_browserStack->setVisible(false);

    m_leftSplitter = new QSplitter(Qt::Vertical, this);
    m_leftSplitter->addWidget(m_browserStack);
    m_leftSplitter->addWidget(m_deviceList);
    m_leftSplitter->setStretchFactor(0, 1);
    m_leftSplitter->setStretchFactor(1, 0);
    m_leftSplitter->setChildrenCollapsible(false);

    m_splitter = new QSplitter(this);
    m_splitter->addWidget(m_leftSplitter);
    m_splitter->addWidget(m_termSplitter);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 1090});   // 对应Python: splitter_3.setSizes([280, 1090])
    setCentralWidget(m_splitter);

    connect(m_deviceList, &DeviceListWidget::activated,
            this, &MainWindow::openSshSession);
#ifdef CUBESHELL_WITH_LOCALPTY
    connect(m_deviceList, &DeviceListWidget::localTerminalRequested,
            this, &MainWindow::openLocalTerminal);
#endif
    connect(m_deviceList, &DeviceListWidget::addRequested,
            this, &MainWindow::addDevice);
    connect(m_deviceList, &DeviceListWidget::editRequested,
            this, &MainWindow::editDevice);
    connect(m_deviceList, &DeviceListWidget::removeRequested,
            this, &MainWindow::removeDevice);
    connect(m_deviceList, &DeviceListWidget::saveRequested,
            this, [this]() { saveDevices(); });
    // 对应Python: _on_follow_folder_changed（勾选时立即同步到终端 cwd）
    connect(m_deviceList, &DeviceListWidget::followFolderToggled,
            this, [this](bool on) {
        m_followFolder = on;
        if (on)
            syncBrowserToTerminalCwd();
    });
    // 对应Python: _on_remote_monitoring_changed（启/停当前会话的 RemoteMonitor）
    connect(m_deviceList, &DeviceListWidget::remoteMonitoringToggled,
            this, [this](bool on) {
        if (m_monitorTab && m_monitorTab->monitor()) {
            if (on)
                m_monitorTab->monitor()->start();
            else
                m_monitorTab->monitor()->stop();
        }
        if (!on)
            resetStatusItems();
    });

    // 焦点落到任一 pane 内部时把它记为活动 pane —— 多分屏下不能再靠
    // currentWidget()->hasFocus() 判断（终端内部的子控件持有焦点，
    // 且 AI/文件面板抢焦点后原判断会整体退回首个 pane）。
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *now) {
        if (!now)
            return;
        for (TerminalTabWidget *pane : std::as_const(m_panes)) {
            if (pane == now || pane->isAncestorOf(now)) {
                setActivePane(pane);
                return;
            }
        }
    });
}

// 新建一个分屏面板并接好全部信号。setupUi 的首个 pane 与 splitTab
// 新增的 pane 共用此入口，保证两者行为完全一致。
TerminalTabWidget *MainWindow::createPane()
{
    auto *tabs = new TerminalTabWidget(this);
    // 关闭按钮由 TabCloseButton 自己画，不用 Qt 原生的。
    tabs->setTabsClosable(false);
    tabs->setMovable(true);          // 拖拽排序
    // 不开 documentMode：Python 版 ShellTab 未设置（ui/main.py:107-112）。
    // macOS 上 documentMode 会让 QMacStyle 按系统外观强制标签文字颜色，
    // 深色主题下未选中标签被画成黑字（qdarktheme 的 QTabBar::tab 不设 color，
    // 依赖 QWidget 继承色），导致不可读。
    // Windows Terminal 风格标签：圆角悬浮标签，主题自适应（见上方 helper）。
    tabs->setStyleSheet(windowsTerminalTabStyle());
    tabs->tabBar()->setCursor(Qt::PointingHandCursor);
#ifdef CUBESHELL_WITH_LOCALPTY
    connect(tabs, &TerminalTabWidget::newLocalTerminalRequested,
            this, &MainWindow::openLocalTerminal);
#endif

    connect(tabs, &QTabWidget::tabCloseRequested, this,
            [this, tabs](int index) { closeTabIn(tabs, index); });
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](int) {
        // 切换标签即是对该 pane 的操作 —— 先把它记为活动 pane。
        // 不能只依赖 focusChanged：点击标签栏时 currentChanged 可能早于
        // focusChanged 发出，此时 m_activePane 还指着另一个分屏，
        // 下面几个 update* 就会去读错分屏的当前页（表现为在新分屏建终端后
        // 点首页，左栏设备列表不显示）。
        setActivePane(tabs);
        bindMonitorToTab(tabs->currentWidget());
        updateTerminalInfo();
        // 左侧文件浏览器跟随当前标签。对应Python: shell_tab_current_changed → refreshDirs
        updateLeftPanel(tabs);
        // AI 面板跟随当前 SSH 标签切换 Agent。对应Python: _connect_ai_to_current_tab
        if (m_aiDock && m_aiDock->isVisible())
            connectAiToCurrentTab();
    });
    // 标签右键菜单（关闭/分屏等）。
    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs->tabBar(), &QTabBar::customContextMenuRequested, this,
            [this, tabs](const QPoint &pos) { showTabContextMenu(tabs, pos); });

    m_panes.append(tabs);
    if (!m_activePane)
        m_activePane = tabs;
    return tabs;
}

// 首页：七条快捷键提示，水平/垂直居中。
// 对应Python: ui/main.py 的 self.index（gridLayout_2 内 7 个 QLabel）
QWidget *MainWindow::createHomePage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *box = new QWidget(page);
    auto *grid = new QVBoxLayout(box);
    grid->setContentsMargins(0, 117, 0, 117);   // 对应Python: setContentsMargins(0, 117, 0, 117)
    grid->setSpacing(0);

    // macOS 上 Qt 把 Ctrl 渲染为 Command，文案跟随平台。
#ifdef Q_OS_MACOS
    const QString mod = QStringLiteral("Command");
#else
    const QString mod = QStringLiteral("Ctrl");
#endif
    // 顺序与 Python 侧 gridLayout_2 的行号一致。
    const QList<QPair<QString, QString>> hints = {
        {tr("添加配置"), QStringLiteral("A")},
        {tr("添加隧道"), QStringLiteral("S")},
        {tr("帮助"),     QStringLiteral("P")},
        {tr("关于"),     QStringLiteral("B")},
        {tr("查找命令行"), QStringLiteral("C")},
        {tr("导入配置"), QStringLiteral("I")},
        {tr("导出配置"), QStringLiteral("E")},
    };
    for (const auto &hint : hints) {
        auto *label = new QLabel(QStringLiteral("%1 Shift+%2+%3")
                                     .arg(hint.first, mod, hint.second), box);
        grid->addWidget(label);
    }

    outer->addWidget(box, 0, Qt::AlignHCenter | Qt::AlignVCenter);
    return page;
}

// 对应Python: cube-shell.py 里给新建标签 setTabButton 的那两句
void MainWindow::decorateSessionTab(QTabWidget *tabs, int index)
{
    QTabBar *bar = tabs->tabBar();
    bar->setTabButton(index, QTabBar::LeftSide, new TabStatusDot(bar));

    auto *closeBtn = new TabCloseButton(bar);
    QWidget *page = tabs->widget(index);
    // 标签可拖拽排序，index 会变：关闭时按 page 反查当前位置。
    connect(closeBtn, &TabCloseButton::clicked, this, [this, tabs, page]() {
        const int i = tabs->indexOf(page);
        if (i >= 0)
            closeTabIn(tabs, i);
    });
    bar->setTabButton(index, QTabBar::RightSide, closeBtn);
}

void MainWindow::setTabConnected(QWidget *page, bool connected)
{
    for (QTabWidget *tabs : allPanes()) {
        const int i = tabs->indexOf(page);
        if (i < 0)
            continue;
        auto *dot = qobject_cast<TabStatusDot *>(
            tabs->tabBar()->tabButton(i, QTabBar::LeftSide));
        if (dot)
            dot->setConnected(connected);
        return;
    }
}

// 菜单栏。对应Python: cube-shell.py::menuBarController
void MainWindow::setupMenus()
{
    // --- 文件 ---
    QMenu *fileMenu = menuBar()->addMenu(tr("文件"));
    QAction *addDev = fileMenu->addAction(tr("&新增配置"), this, &MainWindow::addDevice);
    setMenuShortcut(addDev, QKeySequence(QStringLiteral("Shift+Ctrl+A")));   // 新增配置
    addDev->setStatusTip(tr("添加配置"));
    QAction *addTun = fileMenu->addAction(tr("&新增SSH隧道"), this, &MainWindow::addTunnel);
    setMenuShortcut(addTun, QKeySequence(QStringLiteral("Shift+Ctrl+S")));   // 新增SSH隧道
    addTun->setStatusTip(tr("新增SSH隧道"));
    fileMenu->addSeparator();
    QAction *expDev = fileMenu->addAction(tr("&导出设备配置"), this, &MainWindow::exportDevices);
    setMenuShortcut(expDev, QKeySequence(QStringLiteral("Shift+Ctrl+E")));   // 导出设备配置
    expDev->setStatusTip(tr("导出设备配置"));
    QAction *impDev = fileMenu->addAction(tr("&导入设备配置"), this, &MainWindow::importDevices);
    setMenuShortcut(impDev, QKeySequence(QStringLiteral("Shift+Ctrl+I")));   // 导入设备配置
    impDev->setStatusTip(tr("导入设备配置"));
    fileMenu->addSeparator();
    QAction *openCfg = fileMenu->addAction(tr("打开 config.dat…"));
    connect(openCfg, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("打开 config.dat"), QDir::homePath(), QStringLiteral("*.dat;;*"));
        if (!path.isEmpty()) {
            QString err;
            if (m_store.load(path, &err)) {
                m_configPath = path;
                m_jsonPath = QFileInfo(path).absolutePath() + QStringLiteral("/devices.json");
                // 手动打开的 config.dat 里必然是明文密码，同样要迁移。
                migrateSecrets();
                refreshDeviceList();
            } else {
                QMessageBox::warning(this, tr("加载失败"), err);
            }
        }
    });
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(tr("退出"));
    setMenuShortcut(quit, QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // --- 编辑（作用于当前终端） ---
    QMenu *editMenu = menuBar()->addMenu(tr("编辑"));
    QAction *copy = editMenu->addAction(tr("复制"));
#ifdef Q_OS_MACOS
    setMenuShortcut(copy, QKeySequence::Copy);
#else
    setMenuShortcut(copy, QKeySequence(QStringLiteral("Ctrl+Shift+C")));
#endif
    connect(copy, &QAction::triggered, this, [this]() {
        QWidget *w = activeTabWidget() ? activeTabWidget()->currentWidget() : nullptr;
        QTermWidget *term = qobject_cast<QTermWidget *>(w);
        if (!term && w)
            term = w->findChild<QTermWidget *>();
        if (term && !term->selectedText().isEmpty())
            term->copyClipboard();
    });
    QAction *paste = editMenu->addAction(tr("粘贴"));
#ifdef Q_OS_MACOS
    setMenuShortcut(paste, QKeySequence::Paste);
#else
    setMenuShortcut(paste, QKeySequence(QStringLiteral("Ctrl+Shift+V")));
#endif
    connect(paste, &QAction::triggered, this, [this]() {
        QWidget *w = activeTabWidget() ? activeTabWidget()->currentWidget() : nullptr;
        QTermWidget *term = qobject_cast<QTermWidget *>(w);
        if (!term && w)
            term = w->findChild<QTermWidget *>();
        if (term)
            term->pasteClipboard();
    });

    editMenu->addSeparator();

    // --- 查找（终端内容检索：排查线上问题时按关键字定位日志）---
    QAction *find = editMenu->addAction(tr("查找"));
#ifdef Q_OS_MACOS
    setMenuShortcut(find, QKeySequence::Find);                              // Cmd+F
#else
    // 非 macOS 不能用 Ctrl+F：readline(emacs 模式) 的 forward-char、vim 的翻页
    // 都占着它，抢过来会把终端里的常用操作弄坏。跟复制/粘贴一样加 Shift。
    setMenuShortcut(find, QKeySequence(QStringLiteral("Ctrl+Shift+F")));
#endif
    connect(find, &QAction::triggered, this, [this]() {
        if (QTermWidget *term = currentTerminal()) {
            // 有选中内容就直接拿来当关键字。
            if (term->selectedText(false).trimmed().isEmpty())
                term->showSearchBar();
            else
                term->searchSelectedText();
        }
    });

    QAction *findNext = editMenu->addAction(tr("查找下一个"));
    QAction *findPrev = editMenu->addAction(tr("查找上一个"));
#ifdef Q_OS_MACOS
    // macOS 上 Cmd 组合键不会下发给终端，可以安全占用。
    setMenuShortcut(findNext, QKeySequence::FindNext);                      // Cmd+G
    setMenuShortcut(findPrev, QKeySequence::FindPrevious);                  // Cmd+Shift+G
#else
    // 其他平台 Qt 的 FindNext/FindPrevious 默认是 F3/Shift+F3，而 F3 被 mc 之类
    // 的终端程序占用，全局抢走会影响正常使用。这里只留菜单项；搜索栏内部仍支持
    // Enter / Shift+Enter / F3 导航（不影响终端，因为焦点在输入框上）。
#endif
    connect(findNext, &QAction::triggered, this, [this]() {
        if (QTermWidget *term = currentTerminal())
            term->findNextMatch();
    });
    connect(findPrev, &QAction::triggered, this, [this]() {
        if (QTermWidget *term = currentTerminal())
            term->findPreviousMatch();
    });

    // --- 视图 ---
    QMenu *viewMenu = menuBar()->addMenu(tr("视图"));
    QAction *toggleDevices = viewMenu->addAction(tr("设备列表"));
    toggleDevices->setCheckable(true);
    toggleDevices->setChecked(true);
    connect(toggleDevices, &QAction::toggled, m_deviceList, &QWidget::setVisible);
    viewMenu->addSeparator();
    // 分屏：每次调用都新建一个 pane，可无限次分下去（水平/垂直可混排）。
    QAction *splitH = viewMenu->addAction(tr("水平分屏"), this, [this]() {
        QTabWidget *tabs = activeTabWidget();
        if (tabs)
            splitTab(tabs, tabs->currentIndex(), Qt::Horizontal);
    });
    setMenuShortcut(splitH, QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    QAction *splitV = viewMenu->addAction(tr("垂直分屏"), this, [this]() {
        QTabWidget *tabs = activeTabWidget();
        if (tabs)
            splitTab(tabs, tabs->currentIndex(), Qt::Vertical);
    });
    setMenuShortcut(splitV, QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    viewMenu->addSeparator();
    // 分屏之间切换焦点 + 尺寸/合并管理。
    QAction *nextPane = viewMenu->addAction(tr("下一个分屏"), this,
                                            [this]() { focusNextPane(1); });
    setMenuShortcut(nextPane, QKeySequence(QStringLiteral("Ctrl+Alt+Right")));
    QAction *prevPane = viewMenu->addAction(tr("上一个分屏"), this,
                                            [this]() { focusNextPane(-1); });
    setMenuShortcut(prevPane, QKeySequence(QStringLiteral("Ctrl+Alt+Left")));
    QAction *closePane = viewMenu->addAction(tr("关闭当前分屏"), this, [this]() {
        QTabWidget *tabs = activeTabWidget();
        if (!tabs || m_panes.count() <= 1)
            return;   // 只剩一个分屏时不可关闭
        // 把该 pane 的标签全部搬回首个 pane，再由 pruneEmptyPanes 摘除空壳。
        TerminalTabWidget *first = m_panes.first();
        if (tabs == first)
            return;
        while (tabs->count() > 0) {
            const QString title = tabs->tabText(0);
            QWidget *w = tabs->widget(0);
            tabs->removeTab(0);
            const int idx = first->addTab(w, title);
            decorateSessionTab(first, idx);
        }
        pruneEmptyPanes();
        first->setFocus();
    });
    setMenuShortcut(closePane, QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    viewMenu->addAction(tr("均分所有分屏"), this, [this]() {
        // 递归均分整棵 splitter 树。
        std::function<void(QSplitter *)> equalizeTree = [&](QSplitter *sp) {
            equalizeSplitter(sp);
            for (int i = 0; i < sp->count(); ++i) {
                if (auto *child = qobject_cast<QSplitter *>(sp->widget(i)))
                    equalizeTree(child);
            }
        };
        equalizeTree(m_termSplitter);
    });
    viewMenu->addAction(tr("合并所有分屏"), this, &MainWindow::mergeAllPanes);

    // --- 终端 ---
    QMenu *termMenu = menuBar()->addMenu(tr("终端"));
#ifdef CUBESHELL_WITH_LOCALPTY
    QAction *newTerm = termMenu->addAction(tr("新建本机终端"), this, &MainWindow::openLocalTerminal);
    setMenuShortcut(newTerm, QKeySequence(QStringLiteral("Ctrl+T")));
#endif
#ifdef CUBESHELL_WITH_RDP
    // 新建空白 RDP 面板，连接参数由用户在表单中填写后点“连接”。
    // 对应Python: 设备协议为 rdp 时走 open_rdp_tab；C++ 侧另提供手动入口
    termMenu->addAction(tr("新建 RDP 连接"), this,
                        [this]() { openRdpTab(RdpSettings()); });
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // 新建串口连接：先弹对话框选端口和帧格式，确定后开标签页立即建连。
    // 与 RDP 不同——串口参数少且必填端口，先问一次比开个空面板更顺手。
    termMenu->addAction(tr("新建串口连接"), this, [this]() {
        SerialConnectDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        openSerialTab(dlg.settings());
    });
#endif
    // 新建 Telnet / TCP 连接：与串口同理，参数少且主机必填，先问一次
    // 比开个空面板更顺手。不带 #ifdef——TCP/Telnet 在所有构建里都可用。
    termMenu->addAction(tr("新建 Telnet 连接"), this, [this]() {
        NetConnectDialog dlg(QStringLiteral("telnet"), this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        openNetTab(dlg.settings());
    });
    termMenu->addAction(tr("新建 TCP 连接"), this, [this]() {
        NetConnectDialog dlg(QStringLiteral("tcp"), this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        openNetTab(dlg.settings());
    });
    QAction *closeTab = termMenu->addAction(tr("关闭标签页"), this, &MainWindow::closeCurrentTab);
    setMenuShortcut(closeTab, QKeySequence(QStringLiteral("Ctrl+W")));
    termMenu->addSeparator();
    QAction *next = termMenu->addAction(tr("下一个标签页"), this, &MainWindow::nextTab);
    setMenuShortcut(next, QKeySequence(QStringLiteral("Ctrl+Tab")));
    QAction *prev = termMenu->addAction(tr("上一个标签页"), this, &MainWindow::prevTab);
    setMenuShortcut(prev, QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));

    // --- 工具 ---
    QMenu *toolsMenu = menuBar()->addMenu(tr("工具"));
    QAction *tunnels = toolsMenu->addAction(tr("SSH 隧道管理"), this, &MainWindow::showTunnelManager);
    Q_UNUSED(tunnels);
    toolsMenu->addSeparator();
    // 对应Python: setupLeftToolbar 里的 hermes / Claude Code 入口（菜单侧）
    toolsMenu->addAction(QStringLiteral("Hermes Agent"), this, &MainWindow::showHermesPanel);
    toolsMenu->addAction(QStringLiteral("Claude Code"), this, &MainWindow::showClaudeCodePanel);

    // --- 设置 --- 对应Python: menuBarController 的 setting_menu（L2227 + L2260-2292）
    QMenu *settingsMenu = menuBar()->addMenu(tr("设置"));
    // 主题设置 Shift+Ctrl+T：打开 SettingsDialog（主题 Tab）。
    QAction *settings = settingsMenu->addAction(tr("&主题设置"), this, [this] { showSettings(0); });
    setMenuShortcut(settings, QKeySequence(QStringLiteral("Shift+Ctrl+T")));   // 主题设置
    settings->setStatusTip(tr("设置主题"));
    settings->setMenuRole(QAction::PreferencesRole);
    // AI 设置（无快捷键）。对应Python: cube-shell.py L2267-2270
    QAction *aiSettings = settingsMenu->addAction(tr("&AI 设置"), this,
                                                 &MainWindow::showAiSettings);
    aiSettings->setStatusTip(tr("配置 GLM-4.7 AI 能力"));
    // 通用设置 Shift+Ctrl+G：同一个 SettingsDialog（通用 Tab），
    // 内含字体/编码/SSH 超时/回滚行数等，语言设置也在该对话框的语言 Tab。
    QAction *general = settingsMenu->addAction(tr("&通用设置"), this, [this] { showSettings(2); });
    setMenuShortcut(general, QKeySequence(QStringLiteral("Shift+Ctrl+G")));    // 通用设置
    general->setStatusTip(tr("字体、编码、超时等通用设置"));
    // 平台右键菜单集成（按当前平台只编译/显示对应项）。
    // 对应Python: cube-shell.py L2279-2292（platform.system() 分支）
#ifdef Q_OS_MACOS
    settingsMenu->addSeparator();
    QAction *finderInteg = settingsMenu->addAction(
        tr("Finder 右键菜单集成"), this, &MainWindow::showFinderIntegration);
    finderInteg->setStatusTip(tr("安装或卸载 Finder 右键菜单快速操作"));
#endif
#ifdef Q_OS_WIN
    settingsMenu->addSeparator();
    QAction *winInteg = settingsMenu->addAction(
        tr("Windows 右键菜单集成"), this, &MainWindow::showWindowsIntegration);
    winInteg->setStatusTip(tr("安装或卸载 Windows 右键菜单"));
#endif

    // --- 帮助 ---
    QMenu *helpMenu = menuBar()->addMenu(tr("帮助"));
    QAction *about = helpMenu->addAction(tr("&关于"), this, &MainWindow::showAbout);
    setMenuShortcut(about, QKeySequence(QStringLiteral("Shift+Ctrl+B")));
    about->setStatusTip(tr("cubeShell 有关信息"));
    about->setMenuRole(QAction::NoRole);    // 对应Python: about_action.setMenuRole(NoRole)
    QAction *update = helpMenu->addAction(tr("&检查更新"), this, &MainWindow::checkForUpdates);
    setMenuShortcut(update, QKeySequence(QStringLiteral("Shift+Ctrl+U")));
    update->setStatusTip(tr("检查并安装 cube-shell 最新版本"));
    update->setMenuRole(QAction::NoRole);
    // 对应Python: linux_action（Linux常用命令 Shift+Ctrl+P，L2319-2324）
    QAction *linuxCmds = helpMenu->addAction(tr("&Linux常用命令"), this, &MainWindow::showLinuxCommands);
    setMenuShortcut(linuxCmds, QKeySequence(QStringLiteral("Shift+Ctrl+P")));
    linuxCmds->setStatusTip(tr("最常用的Linux命令查找"));
    QAction *help = helpMenu->addAction(tr("&帮助"));
    setMenuShortcut(help, QKeySequence(QStringLiteral("Shift+Ctrl+H")));
    help->setStatusTip(tr("cubeShell使用说明"));
    connect(help, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/BestZYQ/cube-shell")));
    });
}

// 左侧竖向图标工具栏，风格参考 MobaXterm。
// 对应Python: cube-shell.py::setupLeftToolbar L838-870
void MainWindow::setupToolbar()
{
    auto *bar = new QToolBar(tr("工具栏"), this);
    bar->setObjectName(QStringLiteral("leftIconToolbar"));
    bar->setOrientation(Qt::Vertical);
    bar->setMovable(false);
    bar->setIconSize(QSize(24, 24));
    // 只显图标，不带文字。对应Python: Qt.ToolButtonIconOnly
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    addToolBar(Qt::LeftToolBarArea, bar);

    // 图标来自 cpp 项目自带的 resources/icons/icons.qrc（编译进二进制）。
    // label 为菜单/溢出区文字，tooltip 为悬浮提示，与 Python 侧一一对应。
    auto addTool = [this, bar](const QString &iconPath, const QString &label,
                               const QString &tooltip, void (MainWindow::*slot)()) {
        QAction *act = bar->addAction(QIcon(iconPath), label);
        act->setToolTip(tooltip);
        connect(act, &QAction::triggered, this, slot);
        return act;
    };

#ifdef CUBESHELL_WITH_LOCALPROC
    // Docker/常用容器依赖本机或远程 docker CLI 子进程驱动（DockerManager）；
    // 鸿蒙沙箱禁止 exec，入口摘除。走 SSH 管理远程 Docker 的纯数据层不受影响。
    addTool(QStringLiteral(":/icons8-docker-48.png"), tr("Docker 管理"),
            tr("Docker 容器管理"), &MainWindow::showDockerManager);
    addTool(QStringLiteral(":/icons8-container-48.png"), tr("常用容器"),
            tr("常用容器安装"), &MainWindow::showDockerSoft);
#endif
    addTool(QStringLiteral(":/tunnel-diode.png"), tr("SSH 隧道"),
            tr("SSH 隧道管理"), &MainWindow::showTunnelManager);
#ifdef CUBESHELL_WITH_LOCALPROC
    // 内网穿透（frp）需要下载并运行 frpc/frps 本地二进制，鸿蒙摘除。
    addTool(QStringLiteral(":/icons8-nat-48.png"), tr("内网穿透"),
            tr("内网穿透设置"), &MainWindow::showNatDialog);
#endif
    addTool(QStringLiteral(":/icons8-processor-48.png"), tr("进程管理"),
            tr("远程进程管理"), &MainWindow::showProcessManager);
    addTool(QStringLiteral(":/icons8-hermes-48.png"), QStringLiteral("Hermes Agent"),
            QStringLiteral("Hermes Agent"), &MainWindow::showHermesPanel);
    addTool(QStringLiteral(":/icons8-claudecode-48.png"), QStringLiteral("Claude Code"),
            QStringLiteral("Claude Code"), &MainWindow::showClaudeCodePanel);
}

// 状态栏：连接状态 + 终端大小/编码 + 8 项监控指标小方块。
// 对应Python: cube-shell.py::setupStatusBar L871-896
void MainWindow::setupStatusBar()
{
    m_statusBar = new QLabel(this);
    m_termSizeLabel = new QLabel(this);
    m_encodingLabel = new QLabel(QStringLiteral("UTF-8"), this);
    statusBar()->addPermanentWidget(m_statusBar, 1);
    statusBar()->addPermanentWidget(m_termSizeLabel);
    statusBar()->addPermanentWidget(m_encodingLabel);

    // 参数: (图标背景色, 图标字符, 初始文字, objectName) —— 与 Python 侧一致。
    auto makeItem = [this](const char *color, const QString &iconChar,
                           const QString &initText, const char *objName) {
        auto *item = new StatusBoxItem(QLatin1String(color), iconChar, initText, this);
        item->setObjectName(QLatin1String(objName));
        item->setVisible(false);   // 未连接时隐藏，对应Python: sb.hide()
        statusBar()->addPermanentWidget(item);
        return item;
    };

    m_statusHostname = makeItem("#c0392b", QStringLiteral("IP"), QStringLiteral("—"), "status_hostname");
    m_statusCpu      = makeItem("#27ae60", QStringLiteral("C"), QStringLiteral("CPU: —"), "status_cpu");
    m_statusMem      = makeItem("#e67e22", QStringLiteral("M"), QStringLiteral("MEM: —"), "status_mem");
    m_statusUpload   = makeItem("#16a085", QStringLiteral("↑"), QStringLiteral("— Mb/s"), "status_upload");
    m_statusDownload = makeItem("#2980b9", QStringLiteral("↓"), QStringLiteral("— Mb/s"), "status_download");
    m_statusUptime   = makeItem("#8e44ad", QStringLiteral("T"), QStringLiteral("—"), "status_uptime");
    m_statusUser     = makeItem("#2980b9", QStringLiteral("U"), QStringLiteral("—"), "status_user");
    m_statusDisk     = makeItem("#636e72", QStringLiteral("D"), QStringLiteral("/: —%"), "status_disk");

    statusBar()->setObjectName(QStringLiteral("bottomStatusBar"));
    statusBar()->setSizeGripEnabled(false);
    // 注意：状态栏本身常驻可见（还承载 setStatus 业务提示，与 Python 版不同），
    // 监控区显隐仅通过上面 8 个 StatusBoxItem 的 setVisible 控制。
}

// 速度格式化：字节/秒 → 动态单位（MB/s / KB/s / B/s，阈值 1024）。
// 对应Python: function/util.py::format_speed（L487-493）
static QString formatSpeed(double speed)
{
    if (speed >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB/s").arg(speed / (1024.0 * 1024.0), 0, 'f', 2);
    if (speed >= 1024.0)
        return QStringLiteral("%1 KB/s").arg(speed / 1024.0, 0, 'f', 2);
    return QStringLiteral("%1 B/s").arg(speed, 0, 'f', 0);
}

// 快捷键绑定汇总（菜单未覆盖的部分）。
void MainWindow::setupShortcuts()
{
    // Ctrl+T/W/Tab 等已绑定在菜单 QAction 上（见 setupMenus），此处无需重复。
}

// AI 助手停靠面板：右侧停靠、默认隐藏，Ctrl+Shift+K 切换显示/隐藏。
// 对应Python: cube-shell.py L761-771（ai_dock + ai_shortcut）
void MainWindow::setupAiDock()
{
    m_aiDock = new QDockWidget(tr("AI 助手"), this);
    m_aiDock->setObjectName(QStringLiteral("ai_dock"));
    m_aiPanel = new AiChatPanel(m_aiDock);
    m_aiDock->setWidget(m_aiPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_aiDock);
    m_aiDock->setVisible(false);   // 默认隐藏

    // Ctrl+Shift+K 切换 AI 面板显示/隐藏 (K=Knowledge AI, 避免与已有快捷键冲突)
    auto *aiShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")), this);
    connect(aiShortcut, &QShortcut::activated, this, &MainWindow::toggleAiPanel);

    // Panel → MainWindow 路由槽。对应Python: cube-shell.py L5605-5640
    connect(m_aiPanel, &AiChatPanel::userMessageSent,
            this, &MainWindow::onAiUserMessage);
    connect(m_aiPanel, &AiChatPanel::stopRequested,
            this, &MainWindow::onAiStopRequested);
    connect(m_aiPanel, &AiChatPanel::clearRequested,
            this, &MainWindow::onAiClearRequested);
    connect(m_aiPanel, &AiChatPanel::chatModeChanged,
            this, &MainWindow::onAiChatModeChanged);
    connect(m_aiPanel, &AiChatPanel::commandExecuteRequested,
            this, &MainWindow::onAiCommandExecuteRequested);
}

// 对应Python: cube-shell.py::_toggle_ai_panel
void MainWindow::toggleAiPanel()
{
    const bool show = !m_aiDock->isVisible();
    m_aiDock->setVisible(show);
    // 显示时立即绑定当前会话，面板顶部状态栏才能显示已连接主机。
    if (show)
        connectAiToCurrentTab();
}

// AI 面板绑定到当前活动的 SSH 会话（Agent 路由核心）。
// 对应Python: cube-shell.py::_connect_ai_to_current_tab（行 5639-5695）
void MainWindow::connectAiToCurrentTab()
{
    auto *session = qobject_cast<SshSessionTab *>(activeTabWidget()->currentWidget());

    // 无活动 SSH 会话 → 断开旧 agent，面板置为未连接。
    if (!session || !session->terminal()) {
        if (m_activeAiAgent) {
            disconnectAiFromAgent(m_activeAiAgent);
            m_activeAiAgent = nullptr;
        }
        m_aiPanel->setStatus(false);
        return;
    }

    // 查找或懒建该会话的 SshAiAgent。
    SshAiAgent *agent = m_aiAgents.value(session, nullptr);
    if (!agent) {
        // 未连上（无 SshClient）时不建 agent，等下次切换/发送再试。
        auto client = session->terminal()->sshClient();
        if (!client) {
            if (m_activeAiAgent) {
                disconnectAiFromAgent(m_activeAiAgent);
                m_activeAiAgent = nullptr;
            }
            m_aiPanel->setStatus(false);
            return;
        }
        // CommandExecutor 会话内复用（objectName "aiExecutor"），
        // parent 为会话标签 → 随标签销毁。对应Python: 复用 ssh_client
        auto *executor = session->findChild<CommandExecutor *>(
            QStringLiteral("aiExecutor"));
        if (!executor) {
            executor = new CommandExecutor(client.get(), session);
            executor->setObjectName(QStringLiteral("aiExecutor"));
        }
        agent = new SshAiAgent(executor, AiPreferences::load(), session);
        m_aiAgents.insert(session, agent);
        // 绑定终端以支持 AI 交互式命令执行（哨兵机制）。
        // 对应Python: _TerminalExecutor 绑定到活动 SSH 终端
        if (session->terminal()->terminal())
            agent->setTerminal(session->terminal()->terminal());
        // 哨兵检测需要未过滤数据，故接 SshBridge 的原始数据信号。
        //（bridge() 在连接建立前为空，此处 client 已存在所以常规不为空）
        if (session->terminal()->bridge()) {
            connect(session->terminal()->bridge(), &SshBridge::rawDataForAi,
                    agent->terminalExecutor(), &TerminalExecutor::onRawData);
        }
        // 会话若在 closeTabIn 之外被销毁（应用退出等），同步摘掉哈希项，
        // 避免悬垂键；agent 作为子对象随之析构，m_activeAiAgent 由 QPointer 置空。
        // m_destroying 守卫：MainWindow 析构删子对象时本 lambda 仍会被 destroyed
        // 触发，但此刻 m_aiAgents 已处销毁中状态，remove 会 UAF——直接返回，
        // 退出时 hash 随 MainWindow 整体销毁，无需逐项 remove。
        connect(session, &QObject::destroyed, this,
                [this, session]() {
                    if (m_destroying)
                        return;
                    m_aiAgents.remove(session);
                });

        // 异步构建服务器画像注入系统提示词。Builder parent 为 agent → 随之销毁。
        // 对应Python: ServerProfile 首次 AI 交互时异步构建
        auto *profiler = new ServerProfileBuilder(executor, agent);
        connect(profiler, &ServerProfileBuilder::profileReady,
                agent, &SshAiAgent::setServerProfile);
        profiler->buildAsync();
    }

    if (agent == m_activeAiAgent)
        return;   // 已是当前 agent，无需重接

    if (m_activeAiAgent)
        disconnectAiFromAgent(m_activeAiAgent);

    // Agent → Panel 信号路由。对应Python: _connect_ai_to_current_tab 的各 connect
    connect(agent, &SshAiAgent::aiMessage, m_aiPanel, &AiChatPanel::appendAiDelta);
    // 以 m_aiPanel 为 context，disconnectAiFromAgent 时可一并断开。
    // 捕获发出信号的 agent（QPointer 防悬垂）：确认框弹窗期间用户可能
    // 切换标签页导致 m_activeAiAgent 变更，命令必须回到产生它的 agent 执行。
    // 对应Python: agent.command_ready.connect(self._show_confirm_dialog)
    QPointer<SshAiAgent> agentGuard(agent);
    connect(agent, &SshAiAgent::commandReady, m_aiPanel,
            [this, agentGuard](const QList<AiCommand> &commands) {
        onAiCommandReady(commands, agentGuard.data());
    });
    connect(agent, &SshAiAgent::executionStarted, m_aiPanel,
            [this](const QString &cmd) { m_aiPanel->setExecuting(true, cmd); });
    connect(agent, &SshAiAgent::executionFinished, m_aiPanel,
            [this](const AiCommandResult &result) {
        // 失败命令的诊断信息通常只在 stderr，回落展示避免结果卡片空白。
        const QString output = result.stdoutText.isEmpty() ? result.stderrText
                                                           : result.stdoutText;
        m_aiPanel->appendExecutionResult(result.cmd, result.exitCode, output,
                                         result.description);
        // 复位执行中状态。executionFinished 是逐条信号，与 Python 版同义
        //（下一条命令的 executionStarted/Progress 会重新置 true）。
        // 对应Python: _on_execution_finished 末尾 set_executing(False)
        m_aiPanel->setExecuting(false);
    });
    connect(agent, &SshAiAgent::executionProgress, m_aiPanel,
            [this](int current, int total) {
        m_aiPanel->setExecuting(true, QString(), current, total);
    });
    connect(agent, &SshAiAgent::thinkingStarted, m_aiPanel,
            [this]() { m_aiPanel->setThinking(true); });
    connect(agent, &SshAiAgent::thinkingFinished, m_aiPanel,
            [this]() { m_aiPanel->setThinking(false); });
    connect(agent, &SshAiAgent::errorOccurred, m_aiPanel,
            [this](const QString &message) {
        // 对应Python: L5696 append_ai_message(f"❌ 错误: {msg}")
        m_aiPanel->appendAiMessage(tr("❌ 错误: %1").arg(message));
    });
    connect(agent, &SshAiAgent::taskSummary, m_aiPanel, &AiChatPanel::appendTaskSummary);
    connect(agent, &SshAiAgent::diagnosingStarted, m_aiPanel,
            &AiChatPanel::appendDiagnosingHint);
    connect(agent, &SshAiAgent::commandOutput, m_aiPanel,
            &AiChatPanel::updateCommandOutput);

    m_activeAiAgent = agent;

    // 面板顶部状态栏：已连接 + 主机名 + 模型名。
    m_aiPanel->setStatus(true, session->device().hostPort().host);
    m_aiPanel->refreshModelLabel();
}

// 断开 agent 到 Panel 的所有信号连接（含 lambda 连接，以 m_aiPanel 为 context）。
void MainWindow::disconnectAiFromAgent(SshAiAgent *agent)
{
    if (!agent)
        return;
    disconnect(agent, nullptr, m_aiPanel, nullptr);
}

// 用户发送消息 → 按 ChatMode 路由到对应后端。
// 对应Python: cube-shell.py::_on_ai_user_message
// 注意：用户气泡已由 AiChatPanel::onSend 自行插入，此处不可重复 append。
void MainWindow::onAiUserMessage(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;

    if (m_aiPanel->chatMode() == AiChatPanel::ChatMode::SshAgent) {
        // SSH 代理模式：路由到当前活动 SshAiAgent（必要时先绑定当前标签）。
        if (!m_activeAiAgent)
            connectAiToCurrentTab();
        if (!m_activeAiAgent) {
            m_aiPanel->appendAiMessage(
                tr("⚠️ 未连接 SSH 服务器，请先连接一个远程会话。"));
            return;
        }
        m_activeAiAgent->processUserInput(text);
        return;
    }

    // 普通聊天模式：无工具的纯文本对话，直接走 AiChatWorker。
    if (!m_plainChatWorker) {
        m_plainChatWorker = new AiChatWorker(this);
        // deltaReceived(content, reasoning) 与 appendAiDelta(reasoning, content)
        // 参数顺序相反 — 必须经 lambda 换序，否则思考过程与正文互换。
        connect(m_plainChatWorker, &AiChatWorker::deltaReceived, m_aiPanel,
                [this](const QString &content, const QString &reasoning) {
            m_aiPanel->appendAiDelta(reasoning, content);
        });
        // setThinking(false) 内部会 finalizeAiRender，冲刷节流中的最后一帧。
        connect(m_plainChatWorker, &AiChatWorker::finishedText, m_aiPanel,
                [this](const QString &) { m_aiPanel->setThinking(false); });
        connect(m_plainChatWorker, &AiChatWorker::failed, m_aiPanel,
                [this](const QString &message) {
            m_aiPanel->setThinking(false);
            m_aiPanel->appendAiMessage(message);
        });
    }

    const AiPreferences prefs = AiPreferences::load();
    m_plainChatWorker->setPreferences(prefs);
    QJsonArray messages;
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("content"), prefs.systemPrompt},
    });
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), text},
    });
    m_aiPanel->setThinking(true);
    m_plainChatWorker->start(messages);
}

// 上下文模式切换：切到 SSH 代理时立即绑定当前标签。
void MainWindow::onAiChatModeChanged(AiChatPanel::ChatMode mode)
{
    if (mode == AiChatPanel::ChatMode::SshAgent) {
        connectAiToCurrentTab();
    } else {
        // 普通聊天不依附会话 — 断开并清理当前 agent，
        // 否则切回 SSH 代理时 connectAiToCurrentTab 会因指针未清空而提前返回。
        if (m_activeAiAgent) {
            disconnectAiFromAgent(m_activeAiAgent);
            m_activeAiAgent = nullptr;
        }
        m_aiPanel->setStatus(false);
    }
}

// 停止当前 AI 请求 / 命令执行。对应Python: AI 面板停止按钮
void MainWindow::onAiStopRequested()
{
    if (m_activeAiAgent)
        m_activeAiAgent->stop();
    if (m_plainChatWorker && m_plainChatWorker->isRunning())
        m_plainChatWorker->stop();
    m_aiPanel->setThinking(false);
    m_aiPanel->setExecuting(false);
}

// 清空对话历史（面板侧的气泡清理由面板自身负责）。
void MainWindow::onAiClearRequested()
{
    if (m_activeAiAgent)
        m_activeAiAgent->clearConversation();
}

// 命令卡片"执行"按钮回调 — 直接把单条命令发到当前活动终端，
// 不走 AI 执行线程（与批量确认执行是两条独立路径）。
// 对应Python: cube-shell.py::_on_single_command_exec（行 5712-5717）
void MainWindow::onAiCommandExecuteRequested(const QString &cmd)
{
    // "当前活动标签页"语义沿用 Python 版：卡片生成后用户切换 tab 再点执行，
    // 命令会发到当前会话而非产生卡片的会话（固有语义，不做更改）。
    auto *session =
        qobject_cast<SshSessionTab *>(activeTabWidget()->currentWidget());
    if (!session || !session->terminal() || !session->terminal()->terminal()) {
        m_aiPanel->appendAiMessage(tr("⚠️ 无活动的 SSH 会话，无法执行命令。"));
        return;
    }
    // 与 Python 版一致：多行内容原样发送到终端，不做首行截断
    //（for 循环、heredoc、反斜杠续行等合法多行单命令需完整发送）。
    if (cmd.trimmed().isEmpty()) {
        m_aiPanel->appendAiMessage(tr("⚠️ 命令为空，已忽略执行。"));
        return;
    }
    session->terminal()->terminal()->sendText(cmd + QStringLiteral("\n"));
}

// AI 命令就绪 → 按安全检查结果分流：全部 SAFE/LOW 自动整批执行，
// 含 MEDIUM 及以上则弹窗确认（展示全部命令，标记风险命令）。
// agent 为发出 commandReady 的会话 agent：弹窗期间用户切标签会改变
// m_activeAiAgent，命令必须回到产生它的 agent，避免跨主机误执行。
// 对应Python: cube-shell.py::_show_confirm_dialog（行 5719-5746）
void MainWindow::onAiCommandReady(const QList<AiCommand> &commands,
                                  SshAiAgent *agent)
{
    if (commands.isEmpty())
        return;

    bool hasRisky = false;
    for (const AiCommand &cmd : commands) {
        if (static_cast<int>(cmd.safety.riskLevel)
            > static_cast<int>(RiskLevel::Low)) {
            hasRisky = true;
            break;
        }
    }

    // 如果所有命令都是安全的，直接执行
    if (!hasRisky) {
        onCommandsApproved(commands, agent);
        return;
    }

    // QPointer 守卫：对话框 exec 期间会话标签可能被关闭，agent 随之析构。
    QPointer<SshAiAgent> agentGuard(agent);
    CommandConfirmDialog dialog(commands, this);
    connect(&dialog, &CommandConfirmDialog::commandsApproved, this,
            [this, agentGuard](const QList<AiCommand> &approved) {
        onCommandsApproved(approved, agentGuard.data());
    });
    connect(&dialog, &CommandConfirmDialog::commandStep, this,
            [this, agentGuard](const QList<AiCommand> &stepCommands) {
        onCommandsStepMode(stepCommands, agentGuard.data());
    });
    dialog.exec();
}

// 用户确认后整批下发 executeCommands（agent 工作线程逐条执行）。
// 命令固定在产生它的 agent 上执行，不受确认期间切标签影响。
// 对应Python: cube-shell.py::_on_commands_approved（行 5748-5755）
void MainWindow::onCommandsApproved(const QList<AiCommand> &commands,
                                    SshAiAgent *agent)
{
    if (!agent) {
        m_aiPanel->appendAiMessage(
            tr("⚠️ 命令所属的 SSH 会话已关闭，无法执行命令。"));
        return;
    }
    agent->executeCommands(commands);
}

// 逐条确认模式 — 逐条弹窗审批收集批准列表（与执行解耦），
// 收集完在同一 agent 上按原顺序整批执行。
// 对应Python: cube-shell.py::_on_commands_step_mode（行 5757-5786）
void MainWindow::onCommandsStepMode(const QList<AiCommand> &commands,
                                    SshAiAgent *agent)
{
    // QPointer 守卫：逐条弹窗期间会话可能被关闭，执行前再判空。
    QPointer<SshAiAgent> agentGuard(agent);
    QList<AiCommand> approved;
    for (int i = 0; i < commands.size(); ++i) {
        SingleCommandConfirmDialog dialog(commands.at(i), i + 1,
                                          commands.size(), this);
        if (dialog.exec() == QDialog::Accepted) {
            approved.append(commands.at(i));
        } else if (dialog.abortAll()) {
            break;   // 终止全部后续命令
        }
        // 否则只是跳过当前命令，继续下一条
    }
    if (!approved.isEmpty())
        onCommandsApproved(approved, agentGuard.data());
}

// 在标签页中打开 Hermes Agent 管理面板。
// 对应Python: cube-shell.py::showHermesPanel
void MainWindow::showHermesPanel()
{
    const QString tabName = QStringLiteral("Hermes Agent");
    // 已有 Hermes 标签页 → 直接切过去（主/副分屏都查）。
    for (QTabWidget *tabs : allPanes()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabText(i) == tabName) {
                tabs->setCurrentIndex(i);
                return;
            }
        }
    }

    auto *panel = new HermesPanel(this);

    // 把已建立的 SSH 会话喂给面板的连接模式下拉框。
    // 与 showClaudeCodePanel 同款:每个会话复用同一个 executor
    //(随 session tab 销毁,面板切回本地)。
    QStringList hosts;
    QList<CommandExecutor *> executors;
    for (QTabWidget *tabs : allPanes()) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto *session = qobject_cast<SshSessionTab *>(tabs->widget(i));
            if (!session || !session->terminal())
                continue;
            const auto client = session->terminal()->sshClient();
            if (!client)
                continue;
            auto *executor = session->findChild<CommandExecutor *>(
                QStringLiteral("hermesExecutor"));
            if (!executor) {
                executor = new CommandExecutor(client.get(), session);
                executor->setObjectName(QStringLiteral("hermesExecutor"));
            }
            const DeviceEntry &device = session->device();
            hosts.append(device.host.isEmpty() ? device.name : device.host);
            executors.append(executor);
        }
    }
    panel->setAvailableConnections(hosts, executors);

#ifdef CUBESHELL_WITH_LOCALPTY
    // Agent 页请求"在终端中打开"→ 新开本机终端跑 hermes chat。
    // 对应Python: agent_widget.open_terminal_requested → 主窗开终端
    // 鸿蒙：无本地 shell，hermes CLI 本地运行载体不存在；面板内已按
    // CUBESHELL_WITH_LOCALPROC 隐藏该入口，这里同步不接线。
    connect(panel, &HermesPanel::openTerminalRequested,
            this, [this](const QString &profileName) {
        QTermWidget *term = openLocalTerminalAt(QString());
        if (!term)
            return;
        QTabWidget *pane = paneOf(term);
        if (pane) {
            const int idx = pane->indexOf(term);
            if (idx >= 0)
                pane->setTabText(idx, QStringLiteral("hermes:%1").arg(profileName));
        }
        const QString command = QStringLiteral("hermes -p %1 chat").arg(profileName);
        // 延迟 500ms 发送,等 shell 就绪(与 openClaudeTerminal 一致)
        QTimer::singleShot(500, term, [term, command]() {
#ifdef Q_OS_WIN
            term->sendText(command + QStringLiteral("\r"));
#else
            term->sendText(command + QStringLiteral("\n"));
#endif
        });
    });
#endif // CUBESHELL_WITH_LOCALPTY

    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(panel, tabName);
    pane->setCurrentIndex(idx);
    panel->refresh();   // 对应Python: Tab 激活时刷新
}

// 在标签页中打开 Claude Code 管理面板。
// 对应Python: cube-shell.py::showClaudeCodePanel
void MainWindow::showClaudeCodePanel()
{
    const QString tabName = QStringLiteral("Claude Code");
    // 已有 Claude Code 标签页 → 直接切过去。
    for (QTabWidget *tabs : allPanes()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->tabText(i) == tabName) {
                tabs->setCurrentIndex(i);
                return;
            }
        }
    }

    auto *panel = new ClaudeCodePanel(this);

    // 把已建立的 SSH 会话喂给面板的连接模式下拉框。
    // 对应Python: claude_code_panel.py::_refresh_connections（ssh_clients）
    for (QTabWidget *tabs : allPanes()) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto *session = qobject_cast<SshSessionTab *>(tabs->widget(i));
            if (!session || !session->terminal())
                continue;
            const auto client = session->terminal()->sshClient();
            if (!client)
                continue;
            // 每个会话复用同一个 executor（随 session tab 销毁，
            // 面板内 QPointer 自动失效回退本地）。
            auto *executor = session->findChild<CommandExecutor *>(
                QStringLiteral("claudeCodeExecutor"));
            if (!executor) {
                executor = new CommandExecutor(client.get(), session);
                executor->setObjectName(QStringLiteral("claudeCodeExecutor"));
            }
            const DeviceEntry &device = session->device();
            panel->addRemoteConnection(
                device.host.isEmpty() ? device.name : device.host, executor);
        }
    }

#ifdef CUBESHELL_WITH_LOCALPTY
    // 子面板请求在终端执行 claude 命令 → 新开本机终端。
    // 对应Python: panel.open_terminal_requested.connect(open_claude_terminal)
    // 鸿蒙：无本地 shell，claude CLI 本地运行载体不存在（面板远程模式仍可用）。
    connect(panel, &ClaudeCodePanel::openTerminalRequested,
            this, &MainWindow::openClaudeTerminal);
#endif

    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(panel, tabName);
    pane->setCurrentIndex(idx);
    // 首次显示由 panel 的 showEvent 触发初始化与首刷（对应Python 行 117-122）
}

#ifdef CUBESHELL_WITH_LOCALPTY
// 在新本机终端中执行 claude 命令（延迟 500ms 发送，等 shell 就绪）。
// 对应Python: cube-shell.py::open_claude_terminal（行 1314-1337）
// 鸿蒙：无本地 shell，本函数整体不编译（其唯一发信方也在 LOCALPTY 内摘除）。
void MainWindow::openClaudeTerminal(const QString &command)
{
    QTermWidget *term = openLocalTerminalAt(QString());
    if (!term)
        return;
    // Tab 名改为 claude 语义名。对应Python: add_new_tab(name=tab_name)
    if (QTabWidget *pane = paneOf(term)) {
        const int idx = pane->indexOf(term);
        if (idx >= 0)
            pane->setTabText(idx, claudeTabName(command));
    }
    // 对应Python: QTimer.singleShot(500, _send_terminal_line)；
    // Windows ConPTY 需要 \r 才能执行，macOS/Linux 用 \n。
    QTimer::singleShot(500, term, [term, command]() {
#ifdef Q_OS_WIN
        term->sendText(command + QStringLiteral("\r"));
#else
        term->sendText(command + QStringLiteral("\n"));
#endif
    });
}
#endif // CUBESHELL_WITH_LOCALPTY

// 对应Python: cube-shell.py::_claude_tab_name（行 1282-1300）：
// 命令可能包裹了切目录前缀，按语义提取而非取最后一个 token。
QString MainWindow::claudeTabName(const QString &command)
{
    const QStringList tokens =
        command.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int i = tokens.indexOf(QStringLiteral("--resume"));
    if (i >= 0 && i + 1 < tokens.size())
        return QStringLiteral("claude:%1").arg(tokens.at(i + 1).left(12));
    if (tokens.contains(QStringLiteral("agents")))
        return QStringLiteral("claude:agents");
    return QStringLiteral("claude");
}

// 隧道池：tunnel.json + 设备凭据解析回调。
// 对应Python: cube-shell.py::tunnel_refresh + open_data(ssh)
void MainWindow::setupTunnels()
{
    m_tunnelPool = new TunnelPool(this);
    m_tunnelPool->setConfigPath(GlobalState::tunnelConfigPath());
    m_tunnelPool->loadConfig();
    m_tunnelPool->setCredentialResolver(
        [this](const QString &deviceName, TunnelSpec &spec, QString &error) {
            // resolved()：隧道要真的建 SSH 连接，需要密码。
            const DeviceEntry e = m_store.resolved(deviceName);
            if (e.name.isEmpty()) {
                error = tr("未找到设备“%1”").arg(deviceName);
                return false;
            }
            const HostPort hp = e.hostPort();
            spec.sshHost = hp.host;
            spec.sshPort = hp.port;
            spec.sshUser = e.username;
            spec.sshPassword = e.password;
            spec.keyType = e.keyType;
            spec.keyFile = e.keyFile;
            return true;
        });
}

// JumpServer / jms:// URL 对接。
// 对应Python: cube-shell.py 中 BastionClient(main_window) 的组装
void MainWindow::setupBastion()
{
    m_bastion = new BastionClient(this);
    connect(m_bastion, &BastionClient::connectRequested,
            this, &MainWindow::onBastionConnect);
}

void MainWindow::handleUrl(const QString &url)
{
#ifdef CUBESHELL_WITH_RDP
    // rdp:// / rdp+ntlm-password:// → 直接开 RDP 标签（不走 BastionClient）。
    // 对应Python: core/rdp/rdp_client.py::build_rdp_url 的 URL 形式
    if (url.startsWith(QLatin1String("rdp://")) || url.startsWith(QLatin1String("rdp+"))) {
        const UrlConnectionInfo info = parseRdpUrl(url);
        if (!info.valid) {
            setStatus(tr("无效的 RDP URL：%1").arg(info.error));
            return;
        }
        RdpSettings settings;
        settings.host = info.host;
        settings.port = info.port;
        settings.username = info.user;
        settings.password = info.password;
        settings.domain = info.domain;
        openRdpTab(settings);
        return;
    }
#endif
    // telnet:// → 直接开 Telnet 标签（不走 BastionClient）。
    // telnet 是 IANA 在案的标准 scheme，网页/文档里的链接本就期望被终端接管。
    // 无 #ifdef：TCP/Telnet 不依赖任何可选组件。
    if (url.startsWith(QLatin1String("telnet://"))) {
        const UrlConnectionInfo info = parseTelnetUrl(url);
        if (!info.valid) {
            setStatus(tr("无效的 Telnet URL：%1").arg(info.error));
            return;
        }
        TcpSettings settings;
        settings.mode = QStringLiteral("telnet");
        settings.host = info.host;
        settings.port = quint16(info.port);
        settings.username = info.user;
        settings.password = info.password;
        // URL 里显式写了用户名 = 意图明确，直接开自动登录（默认值是关的）。
        // 只有用户名没密码也照开：状态机会自动送用户名，密码留给用户手输。
        settings.autoLogin = !info.user.isEmpty();
        openNetTab(settings);
        return;
    }
#ifdef CUBESHELL_WITH_LOCALPTY
    // cubeshell://open-local?path=<dir>[&command=<cmd>] → 直接开本机终端标签。
    // Finder 快速操作 / Windows 右键菜单“在 CubeShell 中打开终端”走的就是这条路径。
    // 必须在此接管：BastionClient::handleUrl 只解析 jms:// 与 ssh://，落到它那里
    // 会得到 valid == false 并被静默丢弃（即菜单点了没反应）。
    // 对应Python: cube-shell.py::handle_open_url 的 cubeshell:// 分支
    // 鸿蒙：无本地 shell，不接管（openLocalTerminalAt 在 LOCALPTY 外不存在）。
    if (url.startsWith(QLatin1String("cubeshell://"))) {
        const UrlConnectionInfo info = parseCubeshellUrl(url);
        if (!info.valid) {
            setStatus(tr("无效的 CubeShell URL：%1").arg(info.error));
            return;
        }
        if (info.action != QLatin1String("open-local")) {
            setStatus(tr("不支持的 CubeShell 操作：%1").arg(info.action));
            return;
        }
        openLocalTerminalAtPath(info.path, info.command);
        return;
    }
#endif
    if (m_bastion)
        m_bastion->handleUrl(url);
}

// 对应Python: core/url_dispatch/bastion_client.py::auto_connect 的 UI 部分
void MainWindow::onBastionConnect(const BastionConnectParams &params)
{
    DeviceEntry device;
    device.name = params.tabName;
    device.username = params.user;
    device.password = params.password;
    device.host = QStringLiteral("%1:%2").arg(params.host).arg(params.port);
    device.port = quint16(params.port);
    device.keyType = params.keyType;
    device.keyFile = params.keyFile;
    // 这条连接落在 JumpServer 的 koko 上，不是资产本身。SFTP 面板要靠这个标记
    // 才能把"代理端不提供文件浏览"说清楚（见 SftpBrowserWidget::setBastionProxied）。
    // 运行时标记，不会写进 devices.json。
    device.viaBastion = true;
    openSshSession(device);
}

void MainWindow::setStatus(const QString &text)
{
    m_statusBar->setText(text);
}

// ---------------------------------------------------------------------------
// 设备配置
// ---------------------------------------------------------------------------

void MainWindow::loadDevices()
{
    // Primary search: GlobalState::configFilePath matches Python's
    // appdirs.user_config_dir("cube-shell") → ~/Library/Application Support/cube-shell/
    // on macOS. Additional fallbacks for development / non-standard layouts.
    QStringList candidates;
    candidates << GlobalState::configFilePath(QStringLiteral("config.dat"))
               << QDir::homePath() + QStringLiteral("/.cube-shell/config.dat")
               << QDir::currentPath() + QStringLiteral("/conf/config.dat")
               << QDir::currentPath() + QStringLiteral("/config.dat")
               << QDir::currentPath() + QStringLiteral("/../conf/config.dat");

    m_jsonPath = GlobalState::configFilePath(QStringLiteral("devices.json"));
    bool loaded = false;

    for (const QString &path : candidates) {
        if (!QFileInfo::exists(path))
            continue;
        QString err;
        if (m_store.load(path, &err)) {
            m_configPath = path;
            m_jsonPath = QFileInfo(path).absolutePath() + QStringLiteral("/devices.json");
            loaded = true;
            break;
        }
    }

    // JSON 存在就优先用它（更新）。
    //
    // 这个分支以前嵌在上面的 config.dat 循环里，于是「有 devices.json 但没有
    // config.dat」的用户什么都读不到。迁移之后这条路径变得要命：devices.json
    // 里存的 id 是钥匙串密码的唯一索引，读不到它就等于所有密码都成了孤儿。
    if (QFileInfo::exists(m_jsonPath)) {
        DeviceConfigStore json;
        if (json.loadJson(m_jsonPath)) {
            m_store = json;
            loaded = true;
        }
    }

    if (!loaded) {
        m_deviceList->setStatus(tr("未找到 config.dat — 请使用“文件 ▸ 打开 config.dat…”"));
        return;
    }

    migrateSecrets();
    refreshDeviceList();
}

// 明文密码 → 钥匙串的一次性迁移。详见 core/config/SecretMigration.h。
void MainWindow::migrateSecrets()
{
    if (!m_store.needsMigration())
        return;

    const SecretMigration::Result r = SecretMigration::run(m_store, m_jsonPath);
    switch (r.status) {
    case SecretMigration::Result::NotNeeded:
        break;
    case SecretMigration::Result::Unsupported:
        // Windows/Linux 后端补齐前会走到这里。密码仍是明文，但文件权限已经
        // 收到 0600。只在状态栏说一句——每次启动弹一个用户改不了的模态框
        // 没有意义。
        setStatus(tr("当前平台暂不支持系统密钥库，设备密码仍以明文保存"
                     "（文件权限已限制为仅本人可读）"));
        break;
    case SecretMigration::Result::Migrated:
        if (r.migrated > 0) {
            QMessageBox::information(
                this, tr("密码已迁移"),
                tr("已将 %1 个设备密码从配置文件迁入系统钥匙串，"
                   "配置文件中不再保存明文密码。\n\n"
                   "迁移前的配置已备份到：\n%2\n\n"
                   "确认各设备均可正常连接后，建议手动删除该备份文件"
                   "（它仍含明文密码）。")
                    .arg(r.migrated).arg(r.backupPath));
        }
        break;
    case SecretMigration::Result::Failed:
        // 失败时明文原样保留，功能不受影响，下次启动会再试一次。
        QMessageBox::warning(
            this, tr("密码迁移未完成"),
            tr("未能将设备密码迁入系统钥匙串：%1\n\n"
               "配置文件中的密码保持不变，设备仍可正常连接，"
               "下次启动会自动重试。").arg(r.error));
        break;
    }
}

void MainWindow::refreshDeviceList()
{
    m_deviceList->setDevices(m_store.devices());
    m_deviceList->setStatus(tr("共 %1 个设备").arg(m_store.count()));
}

bool MainWindow::saveDevices()
{
    if (m_jsonPath.isEmpty())
        m_jsonPath = GlobalState::configFilePath(QStringLiteral("devices.json"));
    QDir().mkpath(QFileInfo(m_jsonPath).absolutePath());
    QString err;
    if (!m_store.saveJson(m_jsonPath, &err)) {
        QMessageBox::warning(this, tr("保存失败"), err);
        return false;
    }
    // 密码与设备条目必须一起落盘：只写 JSON 会让新增设备连不上（钥匙串里没有
    // 它的密码），只写钥匙串会在下次启动时留下对不上号的孤儿。
    if (!m_store.flushSecrets(&err)) {
        QMessageBox::warning(this, tr("保存密码失败"),
                             tr("设备已保存，但密码未能写入系统钥匙串：%1").arg(err));
        return false;
    }
    setStatus(tr("已保存 %1 个设备 → %2").arg(m_store.count()).arg(m_jsonPath));
    return true;
}

void MainWindow::addDevice()
{
    AddDeviceDialog dlg(this);
    // 测试连接用：新建设备密码就在表单里，这个回调通常返回空，仅为统一接口。
    dlg.setPasswordResolver([this](const QString &id) { return m_store.resolvedPassword(id); });
    if (dlg.exec() != QDialog::Accepted)
        return;
    // 新建设备：dlg.device() 已在构造时分配好 id，密码随条目带进来，
    // addDevice 负责把它搬进密码表。
    m_store.addDevice(dlg.device());
    refreshDeviceList();
    saveDevices();
}

void MainWindow::editDevice(const QString &name)
{
    const DeviceEntry *found = m_store.find(name);
    if (!found)
        return;
    // 值副本，不是指针。find() 返回的是哈希内部地址，下面的 removeDevice()
    // 一执行就失效，继续用就是 use-after-free。
    const DeviceEntry old = *found;

    AddDeviceDialog dlg(this);
    dlg.setDevice(old);
    // 钥匙串里已有密码时，密码框允许留空（校验放行、占位符提示）。
    // 不告诉对话框这件事，迁移一完成所有 RDP 设备就都保存不了了。
    dlg.setHasStoredPassword(m_store.hasPassword(old.id));
    // 测试连接用：编辑态密码框可能留空（"留空则不修改"），按 id 取钥匙串里的真实密码。
    dlg.setPasswordResolver([this](const QString &id) { return m_store.resolvedPassword(id); });
    if (dlg.exec() != QDialog::Accepted)
        return;

    DeviceEntry edited = dlg.device();
    edited.id = old.id;    // 改名也好改协议也好，id 终生不变——它是密码的索引

    // Name may have changed: remove the old key, insert the new.
    m_store.removeDevice(name);
    m_store.addDevice(edited);
    // 只有用户真的动过密码框才覆盖。空密码框是「没改」而不是「清空」，
    // 照单全收会让人一改端口就把密码丢了。
    if (dlg.passwordEdited())
        m_store.setPassword(old.id, edited.password);

    refreshDeviceList();
    saveDevices();
}

void MainWindow::removeDevice(const QString &name)
{
    if (QMessageBox::question(this, tr("删除配置"),
                              tr("确定要删除“%1”吗？此操作无法恢复。").arg(name))
            != QMessageBox::Yes)
        return;
    // 先清密码再删条目：flushSecrets 只保留仍被引用的 id，
    // 顺序反了这条密码就会在钥匙串里变成永远清理不掉的孤儿。
    if (const DeviceEntry *e = m_store.find(name))
        m_store.setPassword(e->id, QString());
    m_store.removeDevice(name);
    refreshDeviceList();
    saveDevices();
}

// 对应Python: cube-shell.py::export_config（导出设备配置 Shift+Ctrl+E）
void MainWindow::exportDevices()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出设备配置"), QDir::homePath() + QStringLiteral("/devices.json"),
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QString err;
    // exportJson 而非 saveJson：导出物要给人拷来拷去，不带密码也不带 id。
    if (m_store.exportJson(path, &err)) {
        setStatus(tr("已导出 %1 个设备 → %2（不含密码）").arg(m_store.count()).arg(path));
        QMessageBox::information(
            this, tr("导出完成"),
            tr("已导出 %1 个设备到：\n%2\n\n"
               "出于安全考虑，导出文件不包含密码。\n"
               "在其他机器导入后需要重新输入密码。").arg(m_store.count()).arg(path));
    } else {
        QMessageBox::warning(this, tr("导出失败"), err);
    }
}

// 对应Python: cube-shell.py::import_config（导入设备配置 Shift+Ctrl+I）
void MainWindow::importDevices()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("导入设备配置"), QDir::homePath(),
        tr("设备配置 (*.json *.dat);;所有文件 (*)"));
    if (path.isEmpty())
        return;
    DeviceConfigStore imported;
    QString err;
    bool ok = false;
    if (path.endsWith(QLatin1String(".json")))
        ok = imported.loadJson(path, &err);
    else
        ok = imported.load(path, &err);
    if (!ok) {
        QMessageBox::warning(this, tr("导入失败"), err);
        return;
    }

    int renamed = 0;
    for (const DeviceEntry &src : imported.devices()) {
        DeviceEntry e = src;
        // 外部文件的 id 一律不可信：同一份导出文件导入两次就会撞 id，
        // 手写的文件可能压根没有。清空让 addDevice 现分配一个。
        const QString importedId = e.id;
        e.id.clear();
        // 重名不再静默覆盖。以前覆盖的只是一条配置；现在会让原设备的密码
        // 在钥匙串里变成孤儿，而新设备又没有密码——坏得无声无息。
        if (m_store.find(e.name)) {
            int n = 2;
            QString candidate;
            do {
                candidate = tr("%1 (导入 %2)").arg(src.name).arg(n++);
            } while (m_store.find(candidate));
            e.name = candidate;
            ++renamed;
        }
        // cachedPassword 而非 resolved：只取导入文件自带的密码，
        // 不去解锁本机钥匙串（那里按 importedId 查也只会查到别人的东西）。
        e.password = imported.cachedPassword(importedId);
        m_store.addDevice(e);
    }
    refreshDeviceList();
    saveDevices();
    if (renamed > 0) {
        setStatus(tr("已导入 %1 个设备（%2 个重名已自动改名）")
                      .arg(imported.count()).arg(renamed));
    } else {
        setStatus(tr("已导入 %1 个设备").arg(imported.count()));
    }
}

// ---------------------------------------------------------------------------
// 标签 / 分屏
// ---------------------------------------------------------------------------

void MainWindow::closeTabIn(QTabWidget *tabs, int index)
{
    QWidget *w = tabs->widget(index);
    if (w && w == m_homePage)
        return;   // 首页常驻，不可关闭
    // 先断底层 socket 再安排销毁：左侧 SFTP 浏览器的 deleteLater 先于 tab 执行，
    // 其析构要取消并 join 传输线程。socket 还活着时，走主 session 通道的传输流
    // 只能靠 EAGAIN 轮询看取消标志（每轮最长 5s），可能打穿 join 预算、
    // 逼得析构走泄漏兜底。shutdownSocket 幂等（~SshSessionTab 会再调一次），
    // 且只做 ::shutdown(fd)，与仍在跑的终端读循环并发安全。
    if (auto *session = qobject_cast<SshSessionTab *>(w)) {
        if (session->terminal() && session->terminal()->sshClient())
            session->terminal()->sshClient()->shutdownSocket();
    }
    // 先清理左侧挂接的文件浏览器（已 reparent 到 m_browserStack，
    // 不会随 tab 页销毁）。先于 tab 销毁，保证 SftpClient 早于 SshClient 析构。
    if (QWidget *browser = m_tabBrowsers.take(w)) {
        m_browserStack->removeWidget(browser);
        browser->deleteLater();
    }
#ifdef CUBESHELL_WITH_LOCALPROC
    // 会话标签被关闭：其 dockerExecutor 将随之销毁，先从 Docker 后端撤下。
    if (m_dockerManager && m_dockerExecutor && m_dockerExecutor->parent() == w) {
        m_dockerManager->setRemoteExecutor(nullptr);
        m_dockerExecutor = nullptr;
    }
#endif
    // 进程管理对话框同样只引用不持有 executor，一并撤下。
    if (m_processExecutor && m_processExecutor->parent() == w) {
        if (m_processManagerDialog)
            m_processManagerDialog->setExecutor(nullptr);
        m_processExecutor = nullptr;
    }
    // AI Agent 清理：标签关闭时销毁对应的 SshAiAgent（其 executor 随标签销毁，
    // 必须先于标签 deleteLater 停掉执行线程）。
    // 对应Python: cube-shell.py 标签关闭处 _ai_agents.pop()
    if (auto *session = qobject_cast<SshSessionTab *>(w)) {
        if (auto *agent = m_aiAgents.take(session)) {
            if (agent == m_activeAiAgent) {
                disconnectAiFromAgent(agent);
                m_activeAiAgent = nullptr;
                m_aiPanel->setStatus(false);
            }
            agent->shutdown();
            delete agent;
        }
    }
#ifdef CUBESHELL_WITH_RDP
    // RDP 标签关闭：先断开连接（停 FreeRDP 后端线程/外部客户端进程），再销毁面板。
    // 对应Python: cube-shell.py::off_rdp 里的 widget.stop()
    if (auto *rdp = qobject_cast<RdpPanel *>(w))
        rdp->client()->disconnectFromHost();
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // 串口标签关闭：先关端口（顺带停日志文件），再销毁面板。
    if (auto *serial = qobject_cast<SerialTerminalWidget *>(w))
        serial->client()->close();
#endif
    // TCP/Telnet 标签关闭：先断 socket（顺带停日志文件），再销毁面板。
    // 放在 #ifdef 之外——这两个协议无条件编译。
    if (auto *net = qobject_cast<NetTerminalWidget *>(w))
        net->client()->disconnectFromHost();
    tabs->removeTab(index);
    if (w)
        w->deleteLater();
    pruneEmptyPanes();
    // 关闭标签后，活动 pane 可能已被清理（pruneEmptyPanes）——刷新时指定
    // 首个 pane，避免 activeTabWidget() 返回已销毁的 QPointer。
    updateLeftPanel(m_panes.isEmpty() ? nullptr : m_panes.first());
}

void MainWindow::closeCurrentTab()
{
    QTabWidget *tabs = activeTabWidget();
    if (tabs->count() > 0)
        closeTabIn(tabs, tabs->currentIndex());
}

void MainWindow::nextTab()
{
    QTabWidget *tabs = activeTabWidget();
    if (tabs->count() > 1)
        tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
}

void MainWindow::prevTab()
{
    QTabWidget *tabs = activeTabWidget();
    if (tabs->count() > 1)
        tabs->setCurrentIndex((tabs->currentIndex() + tabs->count() - 1) % tabs->count());
}

// 标签右键菜单。对应Python: 标签栏右键（关闭/其他/分屏）
void MainWindow::showTabContextMenu(QTabWidget *tabs, const QPoint &pos)
{
    const int index = tabs->tabBar()->tabAt(pos);
    if (index < 0)
        return;
    if (tabs->widget(index) == m_homePage)
        return;   // 首页没有关闭/分屏菜单
    QMenu menu(this);
    menu.addAction(tr("关闭"), this, [this, tabs, index]() { closeTabIn(tabs, index); });
    menu.addAction(tr("关闭其他标签页"), this, [this, tabs, index]() {
        QWidget *keep = tabs->widget(index);
        for (int i = tabs->count() - 1; i >= 0; --i) {
            if (tabs->widget(i) != keep)
                closeTabIn(tabs, i);
        }
    });
    menu.addAction(tr("关闭右侧标签页"), this, [this, tabs, index]() {
        for (int i = tabs->count() - 1; i > index; --i)
            closeTabIn(tabs, i);
    });
    menu.addSeparator();
    menu.addAction(tr("水平分屏"), this, [this, tabs, index]() {
        splitTab(tabs, index, Qt::Horizontal);
    });
    menu.addAction(tr("垂直分屏"), this, [this, tabs, index]() {
        splitTab(tabs, index, Qt::Vertical);
    });
    // 已有多个分屏时，允许把标签直接搬到指定的另一个分屏（不新建 pane）。
    if (m_panes.count() > 1) {
        QMenu *moveMenu = menu.addMenu(tr("移动到分屏"));
        for (int p = 0; p < m_panes.count(); ++p) {
            TerminalTabWidget *dest = m_panes[p];
            if (dest == tabs)
                continue;   // 已在此分屏
            moveMenu->addAction(tr("分屏 %1").arg(p + 1), this,
                                [this, tabs, index, dest]() {
                if (index < 0 || index >= tabs->count())
                    return;
                if (tabs->widget(index) == m_homePage)
                    return;
                const QString title = tabs->tabText(index);
                QWidget *w = tabs->widget(index);
                tabs->removeTab(index);
                const int newIdx = dest->addTab(w, title);
                decorateSessionTab(dest, newIdx);
                dest->setCurrentIndex(newIdx);
                pruneEmptyPanes();
                dest->setFocus();
            });
        }
    }
    menu.exec(tabs->tabBar()->mapToGlobal(pos));
}

// 把标签拆到一个新建的相邻分屏。对应Python: ui/ 拖拽分屏逻辑（简化为菜单驱动）
void MainWindow::splitTab(QTabWidget *source, int index, Qt::Orientation orientation)
{
    if (index < 0 || index >= source->count())
        return;
    if (source->widget(index) == m_homePage)
        return;   // 首页不参与分屏

    TerminalTabWidget *newPane = createPane();
    insertPaneNextTo(source, newPane, orientation);

    const QString title = source->tabText(index);
    QWidget *w = source->widget(index);
    source->removeTab(index);
    const int newIdx = newPane->addTab(w, title);
    // removeTab 会销毁旧的 tabButton，移过去后重新装上圆点和关闭按钮。
    decorateSessionTab(newPane, newIdx);
    newPane->setCurrentIndex(newIdx);
    newPane->setFocus();
    // 刚从单分屏变多分屏时，原分屏的标题需要补上"分屏 1 ·"前缀。
    updatePaneHighlight();
}

// 在 source 所在的 splitter 中，于 source 之后插入 pane，方向为 orientation。
// 若 splitter 方向与 orientation 不符且不止一个子控件，则把 source 就地
// 替换为一个新的子 splitter（嵌套），实现水平+垂直混排的自由分屏。
void MainWindow::insertPaneNextTo(QTabWidget *source, TerminalTabWidget *pane,
                                  Qt::Orientation orientation)
{
    auto *splitter = qobject_cast<QSplitter *>(source->parentWidget());
    if (!splitter)
        return;   // source 不在 splitter 里（理论不应发生）

    const int sourceIdx = splitter->indexOf(source);
    if (sourceIdx < 0)
        return;

    // 当前 splitter 方向与目标方向一致，或只有一个子控件（可自由改方向）→ 直接插入。
    if (splitter->orientation() == orientation || splitter->count() == 1) {
        splitter->setOrientation(orientation);
        splitter->insertWidget(sourceIdx + 1, pane);
        equalizeSplitter(splitter);
        return;
    }

    // 方向不一致且 splitter 有多个子控件 → 不能改变现有 splitter 方向，
    // 否则会打乱其他 pane 的布局。解决：把 source 就地替换为一个嵌套的子 splitter，
    // 子 splitter 内装 source 和 pane，方向为 orientation。
    auto *subSplitter = new QSplitter(orientation, splitter);
    subSplitter->setChildrenCollapsible(false);
    // 先从父 splitter 摘下 source 的尺寸，再用子 splitter 换上去，
    // 保证嵌套前后父 splitter 其余子控件的尺寸不变。
    const QList<int> oldSizes = splitter->sizes();
    splitter->insertWidget(sourceIdx, subSplitter);
    source->setParent(subSplitter);
    subSplitter->addWidget(source);
    subSplitter->addWidget(pane);
    splitter->setSizes(oldSizes);   // 恢复父 splitter 原尺寸分布
    equalizeSplitter(subSplitter);  // 子 splitter 内两个 pane 均分
}

// 清理空 pane 并折叠只剩一个子控件的中间 splitter。
// 首个 pane（承载首页）常驻，其余 pane 空时自动移除。
void MainWindow::pruneEmptyPanes()
{
    // 逆序遍历 pane 列表，删除时不影响后续索引。
    for (int i = m_panes.count() - 1; i >= 1; --i) {
        TerminalTabWidget *pane = m_panes[i];
        if (pane->count() == 0) {
            m_panes.removeAt(i);
            // 从 splitter 树中摘下。QSplitter 没有 removeWidget()，
            // 标准做法是 setParent(nullptr)——先 hide 避免它短暂
            // 变成顶层窗口而闪一下。
            pane->hide();
            pane->setParent(nullptr);
            pane->deleteLater();
        }
    }

    // 折叠只剩一个子控件的中间 splitter：把该子控件提到祖父 splitter 替换掉
    // 该子 splitter，避免嵌套层级无限增长（反复分屏 + 关闭后容易留下空壳）。
    std::function<void(QSplitter *)> collapseSingleChild = [&](QSplitter *sp) {
        for (int i = 0; i < sp->count(); ++i) {
            if (auto *child = qobject_cast<QSplitter *>(sp->widget(i)))
                collapseSingleChild(child);
        }
        if (sp == m_termSplitter)
            return;   // 顶层 splitter 不折叠
        if (sp->count() == 1) {
            QWidget *only = sp->widget(0);
            auto *grandpa = qobject_cast<QSplitter *>(sp->parentWidget());
            if (!grandpa)
                return;
            const int spIdx = grandpa->indexOf(sp);
            const QList<int> oldSizes = grandpa->sizes();
            grandpa->insertWidget(spIdx, only);
            grandpa->setSizes(oldSizes);
            sp->deleteLater();
        }
    };
    collapseSingleChild(m_termSplitter);

    // pane 数量变了：高亮标记按"是否多分屏"重算（降回单分屏时清除所有高亮）。
    // 活动 pane 若刚被删除，QPointer 已归零，改指首个 pane。
    if (!m_activePane && !m_panes.isEmpty())
        m_activePane = m_panes.first();
    updatePaneHighlight();
}

// 把 splitter 的子控件尺寸重新均分（新建 pane 时保证不挤占其他 pane）。
void MainWindow::equalizeSplitter(QSplitter *splitter)
{
    const int n = splitter->count();
    if (n == 0)
        return;
    const int total = (splitter->orientation() == Qt::Horizontal)
                          ? splitter->width() : splitter->height();
    const int avg = qMax(1, total / n);
    QList<int> sizes;
    sizes.reserve(n);
    for (int i = 0; i < n; ++i)
        sizes << avg;
    splitter->setSizes(sizes);
}

QTabWidget *MainWindow::activeTabWidget() const
{
    // 返回最后获得焦点的 pane。若 m_activePane 已被销毁（QPointer 归零），
    // 则退回首个 pane（理论上不会发生，pruneEmptyPanes 保证首个 pane 常驻）。
    if (m_activePane)
        return m_activePane;
    return m_panes.isEmpty() ? nullptr : m_panes.first();
}

QTermWidget *MainWindow::currentTerminal() const
{
    QTabWidget *tabs = activeTabWidget();
    QWidget *w = tabs ? tabs->currentWidget() : nullptr;
    if (!w)
        return nullptr;
    // 会话标签页可能是 QTermWidget 本身，也可能是包了一层容器的（SSH/串口）。
    if (auto *term = qobject_cast<QTermWidget *>(w))
        return term;
    return w->findChild<QTermWidget *>();
}

void MainWindow::setActivePane(TerminalTabWidget *pane)
{
    if (m_activePane == pane)
        return;
    m_activePane = pane;
    updatePaneHighlight();
}

// 给活动分屏加高亮左边框，非活动分屏保持原样。只有一个分屏时不加任何标记
// （无需区分），避免单分屏用户看到多余的装饰。
void MainWindow::updatePaneHighlight()
{
    const bool multi = m_panes.count() > 1;
    for (TerminalTabWidget *pane : std::as_const(m_panes)) {
        const bool active = multi && (pane == m_activePane);
        // 在标签样式基础上追加高亮边框，切主题时 windowsTerminalTabStyle()
        // 会重新取色，这里跟着一起重建。
        QString qss = windowsTerminalTabStyle();
        if (active) {
            qss += QStringLiteral(
                "QTabWidget::pane { border-top: 1px solid palette(highlight); }");
        }
        pane->setStyleSheet(qss);
    }
    // 分屏数变化时徽章的显示条件（多分屏才显示）和序号都可能变，一并重算。
    refreshPaneIndicators();
}

// 焦点在分屏之间循环切换。delta=1 向后，delta=-1 向前。
// 对应快捷键 Ctrl+Alt+Tab / Ctrl+Alt+Shift+Tab（或菜单"下一个分屏"/"上一个分屏"）。
void MainWindow::focusNextPane(int delta)
{
    if (m_panes.count() <= 1)
        return;   // 只有一个 pane，无需切换
    const int current = m_activePane ? m_panes.indexOf(m_activePane) : 0;
    const int next = (current + delta + m_panes.count()) % m_panes.count();
    m_panes[next]->setFocus();
}

// 全部分屏合并回首个 pane：把其他 pane 的全部标签搬到首个 pane，再清空其他 pane。
void MainWindow::mergeAllPanes()
{
    if (m_panes.count() <= 1)
        return;
    TerminalTabWidget *first = m_panes.first();
    for (int i = 1; i < m_panes.count(); ++i) {
        QTabWidget *pane = m_panes[i];
        while (pane->count() > 0) {
            const QString title = pane->tabText(0);
            QWidget *w = pane->widget(0);
            pane->removeTab(0);
            const int idx = first->addTab(w, title);
            decorateSessionTab(first, idx);
        }
    }
    pruneEmptyPanes();
    first->setFocus();
    // 合并后只剩一个分屏 → 标题不再显示"分屏 1 ·"前缀，高亮也消失。
    updateLeftPanel(first);
    updatePaneHighlight();
}

QList<QTabWidget *> MainWindow::allPanes() const
{
    QList<QTabWidget *> result;
    result.reserve(m_panes.count());
    for (TerminalTabWidget *pane : m_panes)
        result.append(pane);
    return result;
}

QTabWidget *MainWindow::paneOf(QWidget *page) const
{
    for (QTabWidget *pane : allPanes()) {
        if (pane->indexOf(page) >= 0)
            return pane;
    }
    return nullptr;
}

// 新标签页的落点：当前活动 pane（无则首个 pane）。
TerminalTabWidget *MainWindow::targetPane() const
{
    if (m_activePane)
        return m_activePane.data();
    return m_panes.isEmpty() ? nullptr : m_panes.first();
}

// 状态栏终端信息（大小/编码）。
void MainWindow::updateTerminalInfo()
{
    QTabWidget *tabs = activeTabWidget();
    if (!tabs)
        return;
    QWidget *w = tabs->currentWidget();
    // 仅远程 SSH 会话标签页显示设备列表底部的两个会话开关。
    // 对应Python: 未连接时 follow_folder / remote_monitoring 不可见
    if (m_deviceList)
        m_deviceList->setSessionActive(qobject_cast<SshSessionTab *>(w) != nullptr);
    QTermWidget *term = qobject_cast<QTermWidget *>(w);
    if (!term && w)
        term = w->findChild<QTermWidget *>();
    // setupUi 里首页 addTab 会触发 currentChanged，此时 setupStatusBar 尚未
    // 运行，标签还不存在——早退避免解引用空指针。
    if (!m_termSizeLabel)
        return;
    if (term)
        m_termSizeLabel->setText(QStringLiteral("%1×%2")
                                     .arg(term->screenColumnsCount())
                                     .arg(term->screenLinesCount()));
    else
        m_termSizeLabel->clear();
}

// ---------------------------------------------------------------------------
// 左侧文件浏览器（设备列表下方）
// ---------------------------------------------------------------------------

// 左侧文件浏览器切到当前标签对应的页，并替换设备列表（仅留底部复选框）；
// 无对应页（首页/未连接）则收起浏览器、恢复设备列表。
// 对应Python: shell_tab_current_changed 里文件树随 Tab 切换/清空的逻辑
void MainWindow::updateLeftPanel(QTabWidget *pane)
{
    // 同 updateTerminalInfo：setupUi 中首页 addTab 触发的 currentChanged
    // 早于左栏控件创建，此处提前返回。
    if (!m_browserStack || !m_deviceList || !m_leftSplitter)
        return;
    // pane 非空表示由该 pane 的 currentChanged 触发 —— 必须用它，不能用
    // activeTabWidget()：焦点可能还留在另一个分屏上（在新分屏建终端后点
    // 首页，焦点仍属新分屏），那样会取错页面导致设备列表不显示。
    QTabWidget *tabs = pane ? pane : activeTabWidget();
    QWidget *page = tabs ? tabs->currentWidget() : nullptr;
    QWidget *browser = page ? m_tabBrowsers.value(page) : nullptr;
    if (!browser) {
        m_browserStack->setVisible(false);
        m_deviceList->setBrowserMode(false);
        return;
    }
    m_browserStack->setCurrentWidget(browser);
    m_deviceList->setBrowserMode(true);
    if (!m_browserStack->isVisible()) {
        m_browserStack->setVisible(true);
        if (!m_leftBrowserSized) {
            // 首次展开：文件树占满左栏，设备控件压缩到底部复选框行高度。
            m_leftSplitter->setSizes({m_leftSplitter->height(), 1});
            m_leftBrowserSized = true;
        }
    }
    // 更新浏览器路径栏的分屏徽章。
    updatePaneIndicator(tabs, page);
    // 本机终端 + 跟随目录：切回来时同步一次终端 cwd。
    // 对应Python: follow_folder 勾选时切 tab 自动 refreshDirs
    if (m_deviceList->followFolderEnabled())
        syncBrowserToTerminalCwd();
}

// 刷新文件浏览器路径栏左侧的分屏徽章，让用户知道当前看的是谁的目录。
// 单分屏时徽章隐藏（无歧义），多分屏时显示序号 + tooltip 给出完整标签名。
void MainWindow::updatePaneIndicator(QTabWidget *pane, QWidget *page)
{
    if (!pane || !page)
        return;
    QWidget *browser = m_tabBrowsers.value(page);
    if (!browser)
        return;
    const int num = paneNumber(pane);
    const int idx = pane->indexOf(page);
    const QString tabTitle = (idx >= 0) ? pane->tabText(idx) : QString();
    const int total = m_panes.count();
    if (auto *sftp = qobject_cast<SftpBrowserWidget *>(browser))
        sftp->setPaneIndicator(num, total, tabTitle);
    else if (auto *local = qobject_cast<LocalFileBrowserWidget *>(browser))
        local->setPaneIndicator(num, total, tabTitle);
}

// 重刷全部标签的徽章：徽章可见性取决于分屏总数，分屏增减时整体重算，
// 否则刚分屏/合并时已有浏览器的徽章会停留在旧状态。
void MainWindow::refreshPaneIndicators()
{
    for (TerminalTabWidget *pane : std::as_const(m_panes)) {
        for (int i = 0; i < pane->count(); ++i)
            updatePaneIndicator(pane, pane->widget(i));
    }
}

// pane 在 m_panes 中的序号（从 1 开始，用于界面展示）；找不到返回 0。
int MainWindow::paneNumber(QTabWidget *pane) const
{
    const int idx = m_panes.indexOf(qobject_cast<TerminalTabWidget *>(pane));
    return (idx >= 0) ? (idx + 1) : 0;
}

// 把当前标签的文件浏览器同步到终端当前工作目录。
// 对应Python: _on_follow_folder_changed → refreshDirs
void MainWindow::syncBrowserToTerminalCwd()
{
    QWidget *page = activeTabWidget()->currentWidget();
    QWidget *browser = page ? m_tabBrowsers.value(page) : nullptr;
    if (!browser)
        return;
    if (auto *sftp = qobject_cast<SftpBrowserWidget *>(browser)) {
        // 远程：用最后一次 OSC7 报告的 cwd（cwdChanged 接线处记录）。
        const QString last = sftp->property("lastCwd").toString();
        if (!last.isEmpty())
            sftp->setCurrentPath(last);
    } else if (auto *local = qobject_cast<LocalFileBrowserWidget *>(browser)) {
        // 本机：qtermwidget 直接能拿到 shell 进程的 cwd。
        if (auto *term = qobject_cast<QTermWidget *>(page))
            local->setRootPath(term->workingDirectory());
    }
}

// ---------------------------------------------------------------------------
// 远程监控（状态栏 8 项指标）
// ---------------------------------------------------------------------------

// 订阅当前标签的 RemoteMonitor（跨线程信号 → 显式 QueuedConnection）。
// 对应Python: cube-shell.py 监控区随当前标签切换刷新
void MainWindow::bindMonitorToTab(QWidget *tabWidget)
{
    auto *tab = qobject_cast<SshSessionTab *>(tabWidget);
    if (tab == m_monitorTab)
        return;

    // 取消旧订阅。
    if (m_monitorTab && m_monitorTab->monitor())
        disconnect(m_monitorTab->monitor(), nullptr, this, nullptr);
    m_monitorTab = tab;
    resetStatusItems();

    if (!tab)
        return;

    auto subscribe = [this, tab]() {
        if (m_monitorTab != tab)     // 标签已切走，忽略迟到的 monitorReady
            return;
        RemoteMonitor *mon = tab->monitor();
        if (!mon)
            return;
        // RemoteMonitor 的信号从监控线程发射 → 必须 Qt::QueuedConnection。
        // 状态栏 8 项指标同步更新。对应Python: refresh_status_bar
        connect(mon, &RemoteMonitor::statsUpdated,
                this, &MainWindow::updateStatusStats, Qt::QueuedConnection);
        // 未勾选远程监控时不采集。对应Python: _on_remote_monitoring_changed
        if (!m_deviceList->remoteMonitoringEnabled())
            mon->stop();
    };
    if (tab->monitor())
        subscribe();
    else
        connect(tab, &SshSessionTab::monitorReady, this, subscribe);
}

// 状态栏 8 项指标刷新（主线程，经 QueuedConnection 进入）。
// 对应Python: cube-shell.py::refreshSysInfo L4608-4691
void MainWindow::updateStatusStats(const RemoteStats &stats)
{
    if (m_monitorTab) {
        const DeviceEntry &dev = m_monitorTab->device();
        m_statusHostname->setText(dev.hostPort().host);
        m_statusUser->setText(dev.username);
    }
    if (stats.cpuValid)
        m_statusCpu->setText(QStringLiteral("CPU: %1%").arg(stats.cpu.totalUsage, 0, 'f', 2));
    m_statusMem->setText(QStringLiteral("MEM: %1%").arg(stats.memory.usagePercent, 0, 'f', 2));
    if (stats.networkValid) {
        // 字节动态单位。对应Python: util.format_speed(transmit/receive_speed)
        m_statusUpload->setText(formatSpeed(stats.txSpeed));
        m_statusDownload->setText(formatSpeed(stats.rxSpeed));
    }
    if (!stats.uptimeText.isEmpty())
        m_statusUptime->setText(stats.uptimeText);

    // 磁盘：优先显示真实物理分区（关键挂载点白名单，过滤 tmpfs），
    // 最多 4 个，双空格连接；无命中时兜底显示全分区总用量。
    // 对应Python: cube-shell.py::refreshSysInfo L4646-4662
    static const QSet<QString> kKeyMounts = {
        QStringLiteral("/"), QStringLiteral("/data"), QStringLiteral("/boot"),
        QStringLiteral("/home"), QStringLiteral("/var"), QStringLiteral("/tmp"),
        QStringLiteral("/opt")};
    QStringList diskParts;
    for (const DataParser::DiskPartition &p : stats.diskPartitions) {
        if (!kKeyMounts.contains(p.mountPoint)
            || p.filesystem.startsWith(QLatin1String("tmpfs")))
            continue;
        diskParts << QStringLiteral("%1: %2%").arg(p.mountPoint).arg(int(p.usagePercent));
        if (diskParts.size() == 4)   // 对应 real_parts[:4]
            break;
    }
    if (!diskParts.isEmpty())
        m_statusDisk->setText(diskParts.join(QStringLiteral("  ")));
    else
        m_statusDisk->setText(QStringLiteral("/: %1%").arg(stats.diskTotalUsage, 0, 'f', 0));

    // 首次数据到达后显示 8 个监控小方块（状态栏本身常驻，不整体 show/hide）
    for (StatusBoxItem *item : {m_statusHostname, m_statusCpu, m_statusMem, m_statusUpload,
                                m_statusDownload, m_statusUptime, m_statusUser, m_statusDisk})
        item->setVisible(true);
}

// 断开/切换标签时复位并隐藏 8 项指标。对应Python: statusBar().hide()
void MainWindow::resetStatusItems()
{
    if (!m_statusHostname)
        return;   // setupStatusBar 尚未执行
    m_statusHostname->setText(QStringLiteral("—"));
    m_statusCpu->setText(QStringLiteral("CPU: —"));
    m_statusMem->setText(QStringLiteral("MEM: —"));
    m_statusUpload->setText(QStringLiteral("— Mb/s"));
    m_statusDownload->setText(QStringLiteral("— Mb/s"));
    m_statusUptime->setText(QStringLiteral("—"));
    m_statusUser->setText(QStringLiteral("—"));
    m_statusDisk->setText(QStringLiteral("/: —%"));
    for (StatusBoxItem *item : {m_statusHostname, m_statusCpu, m_statusMem, m_statusUpload,
                                m_statusDownload, m_statusUptime, m_statusUser, m_statusDisk})
        item->setVisible(false);
}

// ---------------------------------------------------------------------------
// 终端 / 会话
// ---------------------------------------------------------------------------

#ifdef CUBESHELL_WITH_LOCALPTY
void MainWindow::openLocalTerminal()
{
    openLocalTerminalAt(QString());
}

// 在 path 目录新开本机终端，可选在 shell 就绪后自动执行 command。
// cubeshell://open-local 的落地点（Finder 快速操作 / Windows 右键菜单）。
// 对应Python: cube-shell.py::open_local_terminal_at_path
void MainWindow::openLocalTerminalAtPath(const QString &path, const QString &command)
{
    // 对应Python: if not os.path.isdir(path): logger.warning(...); return
    // parseCubeshellUrl 已校验过一次，但直接调用方（命令行传目录）没有，这里兜底。
    if (!QFileInfo(path).isDir()) {
        setStatus(tr("目录不存在：%1").arg(path));
        return;
    }

    // 窗口刚 show() 尚未完成布局时创建终端，PTY 会按错误的初始尺寸建窗；
    // 延到事件循环下一轮再开，冷启动（argv 带 URL）和已运行时都安全。
    // 对应Python: QTimer.singleShot(0, lambda: window.open_local_terminal_at_path(...))
    QTimer::singleShot(0, this, [this, path, command]() {
        QTermWidget *term = openLocalTerminalAt(path);
        if (!term)
            return;
        if (command.trimmed().isEmpty())
            return;
        // 对应Python: QTimer.singleShot(500, _send_terminal_line)；
        // Windows ConPTY 需要 \r 才能执行，macOS/Linux 用 \n（与 openClaudeTerminal 一致）。
        QTimer::singleShot(500, term, [term, command]() {
#ifdef Q_OS_WIN
            term->sendText(command + QStringLiteral("\r"));
#else
            term->sendText(command + QStringLiteral("\n"));
#endif
        });
    });
}

// startDir 非空时以其为 shell 初始工作目录（延迟启动：先设目录再 run）。
// 对应Python: cube-shell.py::open_local_terminal_in_selected_folder（start_dir）
// 鸿蒙：无本地 shell，本函数整体不编译（所有调用方均已按 LOCALPTY 摘除）。
QTermWidget *MainWindow::openLocalTerminalAt(const QString &startDir)
{
    const bool hasDir = !startDir.isEmpty() && QFileInfo(startDir).isDir();
    auto *term = new QTermWidget(hasDir ? 0 : 1, this);   // startnow=1 -> run the shell immediately
    if (hasDir) {
        term->setWorkingDirectory(startDir);
        term->startShellProgram();
    }
    QFont font(GlobalState::instance().fontFamily(), GlobalState::instance().fontSize());
    if (font.family().isEmpty())
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    term->setTerminalFont(font);
    // 从 theme.json 读取终端配色方案(对应 Python current_theme_name)
    term->setColorScheme(GlobalState::instance().terminalTheme());
    // 右键菜单切换配色后：持久化到 theme.json 并同步到所有已打开终端。
    connect(term, &QTermWidget::colorSchemeChanged, this,
            [this](const QString &name) { applyTerminalThemeEverywhere(name, this); });
    // 回滚缓冲行数（同时决定“查找”能检索到多久以前的输出）。
    term->setHistorySize(GlobalState::instance().scrollbackLines());
    // 右键菜单"AI" → 切换 AI 助手面板。
    // 对应Python: ai_action → self.window()._toggle_ai_panel()
    connect(term, &QTermWidget::aiRequested, this, &MainWindow::toggleAiPanel);
    // Ctrl/Cmd+滚轮缩放后同步字号到内存主题，新开终端沿用。
    // 对应Python: zoom_in/zoom_out 中 util.THEME['font_size'] = size
    connect(term, &QTermWidget::fontSizeChanged, this,
            [](int size) { GlobalState::instance().setFontSize(size); });
    // 标签名带目录名后缀。对应Python: tab_name = f"本机终端 - {base}"
    QString title = tr("本机终端");
    if (hasDir) {
        const QString base = QFileInfo(startDir).fileName();
        title += QStringLiteral(" - ") + (base.isEmpty() ? startDir : base);
    }
    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(term, title);
    decorateSessionTab(pane, idx);
    pane->setCurrentIndex(idx);
    updateTerminalInfo();

    // 左侧展示本地文件目录（初始 home）。
    // 对应Python: 本机终端 (is_local) 分支的 refreshDirs → 本地目录树
    auto *browser = new LocalFileBrowserWidget(m_browserStack);
    if (hasDir)
        browser->setRootPath(startDir);
    // 右键“新建位于文件夹位置的终端窗口”→ 以选中目录再开一个本机终端。
    // 对应Python: cube-shell.py::open_local_terminal_in_selected_folder
    connect(browser, &LocalFileBrowserWidget::newTerminalRequested,
            this, &MainWindow::openLocalTerminalAt);
    m_browserStack->addWidget(browser);
    m_tabBrowsers.insert(term, browser);
    updateLeftPanel();

    connect(term, &QTermWidget::finished, this, [this, term]() {
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(term);
            if (i >= 0) {
                closeTabIn(tabs, i);
                return;
            }
        }
        term->deleteLater();
    });
    return term;
}
#endif // CUBESHELL_WITH_LOCALPTY

void MainWindow::openSshSession(const DeviceEntry &stub)
{
    // The list item only carries name/host; pull the full entry (credentials)
    // from the store by name.
    //
    // resolved() 而非 find()：设备列表里的条目不带密码（密码只在钥匙串里），
    // 这里是「真要连了」的时刻，才按需解锁取出来。找不到时回落 stub，
    // 与原先 find() 返回 nullptr 的兜底行为一致。
    const DeviceEntry full = m_store.resolved(stub.name);
    const DeviceEntry device = full.name.isEmpty() ? stub : full;

    // --- 协议分发 ---
    // 五种协议，按 device.protocol 显式分流。方法名叫 openSshSession 是历史
    // 遗留（它其实是所有协议的双击入口），SSH 留在末尾兜底。
    //
    // 串口：无 host/凭据，DeviceEntry 里存的是端口名与帧格式参数。
    if (device.isSerial()) {
#ifdef CUBESHELL_WITH_SERIAL
        openSerialTab(serialSettingsFromDevice(device));
#else
        QMessageBox::warning(this, tr("串口"),
                             tr("当前构建未启用串口支持（CUBESHELL_WITH_SERIAL=OFF）。"));
#endif
        return;
    }

    // RDP：对应Python: cube-shell.py::cd（行 3119-3206）中
    // device_protocol(conf)=="rdp" 走 open_rdp_tab 分支
    if (device.isRdp()) {
#ifdef CUBESHELL_WITH_RDP
        RdpSettings settings;
        const HostPort hp = device.hostPort();   // "host:port" → host/port（RDP 默认 3389）
        settings.host = hp.host;
        settings.port = hp.port;
        settings.username = device.username;
        settings.password = device.password;
        settings.domain = device.domain;
        openRdpTab(settings);   // openRdpTab 内部用 computeRdpTargetResolution 计算分辨率
#else
        QMessageBox::warning(this, tr("RDP"),
                             tr("当前构建未启用 RDP 支持（CUBESHELL_WITH_RDP=OFF）。"));
#endif
        return;
    }

    // Telnet / 裸 TCP：共用一个面板，模式由 TcpSettings::mode 区分。
    // 无 #ifdef 兜底分支——这两个协议在任何构建里都编进来了。
    if (device.isTelnet() || device.isTcp()) {
        openNetTab(netSettingsFromDevice(device));
        return;
    }

    // 兜底：SSH。protocol 为空的旧配置也走这里（见 DeviceEntry::isSsh）。
    auto *tab = new SshSessionTab(device, this);
    // 右键菜单"AI" → 切换 AI 助手面板。
    // 对应Python: ai_action → self.window()._toggle_ai_panel()
    if (tab->terminal() && tab->terminal()->terminal())
        connect(tab->terminal()->terminal(), &QTermWidget::aiRequested,
                this, &MainWindow::toggleAiPanel);
    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(tab, device.name.isEmpty() ? device.host : device.name);
    decorateSessionTab(pane, idx);
    setTabConnected(tab, false);   // 连上之前先亮红点
    pane->setCurrentIndex(idx);
    setStatus(tr("正在连接 %1@%2…").arg(device.username, device.host));

    connect(tab, &SshSessionTab::connected, this, [this, device, tab]() {
        setStatus(tr("已连接：%1@%2").arg(device.username, device.host));
        setTabConnected(tab, true);
        // 连接成功 → 把会话的 SFTP 浏览器挂到左侧面板（reparent 到 stack）。
        // 对应Python: on_ssh_connected 后左侧 treeWidget 展示远程文件树
        if (SftpBrowserWidget *browser = tab->sftpBrowser()) {
            if (m_browserStack->indexOf(browser) < 0) {
                m_browserStack->addWidget(browser);
                m_tabBrowsers.insert(tab, browser);
            }
        }
        updateLeftPanel();
        updateTerminalInfo();
    });
    // OSC7 cwd 报告：首个即远程 home，作为文件树初始目录；
    // 之后仅在勾选“跟随终端目录”时联动。
    // 对应Python: _on_cwd_changed（follow_folder → refreshDirs）
    connect(tab, &SshSessionTab::cwdChanged, this, [this, tab](const QString &path) {
        SftpBrowserWidget *browser = tab->sftpBrowser();
        if (!browser)
            return;
        const bool first = !browser->property("cwdSeen").toBool();
        browser->setProperty("cwdSeen", true);
        browser->setProperty("lastCwd", path);
        if (first || m_deviceList->followFolderEnabled())
            browser->setCurrentPath(path);
    });
    connect(tab, &SshSessionTab::connectionFailed, this, [this, tab](const QString &msg) {
        setStatus(tr("连接失败"));
        QMessageBox::warning(this, tr("SSH 连接失败"), msg);
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(tab);
            if (i >= 0) {
                closeTabIn(tabs, i);
                return;
            }
        }
        tab->deleteLater();
    });
    connect(tab, &SshSessionTab::disconnected, this, [this, device, tab]() {
        setStatus(tr("已断开：%1").arg(device.host));
        setTabConnected(tab, false);
#ifdef CUBESHELL_WITH_LOCALPROC
        // Docker 后端若正用着该会话的 executor，撤下避免悬垂
        // （DockerManager 持裸指针，凭 QPointer 判归属）。
        if (m_dockerManager && m_dockerExecutor && m_dockerExecutor->parent() == tab) {
            m_dockerManager->setRemoteExecutor(nullptr);
            m_dockerExecutor = nullptr;
        }
#endif
        // 进程管理对话框同理：撤下该会话的 executor，避免刷新到已断连接。
        if (m_processExecutor && m_processExecutor->parent() == tab) {
            if (m_processManagerDialog)
                m_processManagerDialog->setExecutor(nullptr);
            m_processExecutor = nullptr;
        }
    });
    connect(tab, &SshSessionTab::mfaRequested, this, [this](const QString &prompt) {
        setStatus(tr("MFA 验证：%1").arg(prompt));
    });

    bindMonitorToTab(tab);
    tab->connectToHost();
}

#ifdef CUBESHELL_WITH_RDP

namespace {

// 计算 RDP 连接分辨率：主屏物理像素（逻辑尺寸×设备像素比）÷ 显示缩放系数 2
//（Retina 高分屏直接用满物理像素会让远程内容过小），夹在 [1280x800, 4096x2304]
// 区间内后宽对齐 4、高对齐 2（RDP 协议要求）。
// 对应Python: cube-shell.py::RDP_DISPLAY_SCALE + _rdp_target_resolution（行 1612-1637）
RdpSettings computeRdpTargetResolution(RdpSettings settings)
{
    int w = 1920, h = 1080;   // 取不到屏幕信息时的回退值
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect geo = screen->geometry();
        const qreal dpr = screen->devicePixelRatio();
        w = int(geo.width() * dpr / 2.0);
        h = int(geo.height() * dpr / 2.0);
    }
    w = qMax(1280, qMin(w, 4096));
    h = qMax(800, qMin(h, 2304));
    w -= w % 4;
    h -= h % 2;
    settings.width = w;
    settings.height = h;
    return settings;
}

} // namespace

// 打开 RDP 远程桌面标签页。host 为空（“新建 RDP 连接”菜单）时只建空白面板，
// 用户填好表单后自行点“连接”；host 非空（rdp:// URL 分发）则立即建连。
// 对应Python: cube-shell.py::add_new_rdp_tab（行 1589-1610）+ open_rdp_tab（行 1640-1691）
// 注：Python 版还支持从设备列表打开 protocol == "rdp" 的设备；C++ 侧
// 由 openSshSession 按 DeviceEntry::isRdp() 分流到本方法（见上方分发逻辑）。
void MainWindow::openRdpTab(const RdpSettings &settings)
{
    const RdpSettings resolved = computeRdpTargetResolution(settings);

    auto *panel = new RdpPanel(this);
    panel->setSettings(resolved);

    // Tab 标题 "RDP: hostname"；空白面板先用占位名，连上后按实际主机改名。
    const QString title = resolved.host.isEmpty()
                              ? tr("RDP 连接")
                              : QStringLiteral("RDP: %1").arg(resolved.host);
    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(panel, title);
    decorateSessionTab(pane, idx);
    setTabConnected(panel, false);   // 连上之前先亮红点
    pane->setCurrentIndex(idx);

    RdpClient *client = panel->client();
    connect(client, &RdpClient::connected, this, [this, panel]() {
        setTabConnected(panel, true);
        const QString host = panel->client()->settings().host;
        setStatus(tr("RDP 已连接：%1").arg(host));
        // 标题跟随实际连接主机（空白面板手填 / 断开后重连均在此复位）。
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                tabs->setTabText(i, QStringLiteral("RDP: %1").arg(host));
                break;
            }
        }
    });
    connect(client, &RdpClient::disconnected, this, [this, panel]() {
        setTabConnected(panel, false);
        setStatus(tr("RDP 已断开：%1").arg(panel->client()->settings().host));
        // Tab 标题加“[断开]”前缀（面板内可重连，连上后由 connected 分支复位）。
        const QString prefix = tr("[断开] ");
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                const QString text = tabs->tabText(i);
                if (!text.startsWith(prefix))
                    tabs->setTabText(i, prefix + text);
                break;
            }
        }
    });

    if (!resolved.host.isEmpty()) {
        setStatus(tr("正在连接 RDP %1:%2…").arg(resolved.host).arg(resolved.port));
        // 直接以完整参数建连：面板的分辨率下拉框只有固定档位，动态计算出的
        // 分辨率不经表单回读，避免被就近档位截断。
        client->connectToHost(resolved);
    }
    panel->setFocus();
}

#endif // CUBESHELL_WITH_RDP

#ifdef CUBESHELL_WITH_SERIAL

// 打开串口标签页。portName 为空时只建空白面板（用户在工具栏选端口后点“连接”），
// 非空则立即建连。结构对照 openRdpTab。
void MainWindow::openSerialTab(const SerialSettings &settings)
{
    auto *panel = new SerialTerminalWidget(this);
    panel->setSettings(settings);

    const QString title = settings.portName.isEmpty()
                              ? tr("串口连接")
                              : QStringLiteral("Serial: %1").arg(settings.portName);
    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(panel, title);
    decorateSessionTab(pane, idx);
    setTabConnected(panel, false);   // 连上之前先亮红点
    pane->setCurrentIndex(idx);

    connect(panel, &SerialTerminalWidget::connected, this, [this, panel]() {
        setTabConnected(panel, true);
        const SerialSettings s = panel->client()->settings();
        setStatus(tr("串口已连接：%1 @%2 %3")
                      .arg(s.portName).arg(s.baudRate).arg(s.frameFormat()));
        // 标题跟随实际连接的端口（空白面板手选 / 断开后重连均在此复位）。
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                tabs->setTabText(i, QStringLiteral("Serial: %1").arg(s.portName));
                break;
            }
        }
    });
    connect(panel, &SerialTerminalWidget::disconnected, this, [this, panel]() {
        setTabConnected(panel, false);
        setStatus(tr("串口已断开：%1").arg(panel->client()->settings().portName));
        // Tab 标题加“[断开]”前缀（面板内可重连，连上后由 connected 分支复位）。
        const QString prefix = tr("[断开] ");
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                const QString text = tabs->tabText(i);
                if (!text.startsWith(prefix))
                    tabs->setTabText(i, prefix + text);
                break;
            }
        }
    });
    connect(panel, &SerialTerminalWidget::connectionFailed, this,
            [this](const QString &message) { setStatus(message); });

    if (!settings.portName.isEmpty()) {
        setStatus(tr("正在打开串口 %1…").arg(settings.portName));
        panel->connectToPort();
    }
    panel->setFocus();
}

#endif // CUBESHELL_WITH_SERIAL

// 打开 TCP/Telnet 标签页。host 为空时只建空白面板（用户在工具栏填主机后点
// “连接”），非空则立即建连。结构对照 openSerialTab。
void MainWindow::openNetTab(const TcpSettings &settings)
{
    // 协议名是专有名词，标题里保持原样不翻译（与 "Serial: xxx" 一致）。
    const QString label = settings.isTelnet() ? QStringLiteral("Telnet")
                                              : QStringLiteral("TCP");
    auto *panel = new NetTerminalWidget(settings.mode, this);
    panel->setSettings(settings);

    const QString title = settings.host.isEmpty()
                              ? tr("%1 连接").arg(label)
                              : QStringLiteral("%1: %2").arg(label, settings.host);
    TerminalTabWidget *pane = targetPane();
    const int idx = pane->addTab(panel, title);
    decorateSessionTab(pane, idx);
    setTabConnected(panel, false);   // 连上之前先亮红点
    pane->setCurrentIndex(idx);

    connect(panel, &NetTerminalWidget::connected, this, [this, panel, label]() {
        setTabConnected(panel, true);
        const TcpSettings s = panel->client()->settings();
        setStatus(tr("已连接：%1").arg(s.displayTarget()));
        // 标题跟随实际连上的目标（空白面板手填 / 断开后改地址重连均在此复位）。
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                tabs->setTabText(i, QStringLiteral("%1: %2").arg(label, s.host));
                break;
            }
        }
    });
    connect(panel, &NetTerminalWidget::disconnected, this, [this, panel]() {
        setTabConnected(panel, false);
        setStatus(tr("已断开：%1").arg(panel->client()->settings().displayTarget()));
        // Tab 标题加“[断开]”前缀（面板内可重连，连上后由 connected 分支复位）。
        const QString prefix = tr("[断开] ");
        for (QTabWidget *tabs : allPanes()) {
            const int i = tabs->indexOf(panel);
            if (i >= 0) {
                const QString text = tabs->tabText(i);
                if (!text.startsWith(prefix))
                    tabs->setTabText(i, prefix + text);
                break;
            }
        }
    });
    connect(panel, &NetTerminalWidget::connectionFailed, this,
            [this](const QString &message) { setStatus(message); });

    if (!settings.host.isEmpty()) {
        setStatus(tr("正在连接 %1…").arg(settings.displayTarget()));
        panel->connectToHost();
    }
    panel->setFocus();
}


// ---------------------------------------------------------------------------
// 对话框
// ---------------------------------------------------------------------------

// 对应Python: function/theme.py + cube-shell.py::show_language_settings
void MainWindow::showSettings(int tabIndex)
{
    SettingsDialog dlg(this);
    dlg.setCurrentTab(tabIndex);
    connect(&dlg, &SettingsDialog::fontChanged, this,
            [this](const QString &family, int pointSize) {
                // 即时应用到所有打开的终端。
                const QFont font(family, pointSize);
                const QList<QTermWidget *> terms = findChildren<QTermWidget *>();
                for (QTermWidget *t : terms)
                    t->setTerminalFont(font);
            });
    // 设备列表字号即时应用（不重建树，保留展开状态）。
    connect(&dlg, &SettingsDialog::deviceListFontSizeChanged, this,
            [this](int pointSize) {
                if (m_deviceList)
                    m_deviceList->setFontSize(pointSize);
            });
    // 切换 dark/light 后重建标签页 QSS（颜色按新主题取值），无需重启。
    connect(&dlg, &SettingsDialog::appearanceChanged, this, [this](const QString &) {
        for (TerminalTabWidget *tabs : std::as_const(m_panes))
            tabs->setStyleSheet(windowsTerminalTabStyle());
    });
    // 回滚行数即时应用到所有打开的终端。注意：Session 换 HistoryType 会丢弃
    // 已有的回滚内容（History.cpp 不做迁移），所以只在用户确实改了值时才下发。
    connect(&dlg, &SettingsDialog::scrollbackLinesChanged, this, [this](int lines) {
        const QList<QTermWidget *> terms = findChildren<QTermWidget *>();
        for (QTermWidget *t : terms)
            t->setHistorySize(lines);
    });
    dlg.exec();
}

// AI 设置对话框：关闭后刷新面板模型名，并把所有已缓存 Agent 的偏好重新从磁盘加载，
// 使新设置（模型/提供商/Base URL/API Key）无需重启立即生效。
// 对应Python: cube-shell.py::show_ai_settings（L2445-2463，两段都有 try/except 容错）
void MainWindow::showAiSettings()
{
    AiSettingsDialog dlg(this);
    dlg.exec();

    if (m_aiPanel)
        m_aiPanel->refreshModelLabel();

    const AiPreferences freshPrefs = AiPreferences::load();
    for (SshAiAgent *agent : std::as_const(m_aiAgents)) {
        if (agent)
            agent->setPreferences(freshPrefs);
    }
}

// 隧道管理对话框（TunnelConfigWidget + AddTunnelDialog）。
// 对应Python: cube-shell.py::Tunnel（隧道管理窗）
void MainWindow::showTunnelManager()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("SSH 隧道管理"));
    dlg.resize(640, 420);
    auto *widget = new TunnelConfigWidget(m_tunnelPool, &dlg);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(widget, 1);
    layout->addWidget(buttons);
    connect(widget, &TunnelConfigWidget::addTunnelRequested, this, [this, widget]() {
        addTunnel();
        widget->refresh();
    });
    dlg.exec();
}

// 对应Python: cube-shell.py::AddTunnelConfig.addTunnel（新增SSH隧道 Shift+Ctrl+S）
void MainWindow::addTunnel()
{
    QStringList deviceNames;
    for (const DeviceEntry &e : m_store.devices())
        deviceNames << e.name;
    AddTunnelDialog dlg(deviceNames, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_tunnelPool->setEntry(dlg.tunnelName(), dlg.entry());
    setStatus(tr("隧道“%1”已保存").arg(dlg.tunnelName()));
}

// 对应Python: function/about.py::AboutDialog
void MainWindow::showAbout()
{
    AboutDialog dlg(this);
    // 关于对话框的"检查更新"按钮复用帮助菜单的入口。
    connect(&dlg, &AboutDialog::checkUpdateRequested,
            this, &MainWindow::checkForUpdates);
    dlg.exec();
}

// 对应Python: cube-shell.py::linux（Linux常用命令 Shift+Ctrl+P）
void MainWindow::showLinuxCommands()
{
    LinuxCommandsDialog dlg(this);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// 更新检查
// ---------------------------------------------------------------------------

// 本机运行版本号 —— 取编译进二进制的 PROJECT_VERSION。
// 刻意不读 theme.json 的 "version"：该文件与 Python 版共用配置目录，
// 老配置里根本没有这个键（退化成 "0" → 每次都误报有新版本），
// 或者停在 Python 版的 2.8.0（→ 把 3.x 用户往回推）。
static QString localAppVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral(CUBESHELL_VERSION) : v;
}

// 手动触发检查更新（帮助菜单项 + 关于对话框按钮共用入口）。
// 对应Python: cube-shell.py::check_for_update + _on_update_checked
void MainWindow::checkForUpdates()
{
    if (!m_updateChecker) {
        m_updateChecker = new UpdateChecker(this);
        connect(m_updateChecker, &UpdateChecker::updateAvailable, this,
                [this](const QString &version, const QString &url, const QString &changelog) {
            setStatus(QString());
            // 有更新：展示确认对话框（版本/说明），确认后打开下载页。
            QString text = tr("发现新版本 v%1，是否立即下载？").arg(version);
            if (!changelog.isEmpty())
                text += QStringLiteral("\n\n") + changelog.left(500);
            const auto ret = QMessageBox::question(
                this, tr("检查更新"), text,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (ret == QMessageBox::Yes)
                QDesktopServices::openUrl(QUrl(url));
        });
        connect(m_updateChecker, &UpdateChecker::noUpdateAvailable, this, [this]() {
            setStatus(QString());
            QMessageBox::information(this, tr("检查更新"),
                                     tr("已是最新版本(v%1)。").arg(localAppVersion()));
        });
        connect(m_updateChecker, &UpdateChecker::checkFailed, this,
                [this](const QString &error) {
            setStatus(QString());
            QMessageBox::warning(this, tr("检查更新"), error);
        });
    }
    if (m_updateChecker->isChecking())
        return;   // 防重复触发，对应Python: _update_worker.isRunning() 判断
    setStatus(tr("正在检查更新…"));
    m_updateChecker->checkForUpdates(localAppVersion());
}

// ---------------------------------------------------------------------------
// 平台右键菜单集成（macOS Finder / Windows 资源管理器）
// ---------------------------------------------------------------------------

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
// 通用的右键菜单集成设置对话框（macOS / Windows 共用）。
// 对应Python: cube-shell.py::_show_context_menu_integration
void MainWindow::showContextMenuIntegration(
    const QString &title, const QString &description,
    const QString &successMsg, const QString &uninstallConfirm,
    const QString &uninstallDone, bool installed,
    const std::function<bool(QString *)> &install,
    const std::function<bool(QString *)> &uninstall)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.setFixedWidth(450);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(15);

    auto *titleLabel = new QLabel(title, &dialog);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold;"));
    layout->addWidget(titleLabel);

    auto *desc = new QLabel(description, &dialog);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    // 当前安装状态。
    auto *statusLabel = new QLabel(&dialog);
    if (installed) {
        statusLabel->setText(tr("● 当前状态：已安装"));
        statusLabel->setStyleSheet(QStringLiteral("color: green; font-weight: bold;"));
    } else {
        statusLabel->setText(tr("● 当前状态：未安装"));
        statusLabel->setStyleSheet(QStringLiteral("color: gray; font-weight: bold;"));
    }
    layout->addWidget(statusLabel);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    auto *actionBtn = new QPushButton(installed ? tr("卸载") : tr("安装"), &dialog);
    auto *closeBtn = new QPushButton(tr("关闭"), &dialog);
    btnLayout->addWidget(actionBtn);
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);

    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(actionBtn, &QPushButton::clicked, &dialog,
            [&dialog, installed, title, successMsg, uninstallConfirm,
             uninstallDone, install, uninstall]() {
        QString err;
        if (installed) {
            if (QMessageBox::question(&dialog, title, uninstallConfirm) != QMessageBox::Yes)
                return;
            if (uninstall(&err))
                QMessageBox::information(&dialog, title, uninstallDone);
            else
                QMessageBox::warning(&dialog, title,
                                     err.isEmpty() ? tr("卸载失败") : err);
        } else {
            if (install(&err))
                QMessageBox::information(&dialog, title, successMsg);
            else
                QMessageBox::warning(&dialog, title,
                                     err.isEmpty() ? tr("安装失败") : err);
        }
        dialog.accept();
    });
    dialog.exec();
}
#endif // Q_OS_MACOS || Q_OS_WIN

#ifdef Q_OS_MACOS
// 对应Python: cube-shell.py::show_finder_integration
void MainWindow::showFinderIntegration()
{
    if (!FinderIntegration::isSupported())
        return;
    showContextMenuIntegration(
        tr("Finder 右键菜单集成"),
        tr("安装后，你可以在 Finder 中右键点击文件夹，\n"
           "选择「快速操作 → 在 CubeShell 中打开终端」\n"
           "即可在当前窗口新建该目录的本地终端 Tab。"),
        tr("Finder 右键菜单已安装！\n\n"
           "现在你可以在 Finder 中右键点击文件夹，\n"
           "选择「快速操作 → 在 CubeShell 中打开终端」。\n\n"
           "提示：如果右键菜单中未显示，请在\n"
           "「系统设置 → 键盘 → 快捷键 → 服务」中确认已启用。"),
        tr("确定要卸载 Finder 右键菜单集成吗？"),
        tr("Finder 右键菜单已卸载。"),
        FinderIntegration::isInstalled(),
        [](QString *err) { return FinderIntegration::installFinderExtension(err); },
        [](QString *err) { return FinderIntegration::uninstallFinderExtension(err); });
}
#endif // Q_OS_MACOS

#ifdef Q_OS_WIN
// 对应Python: cube-shell.py::show_windows_integration
void MainWindow::showWindowsIntegration()
{
    if (!WindowsIntegration::isSupported())
        return;
    showContextMenuIntegration(
        tr("Windows 右键菜单集成"),
        tr("安装后，你可以在资源管理器中右键点击文件夹，\n"
           "选择「在 CubeShell 中打开终端」\n"
           "即可在当前窗口新建该目录的本地终端 Tab。"),
        tr("Windows 右键菜单已安装！\n\n"
           "现在你可以在资源管理器中右键点击文件夹，\n"
           "选择「在 CubeShell 中打开终端」。"),
        tr("确定要卸载 Windows 右键菜单集成吗？"),
        tr("Windows 右键菜单已卸载。"),
        WindowsIntegration::isInstalled(),
        [](QString *err) { return WindowsIntegration::install(err); },
        [](QString *err) { return WindowsIntegration::uninstall(err); });
}
#endif // Q_OS_WIN

// ---------------------------------------------------------------------------
// Docker 管理 / 左侧工具栏占位入口
// ---------------------------------------------------------------------------

#ifdef CUBESHELL_WITH_LOCALPROC
// 显示 Docker 对话框前刷新后端上下文：懒建 DockerManager，并把当前活动
// SSH 会话的 CommandExecutor（objectName "dockerExecutor"，会话内复用）
// 喂给它；无活动 SSH 连接时置空 executor，回到"未连接"行为。
// 对应Python: cube-shell.py:1039-1045 的 isConnected 判定 +
// showClaudeCodePanel 的 executor 复用模式（见上方 569-589 行段落）
// 鸿蒙（LOCALPROC=OFF）：DockerManager 不编译，本节整体摘除。
void MainWindow::ensureDockerManager()
{
    if (!m_dockerManager)
        m_dockerManager = new DockerManager(this);

    auto *session = qobject_cast<SshSessionTab *>(activeTabWidget()->currentWidget());
    std::shared_ptr<SshClient> client;
    if (session && session->terminal())
        client = session->terminal()->sshClient();
    if (!client) {
        // 当前不是 SSH 会话或尚未连上 → 与 Python 未连接时不带上下文一致。
        m_dockerExecutor = nullptr;
        m_dockerManager->setRemoteExecutor(nullptr);
        return;
    }

    // 每个会话复用同一个 executor（随 session tab 销毁，QPointer 自动失效）。
    auto *executor = session->findChild<CommandExecutor *>(
        QStringLiteral("dockerExecutor"));
    if (!executor) {
        executor = new CommandExecutor(client.get(), session);
        executor->setObjectName(QStringLiteral("dockerExecutor"));
    }
    m_dockerExecutor = executor;
    m_dockerManager->setRemoteExecutor(executor);
    m_dockerManager->setRemoteUser(session->device().username);
}

// 对应Python: cube-shell.py:1039-1045 showDockerManagerDialog
void MainWindow::showDockerManager()
{
    ensureDockerManager();
    if (!m_dockerManagerDialog) {
        m_dockerManagerDialog = new DockerManagerDialog(m_dockerManager, this);
        // docker exec / docker logs 命令转发到当前标签页的终端执行
        // （命令字符串已带 \n 结尾；Windows ConPTY 需要 \r 才能执行）。
        // 对应Python: cube-shell.py:3767-3785 terminal.sendText
        connect(m_dockerManagerDialog, &DockerManagerDialog::terminalCommandRequested,
                this, [this](const QString &command) {
                    QWidget *w = activeTabWidget()->currentWidget();
                    QTermWidget *term = qobject_cast<QTermWidget *>(w);
                    if (!term && w)
                        term = w->findChild<QTermWidget *>();
                    if (!term)
                        return;
#ifdef Q_OS_WIN
                    QString cmd = command;
                    if (cmd.endsWith(QLatin1Char('\n')))
                        cmd.chop(1);
                    term->sendText(cmd + QStringLiteral("\r"));
#else
                    term->sendText(command);
#endif
                });
    }
    // Python 仅在 isConnected 时刷新（cube-shell.py:1041-1042）；这里无论
    // 是否连接都刷新——未设 executor 时后端直接回空列表，对话框显示
    // "没有可用的docker容器" 占位行，既避免残留上一会话的过期容器列表，
    // 也不让首次打开时呈现空白树。
    m_dockerManagerDialog->refreshInfo();
    m_dockerManagerDialog->show();
    m_dockerManagerDialog->raise();
    m_dockerManagerDialog->activateWindow();
}

// 对应Python: cube-shell.py:1069-1077 showDockerSoftDialog
void MainWindow::showDockerSoft()
{
    ensureDockerManager();
    if (!m_dockerSoftDialog)
        m_dockerSoftDialog = new DockerSoftDialog(m_dockerManager, this);
    // 每次显示前都刷新。对应Python: cube-shell.py:1071-1074
    m_dockerSoftDialog->refreshInfo();
    m_dockerSoftDialog->show();
    m_dockerSoftDialog->raise();
    m_dockerSoftDialog->activateWindow();
}

// 对应Python: cube-shell.py::showNATDialog + _ensure_nat_dialog
void MainWindow::showNatDialog()
{
    // 懒加载：FrpManager 对应 Python 的 get_frp_manager() 单例，
    // NatDialog 对应 self._nat_dialog。
    if (!m_frpManager)
        m_frpManager = new FrpManager(this);
    if (!m_natDialog)
        m_natDialog = new NatDialog(m_frpManager, &m_store, this);
    // 对应Python: dlg.show(); dlg.raise_(); dlg.activateWindow()
    m_natDialog->show();
    m_natDialog->raise();
    m_natDialog->activateWindow();
}
#endif // CUBESHELL_WITH_LOCALPROC

// 对应Python: cube-shell.py:1390-1399 showProcessManagerDialog
void MainWindow::showProcessManager()
{
    // 当前活动 SSH 会话的 CommandExecutor（objectName "processExecutor"，
    // 会话内复用）；不是 SSH 会话或尚未连上时置空，对话框自行显示
    // "未连接 SSH 服务器"。
    CommandExecutor *executor = nullptr;
    auto *session = qobject_cast<SshSessionTab *>(activeTabWidget()->currentWidget());
    std::shared_ptr<SshClient> client;
    if (session && session->terminal())
        client = session->terminal()->sshClient();
    if (client) {
        executor = session->findChild<CommandExecutor *>(
            QStringLiteral("processExecutor"));
        if (!executor) {
            executor = new CommandExecutor(client.get(), session);
            executor->setObjectName(QStringLiteral("processExecutor"));
        }
    }

    if (!m_processManagerDialog)
        m_processManagerDialog = new ProcessManagerDialog(nullptr, this);
    m_processManagerDialog->setExecutor(executor);
    m_processExecutor = executor;
    // Python 仅在 isConnected 时刷新（cube-shell.py:1392-1396）。
    if (executor)
        m_processManagerDialog->refresh();
    m_processManagerDialog->show();
    m_processManagerDialog->raise();
    m_processManagerDialog->activateWindow();
}

} // namespace cubeshell
