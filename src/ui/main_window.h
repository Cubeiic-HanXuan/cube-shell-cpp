#pragma once

// main_window.h — 主窗口：设备列表 + 多标签终端 + 分屏 + 菜单/工具/状态栏。
// 对应Python: cube-shell.py::MainWindow（主窗组装 + menuBarController）

#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <functional>

#include "config/DeviceConfigStore.h"
// ChatMode 用于槽签名、AiCommand 用于待确认命令缓存 — 均需完整类型。
#include "ai/AiChatPanel.h"

class QTabWidget;
class QSplitter;
class QStackedWidget;
class QLabel;
class QDockWidget;
class QTermWidget;

namespace cubeshell {

class AiChatWorker;
class DeviceListWidget;
class LocalFileBrowserWidget;
class TerminalTabWidget;
class UpdateChecker;
class SshSessionTab;
class TunnelPool;
class BastionClient;
class CommandExecutor;
class DockerManager;
class DockerManagerDialog;
class DockerSoftDialog;
class ProcessManagerDialog;
class NatDialog;
class FrpManager;
class SshAiAgent;
class StatusBoxItem;
struct BastionConnectParams;
struct RemoteStats;
#ifdef CUBESHELL_WITH_RDP
struct RdpSettings;
#endif
#ifdef CUBESHELL_WITH_SERIAL
struct SerialSettings;
#endif
// TCP/Telnet 无条件可用（Qt6::Network 是顶层必需组件）。
struct TcpSettings;
class NetTerminalWidget;

// Main application window.
//
// Split view: device list (left) + terminal tab widget (right, twin panes for
// split view). This is the C++ counterpart of the main window assembled in
// cube-shell.py.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // jms:// / ssh:// URL 事件入口（命令行参数 / macOS QFileOpenEvent）。
    // 对应Python: cube-shell.py 里 BastionClient.handle_url 的调用点
    void handleUrl(const QString &url);

#ifdef CUBESHELL_WITH_RDP
    // 打开 RDP 远程桌面标签页（rdp:// URL 分发 / “新建 RDP 连接”菜单共用入口）。
    // host 为空时创建空白面板等待用户填写表单；非空则按动态计算的分辨率立即建连。
    // 对应Python: cube-shell.py::open_rdp_tab（行 1640-1691）
    void openRdpTab(const RdpSettings &settings);
#endif

#ifdef CUBESHELL_WITH_SERIAL
    // 打开串口标签页（“新建串口连接”菜单 / 设备列表中 protocol=="serial" 共用入口）。
    // portName 为空时创建空白面板等待用户在工具栏选端口；非空则立即建连。
    void openSerialTab(const SerialSettings &settings);
#endif

    // 打开 TCP/Telnet 标签页（“新建 Telnet/TCP 连接”菜单 / 设备列表中
    // protocol=="telnet"|"tcp" / telnet:// URL 共用入口）。
    // host 为空时创建空白面板等待用户在工具栏填写；非空则立即建连。
    // 无 #ifdef：TCP/Telnet 不依赖任何可选组件。
    void openNetTab(const TcpSettings &settings);

private:
    void setupUi();
    void setupMenus();
    void setupToolbar();
    void setupStatusBar();
    void setupShortcuts();
    void setupAiDock();
    void setupTunnels();
    void setupBastion();

    void loadDevices();
    // 明文密码 → 钥匙串的一次性迁移（幂等；已迁移过则直接返回）。
    void migrateSecrets();
    void refreshDeviceList();
    bool saveDevices();

#ifdef CUBESHELL_WITH_LOCALPTY
    void openLocalTerminal();
    // 以 dir 为工作目录新开本机终端（文件树右键“新建位于文件夹位置的终端窗口”）。
    // 返回新建的终端（openClaudeTerminal 需要向其发送命令）。
    // 对应Python: cube-shell.py::open_local_terminal_in_selected_folder
    QTermWidget *openLocalTerminalAt(const QString &dir);
    // 在 path 目录新开本机终端，command 非空时在 shell 就绪后自动执行。
    // cubeshell://open-local 的落地点（Finder 快速操作 / Windows 右键菜单）。
    // 对应Python: cube-shell.py::open_local_terminal_at_path
    void openLocalTerminalAtPath(const QString &path, const QString &command = QString());
#endif
    void openSshSession(const DeviceEntry &device);
    void setStatus(const QString &text);

    // 首页标签内容：居中的快捷键提示清单。
    // 对应Python: ui/main.py 的 self.index（label_7/9/11/12/13/14/15）
    QWidget *createHomePage();
    // 给会话标签装上左侧状态圆点 + 右侧 ✕ 关闭按钮。
    // 对应Python: cube-shell.py 里 setTabButton(TabStatusDot / TabCloseButton)
    void decorateSessionTab(QTabWidget *tabs, int index);
    // 切换某个标签页的连接状态圆点颜色。
    void setTabConnected(QWidget *page, bool connected);

    void addDevice();
    void editDevice(const QString &name);
    void removeDevice(const QString &name);
    void exportDevices();
    void importDevices();

    // --- 标签/分屏 ---
    void closeTabIn(QTabWidget *tabs, int index);
    void closeCurrentTab();
    void nextTab();
    void prevTab();
    void showTabContextMenu(QTabWidget *tabs, const QPoint &pos);
    // 把 tabs 里 index 处的标签拆到一个新建的相邻分屏（orientation 决定水平/垂直）。
    // 多分屏：每次调用都新建一个 pane，而非在固定的第二个 pane 之间来回搬。
    void splitTab(QTabWidget *source, int index, Qt::Orientation orientation);
    // 新建一个 TerminalTabWidget 并接好全部信号（分屏的构造入口）。
    TerminalTabWidget *createPane();
    // 在 source 所在的 splitter 中，于 source 之后插入 pane。
    // 若该 splitter 方向与 orientation 不符且不止一个子控件，则把 source
    // 就地替换为一个新的子 splitter（嵌套），实现任意方向的自由分屏。
    void insertPaneNextTo(QTabWidget *source, TerminalTabWidget *pane,
                          Qt::Orientation orientation);
    // 移除空 pane（首个 pane 常驻），并折叠只剩一个子控件的中间 splitter。
    void pruneEmptyPanes();
    // 把 splitter 的子控件尺寸重新均分。
    static void equalizeSplitter(QSplitter *splitter);
    // 焦点/激活 pane 管理：activeTabWidget 返回最后获得焦点的 pane。
    QTabWidget *activeTabWidget() const;
    // 当前活动 pane 的当前标签页里的终端；不是终端标签（首页/编辑器等）时返回 nullptr。
    QTermWidget *currentTerminal() const;
    void setActivePane(TerminalTabWidget *pane);
    // 焦点在分屏之间循环切换（Ctrl+Alt+方向 / 菜单）。
    void focusNextPane(int delta);
    // 全部分屏合并回第一个 pane。
    void mergeAllPanes();
    // 遍历所有 pane（替代原先写死的 {m_tabs, m_tabs2} 循环）。
    QList<QTabWidget *> allPanes() const;
    // 查找持有 page 的 pane，找不到返回 nullptr。
    QTabWidget *paneOf(QWidget *page) const;
    // 新标签页的落点：当前活动 pane（无则首个 pane）。
    TerminalTabWidget *targetPane() const;
    void updateTerminalInfo();

    // --- 左侧文件浏览器（设备列表下方,随当前标签切换） ---
    // 对应Python: 连接后左侧 treeWidget 展示 SFTP/本地文件目录
    void updateLeftPanel(QTabWidget *pane = nullptr);
    // 刷新文件浏览器的分屏徽章：在路径栏左侧显示归属的”分屏 N”序号。
    // 多分屏下左栏被所有分屏共享，不标明归属时无法分辨当前看的是谁的目录。
    void updatePaneIndicator(QTabWidget *pane, QWidget *page);
    // 重刷全部标签的徽章。徽章是否可见取决于分屏总数，分屏增减时必须整体重算，
    // 否则要等用户切一次标签徽章才更新。
    void refreshPaneIndicators();
    // 给活动分屏的标签栏加高亮边框，非活动分屏淡化 —— 与标题栏配合，
    // 让“哪个分屏是当前焦点”一眼可见。
    void updatePaneHighlight();
    // pane 在 m_panes 中的序号（从 1 开始，用于界面展示）；找不到返回 0。
    int paneNumber(QTabWidget *pane) const;
    // follow_folder 勾选时把当前浏览器同步到终端 cwd。
    // 对应Python: _on_follow_folder_changed → refreshDirs
    void syncBrowserToTerminalCwd();

    // --- 远程监控（状态栏 8 项指标） ---
    void bindMonitorToTab(QWidget *tabWidget);
    // 状态栏 8 项指标更新/复位。对应Python: refresh_status_bar
    void updateStatusStats(const RemoteStats &stats);
    void resetStatusItems();

    // --- AI / Hermes / Claude Code 面板 ---
    // 对应Python: cube-shell.py::_toggle_ai_panel / showHermesPanel / showClaudeCodePanel
    void toggleAiPanel();
    // AI 面板绑定到当前活动 SSH 会话（懒建 per-session Agent 并路由信号）。
    // 对应Python: cube-shell.py::_connect_ai_to_current_tab（行 5639-5695）
    void connectAiToCurrentTab();
    // 断开 agent → 面板的全部信号连接。
    void disconnectAiFromAgent(SshAiAgent *agent);
    // 面板输入区回调（按 ChatMode 路由到 SshAiAgent 或 AiChatWorker）。
    // 对应Python: cube-shell.py::_on_ai_user_message 等
    void onAiUserMessage(const QString &text);
    void onAiChatModeChanged(AiChatPanel::ChatMode mode);
    void onAiStopRequested();
    void onAiClearRequested();
    void onAiCommandExecuteRequested(const QString &cmd);
    // 命令就绪分流：安全命令自动整批执行，危险命令弹确认对话框。
    // agent 为发出 commandReady 的会话 agent — 命令必须回到该 agent 执行，
    // 不能用 m_activeAiAgent（弹窗期间用户切标签会导致跨主机误执行）。
    // 对应Python: cube-shell.py::_show_confirm_dialog（行 5719-5746）
    void onAiCommandReady(const QList<AiCommand> &commands, SshAiAgent *agent);
    // "全部执行"：命令列表整批下发到产生命令的 agent 逐条执行。
    // 对应Python: cube-shell.py::_on_commands_approved（行 5748-5755）
    void onCommandsApproved(const QList<AiCommand> &commands, SshAiAgent *agent);
    // "逐条确认"：逐条弹窗审批，批准的命令在同一 agent 上整批执行。
    // 对应Python: cube-shell.py::_on_commands_step_mode（行 5757-5786）
    void onCommandsStepMode(const QList<AiCommand> &commands, SshAiAgent *agent);
    void showHermesPanel();
    void showClaudeCodePanel();
#ifdef CUBESHELL_WITH_LOCALPTY
    // 在新本机终端中执行 claude 命令（Claude Code 面板的 openTerminalRequested）。
    // 对应Python: cube-shell.py::open_claude_terminal（行 1314-1337）
    void openClaudeTerminal(const QString &command);
#endif
    // 根据 claude 命令语义生成终端 Tab 名称。
    // 对应Python: cube-shell.py::_claude_tab_name（行 1282-1300）
    static QString claudeTabName(const QString &command);

    // --- 对话框 ---
    void showSettings();
    // 对应Python: cube-shell.py::show_ai_settings（L2445）
    void showAiSettings();
    void showTunnelManager();
    void showAbout();
    void addTunnel();
    // 对应Python: cube-shell.py::linux（帮助菜单 "Linux常用命令"）
    void showLinuxCommands();

    // --- 更新检查 ---
    // 对应Python: cube-shell.py::check_for_update（帮助菜单 + 关于对话框共用入口）
    void checkForUpdates();

    // --- 平台右键菜单集成 ---
    // 对应Python: cube-shell.py::_show_context_menu_integration（macOS/Windows 共用）
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    void showContextMenuIntegration(
        const QString &title, const QString &description,
        const QString &successMsg, const QString &uninstallConfirm,
        const QString &uninstallDone, bool installed,
        const std::function<bool(QString *)> &install,
        const std::function<bool(QString *)> &uninstall);
#endif
#ifdef Q_OS_MACOS
    // 对应Python: cube-shell.py::show_finder_integration
    void showFinderIntegration();
#endif
#ifdef Q_OS_WIN
    // 对应Python: cube-shell.py::show_windows_integration
    void showWindowsIntegration();
#endif

    // 左侧工具栏入口（未移植的工具先占位）。
    // 对应Python: cube-shell.py::setupLeftToolbar 绑定的各 show*Dialog
#ifdef CUBESHELL_WITH_LOCALPROC
    void showDockerManager();
    void showDockerSoft();
    void showNatDialog();
#endif
    void showProcessManager();

#ifdef CUBESHELL_WITH_LOCALPROC
    // 显示 Docker 对话框前刷新后端上下文：懒建 DockerManager，并把当前
    // 活动 SSH 会话的 CommandExecutor 喂给它（无活动连接则置空回本地态）。
    // 对应Python: cube-shell.py:1007-1077 懒加载 + isConnected 上下文
    void ensureDockerManager();
#endif

    void onBastionConnect(const BastionConnectParams &params);

    QSplitter *m_splitter = nullptr;       // 左栏 | 终端区
    QSplitter *m_leftSplitter = nullptr;   // 左栏：设备列表 / 文件浏览器（上下）
    QStackedWidget *m_browserStack = nullptr; // 每个会话 Tab 一页文件浏览器
    QSplitter *m_termSplitter = nullptr;   // 顶层分屏容器（可嵌套子 splitter 实现多方向分屏）
    DeviceListWidget *m_deviceList = nullptr;
    QList<TerminalTabWidget *> m_panes;    // 全部分屏面板（首个常驻，其余按需创建/销毁）
    QPointer<TerminalTabWidget> m_activePane; // 最后获得焦点的 pane
    QWidget *m_homePage = nullptr;         // 首页标签（index 0，不可关闭/移动）
    QLabel *m_statusBar = nullptr;
    QLabel *m_termSizeLabel = nullptr;
    QLabel *m_encodingLabel = nullptr;

    // 状态栏 8 项指标（MobaXterm 风格小方块）。
    // 对应Python: cube-shell.py::setupStatusBar 的 _status_* StatusBoxItem
    StatusBoxItem *m_statusHostname = nullptr;
    StatusBoxItem *m_statusCpu = nullptr;
    StatusBoxItem *m_statusMem = nullptr;
    StatusBoxItem *m_statusUpload = nullptr;
    StatusBoxItem *m_statusDownload = nullptr;
    StatusBoxItem *m_statusUptime = nullptr;
    StatusBoxItem *m_statusUser = nullptr;
    StatusBoxItem *m_statusDisk = nullptr;

    QDockWidget *m_aiDock = nullptr;       // AI 助手停靠面板（默认隐藏）
    AiChatPanel *m_aiPanel = nullptr;
    SshSessionTab *m_monitorTab = nullptr; // 当前订阅监控的会话标签

    TunnelPool *m_tunnelPool = nullptr;
    BastionClient *m_bastion = nullptr;
    UpdateChecker *m_updateChecker = nullptr;   // 惰性创建（首次检查更新时）

    // Docker 管理（全部懒加载）。
    // 对应Python: cube-shell.py:1007-1077 _docker_manager_dialog / _docker_soft_dialog
#ifdef CUBESHELL_WITH_LOCALPROC
    DockerManager *m_dockerManager = nullptr;
    DockerManagerDialog *m_dockerManagerDialog = nullptr;
    DockerSoftDialog *m_dockerSoftDialog = nullptr;
    // DockerManager 侧持裸指针；QPointer 守卫会话销毁后的悬垂 executor。
    QPointer<CommandExecutor> m_dockerExecutor;
#endif

    // 远程进程管理（懒加载）。
    // 对应Python: cube-shell.py:1390-1399 showProcessManagerDialog
    ProcessManagerDialog *m_processManagerDialog = nullptr;
    QPointer<CommandExecutor> m_processExecutor;

    // 内网穿透（懒加载）。
    // 对应Python: cube-shell.py::_ensure_nat_dialog 的 self._nat_dialog
    //           + core/frp_manager.py::get_frp_manager 单例
#ifdef CUBESHELL_WITH_LOCALPROC
    NatDialog *m_natDialog = nullptr;
    FrpManager *m_frpManager = nullptr;
#endif

    // AI 助手集成：每个 SSH 会话标签一个 SshAiAgent（懒建，parent 为会话标签）。
    // 对应Python: cube-shell.py:5605-5695 _ai_agents 管理
    QHash<SshSessionTab *, SshAiAgent *> m_aiAgents;
    // MainWindow 正在析构标志：退出时 ~QWidget 删子对象会触发各 tab 的 destroyed
    // lambda 访问 m_aiAgents，但此刻 hash 已处销毁中状态，remove 会 UAF 崩溃。
    // 置位后 lambda 直接返回——退出时 hash 随 MainWindow 整体销毁，无需逐项 remove。
    bool m_destroying = false;
    // QPointer 守卫会话在 closeTabIn 之外被销毁（应用退出）时的悬垂。
    QPointer<SshAiAgent> m_activeAiAgent;
    AiChatWorker *m_plainChatWorker = nullptr;   // 普通聊天模式（无工具）
    // 对应Python: ui.follow_folder 复选框（OSC7 cwd 跟随 + SFTP 联动）
    bool m_followFolder = false;
    // 会话 Tab 页 → 左侧文件浏览器（SftpBrowserWidget / LocalFileBrowserWidget）。
    QHash<QWidget *, QWidget *> m_tabBrowsers;
    bool m_leftBrowserSized = false;   // 首次展开时才设左栏上下比例

    DeviceConfigStore m_store;
    QString m_configPath;   // where the pickle was loaded from (informational)
    QString m_jsonPath;     // where devices are saved (JSON, forward format)
};

} // namespace cubeshell
