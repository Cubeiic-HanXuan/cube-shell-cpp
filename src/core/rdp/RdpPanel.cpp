// RdpPanel.cpp — RDP 连接面板实现。
// 对应Python: core/rdp/rdp_client.py::RDPWidget（UI/画面显示部分，简化版）

#include "RdpPanel.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace cubeshell {

// ---------------------------------------------------------------------------
// 键盘映射表
// ---------------------------------------------------------------------------

// 对应Python: _QTKEY_TO_PS2 (lines 742-762)
// Qt::Key → PS/2 Set1 物理扫描码（US/QWERTY 物理位置）。macOS 下可打印键走物理
// 扫描码而非 Unicode 事件，远程 IME 才能拦截转换（Unicode 事件会绕过 IME 直接落字）。
// 不用 nativeScanCode()：macOS 上 Qt 常返回 0，会让所有键查成同一个扫描码。
static const QHash<int, quint16> s_qtkeyToPs2 = {
    {Qt::Key_Space, 57},
    {Qt::Key_A, 30}, {Qt::Key_B, 48}, {Qt::Key_C, 46}, {Qt::Key_D, 32},
    {Qt::Key_E, 18}, {Qt::Key_F, 33}, {Qt::Key_G, 34}, {Qt::Key_H, 35},
    {Qt::Key_I, 23}, {Qt::Key_J, 36}, {Qt::Key_K, 37}, {Qt::Key_L, 38},
    {Qt::Key_M, 50}, {Qt::Key_N, 49}, {Qt::Key_O, 24}, {Qt::Key_P, 25},
    {Qt::Key_Q, 16}, {Qt::Key_R, 19}, {Qt::Key_S, 31}, {Qt::Key_T, 20},
    {Qt::Key_U, 22}, {Qt::Key_V, 47}, {Qt::Key_W, 17}, {Qt::Key_X, 45},
    {Qt::Key_Y, 21}, {Qt::Key_Z, 44},
    {Qt::Key_0, 11}, {Qt::Key_1, 2}, {Qt::Key_2, 3}, {Qt::Key_3, 4},
    {Qt::Key_4, 5}, {Qt::Key_5, 6}, {Qt::Key_6, 7}, {Qt::Key_7, 8},
    {Qt::Key_8, 9}, {Qt::Key_9, 10},
    {Qt::Key_QuoteLeft, 41}, {Qt::Key_Minus, 12}, {Qt::Key_Equal, 13},
    {Qt::Key_BracketLeft, 26}, {Qt::Key_BracketRight, 27},
    {Qt::Key_Semicolon, 39}, {Qt::Key_Apostrophe, 40},
    {Qt::Key_Backslash, 43}, {Qt::Key_Comma, 51}, {Qt::Key_Period, 52},
    {Qt::Key_Slash, 53},
};

// 对应Python: _NON_EXTENDED_VK_SC (lines 725-733)
// 非扩展键的扫描码：Backspace/Tab/Esc/Enter/F1-F12/修饰键等不带 E0 前缀，
// 否则服务端无法识别（macOS delete 键在远程编辑器失效的根因）。
// 注：此表的 Qt::Key_Minus=74 是小键盘减号（VK_SUBTRACT），与 s_qtkeyToPs2 中
// 主键盘减号 12 不冲突——后者仅 macOS 可打印键路径使用，且在本表之前命中。
static const QHash<int, quint16> s_nonExtendedScancodes = {
    {Qt::Key_Backspace, 14}, {Qt::Key_Escape, 1}, {Qt::Key_Tab, 15}, {Qt::Key_Return, 28},
    {Qt::Key_F1, 59}, {Qt::Key_F2, 60}, {Qt::Key_F3, 61}, {Qt::Key_F4, 62},
    {Qt::Key_F5, 63}, {Qt::Key_F6, 64}, {Qt::Key_F7, 65}, {Qt::Key_F8, 66},
    {Qt::Key_F9, 67}, {Qt::Key_F10, 68}, {Qt::Key_F11, 87}, {Qt::Key_F12, 88},
    {Qt::Key_Shift, 42}, {Qt::Key_Control, 29}, {Qt::Key_Alt, 56},
    {Qt::Key_ScrollLock, 70}, {Qt::Key_NumLock, 69}, {Qt::Key_CapsLock, 58},
    {Qt::Key_Asterisk, 55}, {Qt::Key_Plus, 78}, {Qt::Key_Minus, 74},
};

// 对应Python: qtkey_to_vk (lines 603-632) 中的扩展键
// 这些键需要 E0 前缀（isExtended=true），值为扫描码。
static const QHash<int, quint16> s_extendedKeyScancodes = {
    {Qt::Key_Up, 0x48}, {Qt::Key_Down, 0x50},
    {Qt::Key_Left, 0x4B}, {Qt::Key_Right, 0x4D},
    {Qt::Key_Home, 0x47}, {Qt::Key_End, 0x4F},
    {Qt::Key_PageUp, 0x49}, {Qt::Key_PageDown, 0x51},
    {Qt::Key_Insert, 0x52}, {Qt::Key_Delete, 0x53},
    {Qt::Key_Print, 0x37}, {Qt::Key_Pause, 0x45},
    {Qt::Key_Meta, 0x5B},   // Left Win
    {Qt::Key_Menu, 0x5D},   // Apps key
};

// ---------------------------------------------------------------------------
// 鼠标/滚轮标志（对应 FreeRDP 的 PTR_FLAGS_*，Python 侧由 MOUSEBUTTON 枚举表达）
// ---------------------------------------------------------------------------
static constexpr quint16 PTR_FLAGS_MOVE           = 0x0800;
static constexpr quint16 PTR_FLAGS_DOWN           = 0x8000;
static constexpr quint16 PTR_FLAGS_BUTTON1        = 0x1000;  // 左键
static constexpr quint16 PTR_FLAGS_BUTTON2        = 0x2000;  // 右键
static constexpr quint16 PTR_FLAGS_BUTTON3        = 0x4000;  // 中键
static constexpr quint16 PTR_FLAGS_WHEEL          = 0x0200;
static constexpr quint16 PTR_FLAGS_WHEEL_NEGATIVE = 0x0100;
// 旋转量放在低 8 位（有符号）：+120 → 0x78，-120 → 0x88（补码）
static constexpr quint16 WHEEL_ROTATION_POSITIVE  = 0x0078;
static constexpr quint16 WHEEL_ROTATION_NEGATIVE  = 0x0088;

// ---------------------------------------------------------------------------
// 分辨率
// ---------------------------------------------------------------------------

// QSettings 键（组织/应用名见 app/main.cpp 的 setOrganizationName）。
// 命名沿用 ui/dialogs/SettingsDialog.cpp 的 "分组/键" 习惯。
namespace {
const char kFitToWindowKey[] = "rdp/fit_to_window";
const char kResolutionKey[]  = "rdp/resolution";
const char kClipboardSyncKey[] = "rdp/clipboard_sync";

// 常用档位。可编辑下拉框，用户可直接手填任意 "宽x高"，这里只是省事的预设。
const char *const kResolutionPresets[] = {
    "1024x768", "1280x720",  "1280x800",  "1366x768",  "1440x900",  "1600x900",
    "1680x1050", "1920x1080", "1920x1200", "2560x1440", "3840x2160",
};

// 与 RdpClient 里写入 settings 前的夹取区间保持一致。
constexpr int kMinWidth  = 640;
constexpr int kMinHeight = 480;
constexpr int kMaxWidth  = 4096;
constexpr int kMaxHeight = 2304;

QString formatResolution(QSize size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}
} // namespace

QSize RdpPanel::alignResolution(QSize size)
{
    // 宽 4 的倍数、高 2 的倍数：[MS-RDPBCGR] 对桌面尺寸的要求，也与
    // RdpClient::runConnection 里写入 FreeRDP_DesktopWidth/Height 前的夹取一致。
    int w = qBound(kMinWidth, size.width(), kMaxWidth) & ~3;
    int h = qBound(kMinHeight, size.height(), kMaxHeight) & ~1;
    return QSize(w, h);
}

QSize RdpPanel::parseResolution(const QString &text)
{
    // "1920x1080" / "1920X1080" / " 1920 x 1080 " 都收。整串必须只有这两个数字，
    // 否则宁可判非法退回 "适应窗口"，也不要猜出一个用户没想要的尺寸。
    const QStringList parts = text.split(QLatin1Char('x'), Qt::SkipEmptyParts,
                                         Qt::CaseInsensitive);
    if (parts.size() != 2)
        return QSize();
    bool okW = false;
    bool okH = false;
    const int w = parts.at(0).trimmed().toInt(&okW);
    const int h = parts.at(1).trimmed().toInt(&okH);
    if (!okW || !okH || w <= 0 || h <= 0)
        return QSize();
    return QSize(w, h);
}

RdpPanel::RdpPanel(QWidget *parent)
    : QWidget(parent)
    , m_client(new RdpClient(this))
{
    // ---------------- 连接参数表单 ----------------
    auto *form = new QFormLayout();
    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setPlaceholderText(QStringLiteral("192.168.1.100"));
    form->addRow(tr("主机"), m_hostEdit);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(3389);
    form->addRow(tr("端口"), m_portSpin);

    m_userEdit = new QLineEdit(this);
    form->addRow(tr("用户名"), m_userEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("密码"), m_passwordEdit);

    m_domainEdit = new QLineEdit(this);
    form->addRow(tr("域"), m_domainEdit);

    // ---------------- 会话控制条（连接后不隐藏） ----------------
    // 分辨率必须能在会话中改——"连上之后没有调整分辨率的位置" 是用户反馈的问题。
    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->setEditable(true);   // 档位之外可手填，如 "2048x1152"
    m_resolutionCombo->setInsertPolicy(QComboBox::NoInsert);
    for (const char *preset : kResolutionPresets)
        m_resolutionCombo->addItem(QString::fromLatin1(preset));
    m_resolutionCombo->setToolTip(tr("远程桌面分辨率，可直接输入 宽x高"));

    m_fitCheck = new QCheckBox(tr("适应窗口"), this);
    m_fitCheck->setToolTip(tr("分辨率跟随面板尺寸：远端 1 像素对本地 1 像素，"
                              "画面不经缩放最清晰"));

    m_applyButton = new QPushButton(tr("应用"), this);
    m_applyButton->setToolTip(tr("按新分辨率重连当前会话（RDP 无法在会话中直接改分辨率）"));
    m_applyButton->setEnabled(false);
    m_connectButton = new QPushButton(tr("连接"), this);
    m_disconnectButton = new QPushButton(tr("断开"), this);
    m_disconnectButton->setEnabled(false);

    // 剪贴板同步（cliprdr 通道）。默认开，照 mstsc 的习惯；开关改了要重连才生效，
    // 因为通道插件是建连期按 FreeRDP_RedirectClipboard 加载的（见 RdpClient.cpp）。
    m_clipboardSyncCheck = new QCheckBox(tr("剪贴板同步"), this);
    m_clipboardSyncCheck->setChecked(true);
    m_clipboardSyncCheck->setToolTip(
        tr("本机与远端共享剪贴板（文本与文件）。\n"
           "会把本机复制的内容送到远端主机；改动需重连生效。"));

    // 远端复制文件后自动传回本机（见 shouldAutoFetch）。这个按钮只在体积超限、
    // 需要用户确认时才出现，所以初始隐藏。
    m_fetchFilesButton = new QPushButton(this);
    m_fetchFilesButton->setToolTip(
        tr("把远端剪贴板里的文件传回本机临时目录，完成后可在文件管理器里粘贴"));
    m_fetchFilesButton->setVisible(false);

    // 按钮不收焦点：面板是 StrongFocus 的（键鼠要转发给远端），按钮抢了焦点后
    // 空格/回车会打在按钮上而不是远程桌面。
    for (QPushButton *b : {m_applyButton, m_connectButton, m_disconnectButton,
                           m_fetchFilesButton})
        b->setFocusPolicy(Qt::NoFocus);
    m_fitCheck->setFocusPolicy(Qt::NoFocus);
    m_clipboardSyncCheck->setFocusPolicy(Qt::NoFocus);

    m_statusLabel = new QLabel(tr("未连接"), this);
    // 状态文字与控件同排：sizeHint 交给布局忽略，否则一条长状态
    //（"…服务端未采用 1920x1080"）会把面板最小宽度顶起来，连带撑大主窗口。
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_controlBar = new QWidget(this);
    auto *controlLayout = new QHBoxLayout(m_controlBar);
    controlLayout->setContentsMargins(6, 3, 6, 3);
    controlLayout->addWidget(new QLabel(tr("分辨率"), m_controlBar));
    controlLayout->addWidget(m_resolutionCombo);
    controlLayout->addWidget(m_fitCheck);
    controlLayout->addWidget(m_applyButton);
    controlLayout->addWidget(m_clipboardSyncCheck);
    controlLayout->addWidget(m_fetchFilesButton);
    controlLayout->addWidget(m_connectButton);
    controlLayout->addWidget(m_disconnectButton);
    controlLayout->addWidget(m_statusLabel, /*stretch=*/1);

    // 画面显示区。对应Python: RDPWidget._label（等比缩放居中，深色底）
    m_canvas = new QLabel(this);
    m_canvas->setMinimumSize(1, 1);
    m_canvas->setAlignment(Qt::AlignCenter);
    m_canvas->setStyleSheet(
        QStringLiteral("color: #cccccc; background-color: #1e1e1e;"));
    // 鼠标事件穿透到面板统一处理（坐标再映射回画面分辨率）
    // 对应Python: self._label.setAttribute(Qt.WA_TransparentForMouseEvents, True)
    m_canvas->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (RdpClient::backend() == RdpClient::Backend::CommandLine)
        m_canvas->setText(tr("画面将在外部 RDP 客户端窗口中显示"));

    // 凭据表单收进独立容器：连接成功后整体隐藏（会话中这些字段改了也没用），
    // 断开/失败后恢复以便重连。控制条与状态不在里面——见文件头说明。
    // 对应Python: RDPWidget 本身无表单（参数由 ShellTab 传入，断开靠关闭
    // 标签页），连接后整个标签页都是远程桌面画面
    m_formPanel = new QWidget(this);
    auto *formPanelLayout = new QVBoxLayout(m_formPanel);
    formPanelLayout->setContentsMargins(6, 6, 6, 0);
    formPanelLayout->addLayout(form);

    // 外层零边距：表单隐藏后画布贴边占满面板
    // 对应Python: layout.setContentsMargins(0,0,0,0) + setSpacing(0) (lines 566-569)
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_formPanel);
    layout->addWidget(m_controlBar);
    layout->addWidget(m_canvas, /*stretch=*/1);

    // 帧合并定时器：16ms ≈ 60fps上限（对应Python: _repaint_timer lines 547-550）
    m_repaintTimer = new QTimer(this);
    m_repaintTimer->setInterval(16);
    connect(m_repaintTimer, &QTimer::timeout, this, &RdpPanel::flushFrame);

    connect(m_connectButton, &QPushButton::clicked, this, &RdpPanel::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this,
            &RdpPanel::onDisconnectClicked);
    connect(m_applyButton, &QPushButton::clicked, this,
            &RdpPanel::onApplyResolutionClicked);
    // 密码框里回车即连：密码留空的配置打开后光标就停在这儿
    //（见 promptForPassword），还要用鼠标去点"连接"太别扭。
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &RdpPanel::onConnectClicked);
    // 勾选"适应窗口"时下拉框只作展示（值由画布尺寸决定），故禁用避免误导。
    connect(m_fitCheck, &QCheckBox::toggled, this, [this](bool fit) {
        m_resolutionCombo->setEnabled(!fit);
        if (fit)
            updateResolutionDisplay();
        saveResolutionPrefs();
    });
    // 手填/选档位后立刻落盘：下次开标签页仍是用户要的值。currentTextChanged
    // 同时覆盖"下拉选择"和"编辑框输入"两条路径。
    connect(m_resolutionCombo, &QComboBox::currentTextChanged, this,
            [this](const QString &) { saveResolutionPrefs(); });
    connect(m_client, &RdpClient::stateChanged, this, &RdpPanel::onStateChanged);
    connect(m_client, &RdpClient::errorOccurred, this, &RdpPanel::onError);
    connect(m_client, &RdpClient::frameUpdated, this, &RdpPanel::onFrameUpdated);

    // --- 剪贴板同步接线 ---
    connect(m_clipboardSyncCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_client->setClipboardSyncEnabled(on);
        saveClipboardPrefs();
        if (on) {
            // 通道已加载的会话里（开→关→开）立刻就能用；没加载时快照先存着，
            // 下次建连 RdpClipboard 会补公告，所以这里无条件推一次。
            pushLocalClipboard();
        } else {
            // 关掉不会立刻拆通道（那要重连），但从现在起不再推新内容，
            // 取回入口也收掉。
            m_remoteFileCount = 0;
            m_autoFetchedFingerprint.clear();
            m_fetchFilesButton->setVisible(false);
        }
        if (m_client->state() == RdpClient::State::Connected)
            m_statusLabel->setText(on ? tr("剪贴板同步已开启（如不生效请重连）")
                                      : tr("剪贴板同步已关闭，重连后完全断开通道"));
    });
    // 本机复制的那一刻就把快照推给 worker：远端按 Ctrl+V 时 worker 才能就地应答，
    // 不用回主线程取数据（见 RdpClipboard.h 的线程模型）。
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &RdpPanel::onLocalClipboardChanged);
    connect(m_client, &RdpClient::clipboardTextReceived, this,
            &RdpPanel::onRemoteClipboardText);
    connect(m_client, &RdpClient::remoteClipboardFilesAvailable, this,
            &RdpPanel::onRemoteClipboardFilesAvailable);
    connect(m_client, &RdpClient::remoteClipboardFilesFetched, this,
            &RdpPanel::onRemoteClipboardFilesFetched);
    connect(m_client, &RdpClient::remoteClipboardFetchProgress, this,
            &RdpPanel::onRemoteClipboardFetchProgress);
    connect(m_client, &RdpClient::clipboardError, this, &RdpPanel::onClipboardError);
    connect(m_fetchFilesButton, &QPushButton::clicked, this,
            &RdpPanel::onFetchRemoteFilesClicked);

    // 读取上次选择，再把生效值回填到下拉框（勾了"适应窗口"时显示量出来的尺寸）。
    loadResolutionPrefs();
    updateResolutionDisplay();
    loadClipboardPrefs();

    // 输入事件接收配置（对应Python: RDPWidget.__init__ lines 571-574）
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setCursor(Qt::ArrowCursor);
}

// ---------------------------------------------------------------------------
// 分辨率来源
// ---------------------------------------------------------------------------

QSize RdpPanel::fitResolution() const
{
    // 远端 1 像素 = 本地 1 逻辑像素：画面完全不经缩放，最清晰、无黑边。
    // 高分屏上等于物理像素 ÷ dpr，正是 Python 版 RDP_DISPLAY_SCALE=2 想要的效果，
    // 但不会误伤 dpr=1 的普通屏（旧的 main_window::computeRdpTargetResolution
    // 无条件 ÷2，主流 1080p 上一律夹到 1280x800，就是"分辨率太低"的根因）。
    QSize size = m_canvas ? m_canvas->size() : QSize();

    // 凭据表单还露着时（尚未连上）要把它让出来的高度算进去：库后端连上后
    // m_formPanel 会隐藏，画布正好长高这么多（外层布局零边距零间距）。
    // 不补的话首次建连会按小一圈的画布去算，连上后画面被放大，白丢清晰度。
    if (m_formPanel && m_formPanel->isVisible()
        && RdpClient::backend() == RdpClient::Backend::FreeRdp)
        size.rheight() += m_formPanel->height();

    // 标签页刚建好、布局还没跑时画布可能是 0 或默认的 640x480 之类的假尺寸，
    // 依次退回窗口尺寸、主屏可用区域。外部建连须延后一轮事件循环（见 beginConnect
    // 的注释），走到这里的兜底只是最后一道保险。
    if (size.width() < kMinWidth || size.height() < kMinHeight) {
        if (const QWidget *w = window())
            size = w->size();
    }
    if (size.width() < kMinWidth || size.height() < kMinHeight) {
        if (const QScreen *screen = QGuiApplication::primaryScreen())
            size = screen->availableGeometry().size();
    }
    return alignResolution(size);
}

QSize RdpPanel::targetResolution() const
{
    if (m_fitCheck->isChecked())
        return fitResolution();
    const QSize parsed = parseResolution(m_resolutionCombo->currentText());
    if (!parsed.isValid())
        return fitResolution();   // 手填了一串看不懂的东西，别拿它去连
    return alignResolution(parsed);
}

void RdpPanel::updateResolutionDisplay()
{
    // 把实际要用的值写回下拉框：用户随时看得见"将要用"（未连接）或"正在用"
    //（已连接）的分辨率。不用 findText——档位外的值曾被它静默丢弃。
    const QString text = formatResolution(targetResolution());
    if (m_resolutionCombo->currentText() == text)
        return;
    QSignalBlocker blocker(m_resolutionCombo);   // 别把回填当成用户改动去落盘
    m_resolutionCombo->setCurrentText(text);
}

void RdpPanel::loadResolutionPrefs()
{
    QSettings settings;
    const bool fit = settings.value(QLatin1String(kFitToWindowKey), true).toBool();
    const QSize saved =
        parseResolution(settings.value(QLatin1String(kResolutionKey)).toString());
    {
        QSignalBlocker b1(m_fitCheck);
        QSignalBlocker b2(m_resolutionCombo);
        m_fitCheck->setChecked(fit);
        if (saved.isValid())
            m_resolutionCombo->setCurrentText(formatResolution(alignResolution(saved)));
    }
    m_resolutionCombo->setEnabled(!fit);
}

void RdpPanel::saveResolutionPrefs() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kFitToWindowKey), m_fitCheck->isChecked());
    // 只在手选模式下记分辨率："适应窗口"下框里是量出来的值，记了没意义
    // （换台机器/换窗口大小就该重新量），反而会在取消勾选时给出陈旧值。
    if (!m_fitCheck->isChecked()) {
        const QSize parsed = parseResolution(m_resolutionCombo->currentText());
        if (parsed.isValid())
            settings.setValue(QLatin1String(kResolutionKey),
                              formatResolution(alignResolution(parsed)));
    }
}

void RdpPanel::loadClipboardPrefs()
{
    QSettings settings;
    const bool on = settings.value(QLatin1String(kClipboardSyncKey), true).toBool();
    QSignalBlocker b(m_clipboardSyncCheck);   // 别把回填当成用户改动去落盘
    m_clipboardSyncCheck->setChecked(on);
    // 信号被挡住了，开关值得自己送到 client——它要在建连时读走。
    m_client->setClipboardSyncEnabled(on);
}

void RdpPanel::saveClipboardPrefs() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kClipboardSyncKey),
                      m_clipboardSyncCheck->isChecked());
}

// ---------------------------------------------------------------------------
// 连接参数
// ---------------------------------------------------------------------------

RdpSettings RdpPanel::currentSettings() const
{
    RdpSettings settings;
    settings.host = m_hostEdit->text().trimmed();
    settings.port = m_portSpin->value();
    settings.username = m_userEdit->text().trimmed();
    settings.password = m_passwordEdit->text();
    settings.domain = m_domainEdit->text().trimmed();
    const QSize size = targetResolution();
    settings.width = size.width();
    settings.height = size.height();
    return settings;
}

void RdpPanel::setSettings(const RdpSettings &settings)
{
    // 只喂凭据。分辨率归本面板管——设备配置与 rdp:// URL 里都没有这个字段，
    // 这里若再回填一次就会出现"下拉框一个值、实际连接另一个值"的分裂状态。
    m_hostEdit->setText(settings.host);
    m_portSpin->setValue(settings.port);
    m_userEdit->setText(settings.username);
    m_passwordEdit->setText(settings.password);
    m_domainEdit->setText(settings.domain);
}

void RdpPanel::promptForPassword()
{
    m_statusLabel->setText(tr("请输入密码后点击「连接」"));
    // setFocus 而不只是提示：面板本身是 StrongFocus 的（要收键鼠转发给远端），
    // 打开标签页时焦点在面板上，用户看到光标才知道该往哪儿打字。
    m_passwordEdit->setFocus();
}

void RdpPanel::beginConnect()
{
    const RdpSettings settings = currentSettings();
    if (settings.host.isEmpty()) {
        m_statusLabel->setText(tr("请填写主机地址"));
        return;
    }
    // 先把真正要用的分辨率回填到下拉框，再拿同一份 settings 去连——
    // 显示值与连接值必须同源（历史 bug：两条路径各算一次，用户改了没反应）。
    updateResolutionDisplay();
    // 上一次会话的画面尺寸作废：留着会让 updateConnectedStatus 拿旧尺寸
    // 跟新请求比，误报"服务端未采用"。
    m_videoWidth = 0;
    m_videoHeight = 0;
    // 对应Python: RDPWidget.__init__ 里的 "正在连接远程桌面，请稍候…"
    m_canvas->setText(tr("正在连接远程桌面，请稍候…"));
    m_client->connectToHost(settings);
}

void RdpPanel::onConnectClicked()
{
    beginConnect();
}

void RdpPanel::onDisconnectClicked()
{
    m_client->disconnectFromHost();
}

void RdpPanel::onApplyResolutionClicked()
{
    if (m_client->state() != RdpClient::State::Connected)
        return;

    // RDP 改不了活动会话的分辨率：那要走 Display Control 通道（[MS-RDPEDISP]），
    // 而该扩展依赖图形管线 EGFX，本客户端只挂了 GDI 的 Begin/EndPaint 并在
    // RdpClient::runConnection 里显式关掉了 EGFX（开了会崩在库内）。所以"应用"
    // 就是按新分辨率重连——RDP 重连会回到服务端上原有会话，窗口/程序都还在，
    // 不是新建桌面。
    RdpSettings settings = m_client->settings();   // 生效中的凭据（表单此时是隐藏的）
    const QSize size = targetResolution();
    settings.width = size.width();
    settings.height = size.height();

    updateResolutionDisplay();
    saveResolutionPrefs();
    m_videoWidth = 0;   // 见 beginConnect 里的同一处理
    m_videoHeight = 0;
    // connectToHost 内部会先断开当前会话；m_reconnecting 抑制这期间的
    // Disconnected 分支把凭据表单闪出来（表单此时可能连密码都没有）。
    m_reconnecting = true;
    m_client->connectToHost(settings);
    m_reconnecting = false;
}

void RdpPanel::updateConnectedStatus()
{
    // 已连接时状态栏带上分辨率：这是"到底生效没生效"唯一能自证的地方。
    // 有帧了就报画面实际尺寸——服务端不认我们请求的尺寸时（会话被服务端定死、
    // 老服务端不支持 resize），用户能看到是服务端改的，而不是自己猜。
    const RdpSettings settings = m_client->settings();
    const QSize requested(settings.width, settings.height);
    if (m_videoWidth > 0
        && (m_videoWidth != settings.width || m_videoHeight != settings.height)) {
        m_statusLabel->setText(
            tr("已连接 %1（画面 %2 ≠ 请求 %3，服务端未采用）")
                .arg(settings.host,
                     formatResolution(QSize(m_videoWidth, m_videoHeight)),
                     formatResolution(requested)));
        return;
    }
    m_statusLabel->setText(
        tr("已连接 %1（%2）").arg(settings.host, formatResolution(requested)));
}

void RdpPanel::onStateChanged(RdpClient::State state)
{
    switch (state) {
    case RdpClient::State::Connecting: {
        const RdpSettings settings = m_client->settings();
        if (m_reconnecting)
            m_statusLabel->setText(
                tr("正在以 %1 重连 %2…")
                    .arg(formatResolution(QSize(settings.width, settings.height)),
                         settings.host));
        else
            m_statusLabel->setText(tr("正在连接 %1…").arg(settings.host));
        break;
    }
    case RdpClient::State::Connected:
        updateConnectedStatus();
        // 连接前复制的东西也要能粘：通道刚起来，把当前系统剪贴板补公告一次。
        pushLocalClipboard();
        // 库后端（内嵌渲染）：连接成功后隐藏凭据表单，画面占满面板下方，
        // 与 Python 版一致。控制条（分辨率/应用/断开/状态）留着——会话中要能
        // 改分辨率、也要能断开，这两个此前被一起藏了。
        // 命令行后备画面在外部窗口，表单也留着。
        if (RdpClient::backend() == RdpClient::Backend::FreeRdp) {
            m_formPanel->setVisible(false);
            setFocus();   // 键盘直接落到远程桌面
            // 表单隐藏后画布变大但面板尺寸不变（resizeEvent 不触发），
            // 置脏让定时器按新尺寸重缩
            m_frameDirty = m_hasFrame;
        }
        break;
    case RdpClient::State::Disconnected:
        if (!m_reconnecting) {
            m_statusLabel->setText(tr("未连接"));
            m_formPanel->setVisible(true);   // 断开/失败后恢复表单以便重连
            m_repaintTimer->stop();
            // 定时器已停：等布局稳定后按恢复后的画布尺寸重缩最后一帧
            if (m_hasFrame) {
                m_frameDirty = true;
                QTimer::singleShot(0, this, &RdpPanel::flushFrame);
            }
        }
        // 不清除m_buffer/m_hasFrame，保留最后一帧显示
        break;
    }
    // 远端剪贴板是会话状态：断开后那份清单已经没有意义（listIndex 指向的列表
    // 随会话消失），取回入口必须收掉，否则点了会向已关闭的通道发请求。
    if (state != RdpClient::State::Connected) {
        m_remoteFileCount = 0;
        m_fetchInProgress = false;
        m_fetchIsAuto = false;
        // 指纹也要清：下个会话里远端可能复制同一批文件，留着会被当成"已经自动
        // 取过了"而不再同步。
        m_autoFetchedFingerprint.clear();
        m_fetchFilesButton->setVisible(false);
    }
    const bool busy = state != RdpClient::State::Disconnected;
    setFormEnabled(!busy);
    m_connectButton->setEnabled(!busy);
    m_disconnectButton->setEnabled(busy);
    // "应用" = 重连，只有连上了才有会话可重连
    m_applyButton->setEnabled(state == RdpClient::State::Connected);
}

void RdpPanel::onError(const QString &message)
{
    // 对应Python: _on_connection_error（未连上时把错误展示在画面区域）
    m_statusLabel->setText(tr("连接错误"));
    if (!m_hasFrame)
        m_canvas->setText(tr("RDP 连接失败：\n")
                          + (message.isEmpty() ? tr("未知错误") : message));
}

void RdpPanel::onFrameUpdated(const QImage &frame)
{
    // 对应Python: updateImage (lines 679-689)
    // 只做缓冲区合并，真正的缩放/显示交给flushFrame定时批量执行
    if (frame.isNull())
        return;

    const int fw = frame.width();
    const int fh = frame.height();

    const bool sizeChanged = (fw != m_videoWidth || fh != m_videoHeight);
    if (sizeChanged) {
        // 分辨率变化或首帧：重建缓冲
        m_videoWidth = fw;
        m_videoHeight = fh;
        m_buffer = QPixmap(fw, fh);
        m_buffer.fill(Qt::black);
    }

    if (fw == m_videoWidth && fh == m_videoHeight) {
        // 全帧更新
        m_buffer = QPixmap::fromImage(frame);
    } else {
        // 局部矩形更新（预留，FreeRDP endPaint当前发全帧）
        QPainter painter(&m_buffer);
        painter.drawImage(0, 0, frame);
    }

    m_hasFrame = true;
    m_frameDirty = true;

    // 首帧/服务端改了尺寸时刷一次状态栏：请求值与实际画面不一致的话要说出来。
    if (sizeChanged && m_client->state() == RdpClient::State::Connected)
        updateConnectedStatus();

    // 首帧到达时启动定时器
    if (!m_repaintTimer->isActive())
        m_repaintTimer->start();
}

void RdpPanel::flushFrame()
{
    // 对应Python: _flush_frame (lines 691-709)
    // 关键(去模糊)：按设备像素比渲染到物理像素目标尺寸，再setDevicePixelRatio，
    // 让Qt在高分屏上1:1输出而非二次放大；同时保持宽高比。
    if (!m_frameDirty)
        return;
    m_frameDirty = false;

    const qreal dpr = devicePixelRatioF();
    // 目标用物理像素，保证高分屏清晰
    const int tw = qMax(1, qRound(m_canvas->width() * dpr));
    const int th = qMax(1, qRound(m_canvas->height() * dpr));

    QPixmap scaled = m_buffer.scaled(tw, th,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    m_canvas->setPixmap(scaled);
}

void RdpPanel::resizeEvent(QResizeEvent *event)
{
    // 对应Python: resizeEvent (lines 711-715)
    // 窗口缩放时即使没有新帧也要按新尺寸重绘
    QWidget::resizeEvent(event);
    if (m_hasFrame)
        m_frameDirty = true;

    if (m_fitCheck->isChecked())
        updateResolutionDisplay();   // 未连接时下拉框跟着窗口走，连之前就看得见

    if (m_client->state() != RdpClient::State::Connected)
        return;

    if (!m_fitCheck->isChecked()) {
        updateConnectedStatus();
        return;
    }

    // "适应窗口" 下窗口一变，1:1 的目标尺寸就跟着变，但会话分辨率改不了
    // （见 onApplyResolutionClicked 的注释）。只提示、**不自动重连**——
    // 拖窗口时反复断连比画面被拉伸糟得多。
    const QSize fit = fitResolution();
    const QSize current(m_client->settings().width, m_client->settings().height);
    if (qAbs(fit.width() - current.width()) > 16
        || qAbs(fit.height() - current.height()) > 16)
        m_statusLabel->setText(tr("窗口已变为 %1，点「应用」重连以匹配")
                                   .arg(formatResolution(fit)));
    else
        updateConnectedStatus();
}

void RdpPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 面板被隐藏（切到别的标签页）期间发生的复制会被 onLocalClipboardChanged
    // 跳过——多个 RDP 标签页都监听同一个系统剪贴板，各自都推一遍纯属浪费。
    // 代价是切回来时快照可能是旧的，这里补一次：否则"在别处复制 → 切回 RDP →
    // 远端 Ctrl+V"会粘到上一次的内容。
    pushLocalClipboard();
}

void RdpPanel::setFormEnabled(bool enabled)
{
    // 只管凭据字段。分辨率下拉框/适应窗口不在此列——会话中必须能改，
    // 这正是本次要修的问题（改完点"应用"重连生效）。
    m_hostEdit->setEnabled(enabled);
    m_portSpin->setEnabled(enabled);
    m_userEdit->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled);
    m_domainEdit->setEnabled(enabled);
}

// ---------------------------------------------------------------------------
// 键盘输入
// ---------------------------------------------------------------------------

void RdpPanel::sendKey(QKeyEvent *e, bool isPressed)
{
    // 对应Python: send_key (lines 779-848)
    if (m_client->state() != RdpClient::State::Connected)
        return;

    // 注意：Ctrl+V 在此**不做**特殊处理，照常按扫描码转发下去。RDP 的粘贴是远端
    // 按键触发的，本机只负责在按键之前把内容"挂"在剪贴板通道上（复制那一刻由
    // onLocalClipboardChanged 完成）。这里早退过一版，结果远端收不到粘贴键，
    // 通道通了也粘不出来。macOS 上 Qt 把 Cmd 映射成 ControlModifier，所以
    // Cmd+V 天然转成远端的 Ctrl+V。

    // macOS：可打印键走物理扫描码（PS/2 Set1），让远程 IME 能拦截
    // 对应Python: _mac_ps2_scancode (lines 764-777) + send_key lines 808-818
#ifdef Q_OS_MACOS
    const auto ps2It = s_qtkeyToPs2.constFind(e->key());
    if (ps2It != s_qtkeyToPs2.constEnd()) {
        RdpKeyEvent ke;
        ke.scancode = ps2It.value();
        ke.isExtended = false;
        ke.isPressed = isPressed;
        m_client->sendKeyEvent(ke);
        return;
    }
#endif

    // 非扩展功能键（Backspace/Tab/Esc/Enter/F 键/Shift 等）：扫描码 + isExtended=false
    // 对应Python: _NON_EXTENDED_VK_SC 分支 (lines 832-837)
    const auto nonExtIt = s_nonExtendedScancodes.constFind(e->key());
    if (nonExtIt != s_nonExtendedScancodes.constEnd()) {
        RdpKeyEvent ke;
        ke.scancode = nonExtIt.value();
        ke.isExtended = false;
        ke.isPressed = isPressed;
        m_client->sendKeyEvent(ke);
        return;
    }

    // 扩展键（方向键/Insert/Delete/Home/End/PageUp/Down 等）：扫描码 + isExtended=true
    // 对应Python: vk_code 路径 (lines 838-842)
    const auto extIt = s_extendedKeyScancodes.constFind(e->key());
    if (extIt != s_extendedKeyScancodes.constEnd()) {
        RdpKeyEvent ke;
        ke.scancode = extIt.value();
        ke.isExtended = true;
        ke.isPressed = isPressed;
        m_client->sendKeyEvent(ke);
        return;
    }

    // 可打印字符走 Unicode 事件（对应Python lines 821-827）
    const QString text = e->text();
    if (text.size() == 1 && text.at(0).isPrint()) {
        RdpUnicodeKeyEvent ue;
        ue.character = text.at(0);
        ue.isPressed = isPressed;
        m_client->sendKeyUnicode(ue);
        return;
    }

    // 兜底：使用 nativeScanCode（对应Python line 844）
    if (e->nativeScanCode() > 0) {
        RdpKeyEvent ke;
        ke.scancode = static_cast<quint16>(e->nativeScanCode());
        ke.isExtended = false;
        ke.isPressed = isPressed;
        m_client->sendKeyEvent(ke);
    }
}

void RdpPanel::keyPressEvent(QKeyEvent *event)
{
    // 对应Python: keyPressEvent (lines 850-851)
    sendKey(event, true);
}

void RdpPanel::keyReleaseEvent(QKeyEvent *event)
{
    // 对应Python: keyReleaseEvent (lines 853-854)
    sendKey(event, false);
}

// ---------------------------------------------------------------------------
// 鼠标输入
// ---------------------------------------------------------------------------

QPoint RdpPanel::mapPos(const QPointF &widgetPos) const
{
    // 对应Python: _map_pos (lines 857-875)
    // 把控件坐标映射回画面分辨率（扣除 KeepAspectRatio 居中产生的 letterbox 黑边）
    if (m_videoWidth <= 0 || m_videoHeight <= 0)
        return QPoint(0, 0);

    // Python 版控件本身就是画面，这里画面在子画布上，先换算到画布局部坐标
    const QPointF canvasPos = widgetPos - QPointF(m_canvas->pos());

    const int lw = qMax(1, m_canvas->width());
    const int lh = qMax(1, m_canvas->height());
    // 与 flushFrame 一致：等比缩放，取较小的缩放比
    const qreal scale = qMin(static_cast<qreal>(lw) / m_videoWidth,
                             static_cast<qreal>(lh) / m_videoHeight);
    const qreal dispW = m_videoWidth * scale;
    const qreal dispH = m_videoHeight * scale;
    const qreal offX = (lw - dispW) / 2.0;   // 居中产生的左右黑边
    const qreal offY = (lh - dispH) / 2.0;   // 居中产生的上下黑边

    int x = static_cast<int>((canvasPos.x() - offX) / scale);
    int y = static_cast<int>((canvasPos.y() - offY) / scale);
    x = qBound(0, x, m_videoWidth - 1);
    y = qBound(0, y, m_videoHeight - 1);
    return QPoint(x, y);
}

void RdpPanel::sendMouse(const QPointF &pos, quint16 flags)
{
    // 对应Python: send_mouse (lines 877-892)
    if (m_client->state() != RdpClient::State::Connected)
        return;
    const QPoint mapped = mapPos(pos);
    RdpMouseEvent me;
    me.x = mapped.x();
    me.y = mapped.y();
    me.flags = flags;
    m_client->sendMouseEvent(me);
}

void RdpPanel::mousePressEvent(QMouseEvent *event)
{
    // 对应Python: mousePressEvent (lines 897-899)
    setFocus();
    quint16 flags = PTR_FLAGS_DOWN;
    if (event->button() == Qt::LeftButton)
        flags |= PTR_FLAGS_BUTTON1;
    else if (event->button() == Qt::RightButton)
        flags |= PTR_FLAGS_BUTTON2;
    else if (event->button() == Qt::MiddleButton)
        flags |= PTR_FLAGS_BUTTON3;
    else {
        QWidget::mousePressEvent(event);
        return;
    }
    sendMouse(event->position(), flags);
}

void RdpPanel::mouseReleaseEvent(QMouseEvent *event)
{
    // 对应Python: mouseReleaseEvent (lines 901-902)
    quint16 flags = 0;  // 无 PTR_FLAGS_DOWN 即为释放
    if (event->button() == Qt::LeftButton)
        flags |= PTR_FLAGS_BUTTON1;
    else if (event->button() == Qt::RightButton)
        flags |= PTR_FLAGS_BUTTON2;
    else if (event->button() == Qt::MiddleButton)
        flags |= PTR_FLAGS_BUTTON3;
    else {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    sendMouse(event->position(), flags);
}

void RdpPanel::mouseMoveEvent(QMouseEvent *event)
{
    // 对应Python: mouseMoveEvent → send_mouse(is_hover=True) (lines 894-895)
    sendMouse(event->position(), PTR_FLAGS_MOVE);
}

// ---------------------------------------------------------------------------
// 滚轮输入
// ---------------------------------------------------------------------------

void RdpPanel::wheelEvent(QWheelEvent *event)
{
    // 对应Python: wheelEvent (lines 905-937)
    // 累积 angleDelta，每满一个 detent(120) 发一次滚轮事件：120 同时是 RDP
    // WHEEL_DELTA 与 Qt angleDelta 的单位，触控板的小增量靠残差累积到 120 再发，
    // 否则会被服务端忽略。
    if (m_client->state() != RdpClient::State::Connected)
        return;

    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;

    m_wheelResid += delta;
    const QPoint mapped = mapPos(event->position());
    constexpr int NOTCH = 120;

    while (qAbs(m_wheelResid) >= NOTCH) {
        quint16 flags = PTR_FLAGS_WHEEL;
        if (m_wheelResid > 0) {
            m_wheelResid -= NOTCH;
            flags |= WHEEL_ROTATION_POSITIVE;   // 向上滚
        } else {
            m_wheelResid += NOTCH;
            flags |= PTR_FLAGS_WHEEL_NEGATIVE | WHEEL_ROTATION_NEGATIVE;  // 向下滚
        }
        RdpMouseEvent me;
        me.x = mapped.x();
        me.y = mapped.y();
        me.flags = flags;
        m_client->sendMouseEvent(me);
    }
}

// ---------------------------------------------------------------------------
// 剪贴板同步（cliprdr 通道，实现见 RdpClipboard.cpp）
// ---------------------------------------------------------------------------

// 一批远端文件的指纹，用来认出"这批我已经自动取过了"。
// 用名字+大小而不是 listIndex：下标是每轮公告重新编的，同一批文件重复公告时
// 下标可能一样也可能不一样，拿它做指纹会误判。
QString RdpPanel::fetchFingerprint(const QList<RdpRemoteFile> &files) const
{
    if (files.isEmpty())
        return {};
    QStringList parts;
    parts.reserve(files.size());
    for (const RdpRemoteFile &f : files)
        parts.append(QStringLiteral("%1:%2:%3")
                         .arg(f.name)
                         .arg(f.sizeKnown ? QString::number(f.size)
                                          : QStringLiteral("?"))
                         .arg(f.isDirectory ? QLatin1Char('d') : QLatin1Char('f')));
    return parts.join(QLatin1Char('\n'));
}

void RdpPanel::pushLocalClipboard()
{
    if (!m_clipboardSyncCheck->isChecked())
        return;
    // 未连接时推了也没用（通道还没起）；连上那一刻 onStateChanged 会补一次。
    if (m_client->state() != RdpClient::State::Connected)
        return;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return;

    QStringList files;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            // 只收本地文件；http:// 之类的 URL 没有本地路径，留给下面的文本分支。
            const QString path = url.toLocalFile();
            if (!path.isEmpty())
                files.append(path);
        }
    }
    // 文件与文本二选一、不合并：Finder / 资源管理器复制文件时 mimeData 里通常还
    // 附一份文件名文本，两个格式一起公告，远端粘贴时可能挑中文本，粘出来是一串
    // 路径而不是文件。文件优先——用户复制文件时要的就是文件。
    const QString text = files.isEmpty() && mime->hasText() ? mime->text() : QString();
    if (files.isEmpty() && text.isEmpty())
        return;

    // 挡回声：这正是我们自己刚从远端拿回来、写进系统剪贴板的东西。原路公告回去
    // 只会让远端看到一份它自己给出的数据，白跑一趟协商。
    if (text == m_echoText && files == m_echoFiles)
        return;

    if (!files.isEmpty())
        m_client->clipboardSendFiles(files);
    else
        m_client->sendClipboardText(text);
}

void RdpPanel::onLocalClipboardChanged()
{
    // 只在本面板可见时推：多个 RDP 标签页监听的是同一个系统剪贴板信号，隐藏的
    // 那些推了也没人去粘。切回来时 showEvent 补推。
    if (!isVisible())
        return;
    pushLocalClipboard();
}

void RdpPanel::onRemoteClipboardText(const QString &text)
{
    if (!m_clipboardSyncCheck->isChecked() || text.isEmpty())
        return;
    m_echoText = text;
    m_echoFiles.clear();
    QGuiApplication::clipboard()->setText(text);
}

bool RdpPanel::shouldAutoFetch(qint64 totalBytes, bool sizeUnknown)
{
    Q_UNUSED(sizeUnknown);   // 见头文件：未知大小不拦，由传输中的累计闸门兜底
    return totalBytes <= kAutoFetchLimitBytes;
}

void RdpPanel::onRemoteClipboardFilesAvailable(int count)
{
    m_remoteFileCount = count;
    if (count <= 0 || !m_clipboardSyncCheck->isChecked()) {
        m_fetchFilesButton->setVisible(false);
        m_autoFetchedFingerprint.clear();
        return;
    }

    // 体积先算出来再决定：描述符里带 FD_FILESIZE，所以传之前就知道要传多少，
    // 不用"先传了再说"。
    const QList<RdpRemoteFile> files = m_client->clipboard()->remoteFiles();
    qint64 total = 0;
    bool unknown = false;
    for (const RdpRemoteFile &f : files) {
        if (f.isDirectory)
            continue;
        if (f.sizeKnown)
            total += qint64(f.size);
        else
            unknown = true;
    }

    // 同一批文件只自动取一次。远端公告会重复到达（远端应用重新声明格式、
    // 切换窗口都会再发一次 FormatList），不去重就是对着同一批文件反复重传。
    const QString fingerprint = fetchFingerprint(files);
    const bool alreadyFetched = !fingerprint.isEmpty()
                                && fingerprint == m_autoFetchedFingerprint;

    if (shouldAutoFetch(total, unknown)) {
        m_fetchFilesButton->setVisible(false);
        if (!alreadyFetched && !m_fetchInProgress) {
            m_autoFetchedFingerprint = fingerprint;
            startFetch(/*auto=*/true);
        }
        return;
    }

    // 超限：不自动传，把按钮当"我确认要传这么大"的入口露出来。这是按钮唯一
    // 还存在的理由，所以文字里必须写明体积——否则用户不知道自己在确认什么。
    m_autoFetchedFingerprint.clear();
    m_fetchFilesButton->setVisible(true);
    m_fetchFilesButton->setEnabled(!m_fetchInProgress);
    m_fetchFilesButton->setText(tr("取回 %1 个文件（%2）")
                                    .arg(count)
                                    .arg(QLocale().formattedDataSize(total)));
    m_statusLabel->setText(
        tr("远端复制了 %1 个文件，共 %2，超过 %3 未自动取回——点「取回」确认")
            .arg(count)
            .arg(QLocale().formattedDataSize(total))
            .arg(QLocale().formattedDataSize(kAutoFetchLimitBytes)));
}

void RdpPanel::onFetchRemoteFilesClicked()
{
    startFetch(/*auto=*/false);
}

// 真正发起取回。isAuto 只影响提示文字与失败时的语气：自动取回是用户没主动要求
// 的后台动作，不该用"失败"这种需要用户处置的措辞去打扰他。
void RdpPanel::startFetch(bool isAuto)
{
    if (m_fetchInProgress || m_remoteFileCount <= 0)
        return;

    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) {
        m_statusLabel->setText(tr("取回失败：拿不到临时目录"));
        return;
    }
    // 每次取回落到独立子目录：远端可能连续复制同名文件，共用一个目录会互相覆盖，
    // 而"上次取回的路径还留在系统剪贴板里"是常态，覆盖了就等于悄悄换掉内容。
    // 也正因为剪贴板还指着这些路径，取回的文件**不主动删**——删了用户粘出来就是
    // 一堆失效引用。留给系统的临时目录清理机制。
    const QString dir = QDir(base).absoluteFilePath(
        QStringLiteral("cube-shell-rdp-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(++m_fetchSeq));
    if (!QDir().mkpath(dir)) {
        m_statusLabel->setText(tr("取回失败：无法创建 %1").arg(dir));
        return;
    }

    m_fetchInProgress = true;
    m_fetchIsAuto = isAuto;
    m_fetchFilesButton->setEnabled(false);
    m_statusLabel->setText(isAuto
                               ? tr("正在同步远端复制的 %1 个文件…").arg(m_remoteFileCount)
                               : tr("正在取回 %1 个文件…").arg(m_remoteFileCount));
    m_client->fetchRemoteClipboardFiles(dir);
}

void RdpPanel::onRemoteClipboardFilesFetched(const QStringList &localPaths)
{
    const bool wasAuto = m_fetchIsAuto;
    m_fetchInProgress = false;
    m_fetchIsAuto = false;
    m_fetchFilesButton->setEnabled(m_remoteFileCount > 0);
    if (localPaths.isEmpty()) {
        m_statusLabel->setText(tr("取回结束，没有文件落地"));
        return;
    }

    // 把落地后的本地真实路径塞进系统剪贴板：之后在 Finder / 资源管理器里粘贴
    // 就是真文件。这是"远端→本机文件"这条路能用的关键一步（见 RdpClipboard.h
    // 里关于为什么不做透明粘贴的说明）。
    QList<QUrl> urls;
    urls.reserve(localPaths.size());
    for (const QString &path : localPaths)
        urls.append(QUrl::fromLocalFile(path));
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    m_echoText.clear();
    m_echoFiles = localPaths;
    QGuiApplication::clipboard()->setMimeData(mime);   // 剪贴板接管所有权
    // 自动取回成功后按钮没有存在意义了（文件已经在剪贴板里，再点就是重传一份）。
    m_fetchFilesButton->setVisible(false);
    m_statusLabel->setText(
        wasAuto ? tr("远端复制的 %1 个文件已同步，可直接粘贴").arg(localPaths.size())
                : tr("已取回 %1 个文件，可在文件管理器里直接粘贴").arg(localPaths.size()));
}

void RdpPanel::onRemoteClipboardFetchProgress(qint64 received, qint64 total)
{
    if (!m_fetchInProgress)
        return;
    // 自动取回的兜底闸门：清单里有条目没带 FD_FILESIZE 时事前算不出总量
    //（见 shouldAutoFetch），只能在传输中盯着已收字节。真的超了就中止，
    // 不让一次自动同步无声地拖成几个 GB。
    if (m_fetchIsAuto && received > kAutoFetchLimitBytes) {
        m_client->cancelRemoteClipboardFetch();
        m_fetchInProgress = false;
        m_fetchIsAuto = false;
        m_fetchFilesButton->setVisible(m_remoteFileCount > 0);
        m_fetchFilesButton->setEnabled(m_remoteFileCount > 0);
        m_fetchFilesButton->setText(tr("取回 %1 个文件").arg(m_remoteFileCount));
        m_statusLabel->setText(
            tr("远端文件超过 %1，已停止自动同步——点「取回」手动传回")
                .arg(QLocale().formattedDataSize(kAutoFetchLimitBytes)));
        return;
    }
    if (total > 0)
        m_statusLabel->setText(
            tr("正在取回文件… %1%").arg(qMin<qint64>(100, received * 100 / total)));
    else
        // 远端描述符没带 FD_FILESIZE 时总量未知，只能报已收字节。
        m_statusLabel->setText(
            tr("正在取回文件… 已收 %1").arg(QLocale().formattedDataSize(received)));
}

void RdpPanel::onClipboardError(const QString &message)
{
    // 会话还活着，只是这次剪贴板动作没成——不要走 onError 那条路（它会报
    // "连接错误"并把提示画到画面区，指错方向）。
    const bool wasAuto = m_fetchIsAuto;
    m_statusLabel->setText(message.isEmpty() ? tr("剪贴板同步出错") : message);
    // 取回失败必须解锁按钮，否则一次失败之后它永远是禁用的。
    m_fetchInProgress = false;
    m_fetchIsAuto = false;
    // 自动取回失败了就把按钮露出来当重试入口，并清掉指纹——否则这批文件既自动
    // 取不到、又因为"已经自动取过"而不再重试，用户干瞪眼。
    if (wasAuto) {
        m_autoFetchedFingerprint.clear();
        if (m_remoteFileCount > 0) {
            m_fetchFilesButton->setVisible(true);
            m_fetchFilesButton->setText(tr("重试取回 %1 个文件").arg(m_remoteFileCount));
        }
    }
    m_fetchFilesButton->setEnabled(m_remoteFileCount > 0);
}

// ---------------------------------------------------------------------------
// 拖拽传文件
// ---------------------------------------------------------------------------

void RdpPanel::dragEnterEvent(QDragEnterEvent *event)
{
    // 对应Python: dragEnterEvent (lines 939-943)
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void RdpPanel::dropEvent(QDropEvent *event)
{
    // 对应Python: dropEvent (lines 945-949)
    QStringList files;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty())
            files.append(path);
    }
    if (!files.isEmpty())
        m_client->clipboardSendFiles(files);
    // 同步关掉时通道压根没加载，拖进来也送不出去——明说，别让用户以为在传。
    if (!files.isEmpty() && !m_clipboardSyncCheck->isChecked())
        m_statusLabel->setText(tr("剪贴板同步已关闭，文件未发送（勾选后重连生效）"));
}

} // namespace cubeshell
