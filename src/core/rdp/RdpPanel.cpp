// RdpPanel.cpp — RDP 连接面板实现。
// 对应Python: core/rdp/rdp_client.py::RDPWidget（UI/画面显示部分，简化版）

#include "RdpPanel.h"

#include <QClipboard>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
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

    m_resolutionCombo = new QComboBox(this);
    m_resolutionCombo->addItems({
        QStringLiteral("1024x768"),
        QStringLiteral("1280x800"),
        QStringLiteral("1440x900"),
        QStringLiteral("1920x1080"),
    });
    m_resolutionCombo->setCurrentIndex(1);
    form->addRow(tr("分辨率"), m_resolutionCombo);

    // ---------------- 连接/断开 + 状态 ----------------
    m_connectButton = new QPushButton(tr("连接"), this);
    m_disconnectButton = new QPushButton(tr("断开"), this);
    m_disconnectButton->setEnabled(false);
    auto *buttons = new QHBoxLayout();
    buttons->addWidget(m_connectButton);
    buttons->addWidget(m_disconnectButton);
    buttons->addStretch();

    m_statusLabel = new QLabel(tr("未连接"), this);

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

    // 表单区收进独立容器：连接成功后整体隐藏，断开/失败后恢复以便重连。
    // 对应Python: RDPWidget 本身无表单（参数由 ShellTab 传入，断开靠关闭
    // 标签页），连接后整个标签页都是远程桌面画面
    m_formPanel = new QWidget(this);
    auto *formPanelLayout = new QVBoxLayout(m_formPanel);
    formPanelLayout->addLayout(form);
    formPanelLayout->addLayout(buttons);
    formPanelLayout->addWidget(m_statusLabel);

    // 外层零边距：表单隐藏后画布贴边占满面板
    // 对应Python: layout.setContentsMargins(0,0,0,0) + setSpacing(0) (lines 566-569)
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_formPanel);
    layout->addWidget(m_canvas, /*stretch=*/1);

    // 帧合并定时器：16ms ≈ 60fps上限（对应Python: _repaint_timer lines 547-550）
    m_repaintTimer = new QTimer(this);
    m_repaintTimer->setInterval(16);
    connect(m_repaintTimer, &QTimer::timeout, this, &RdpPanel::flushFrame);

    connect(m_connectButton, &QPushButton::clicked, this, &RdpPanel::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this,
            &RdpPanel::onDisconnectClicked);
    // 密码框里回车即连：密码留空的配置打开后光标就停在这儿
    //（见 promptForPassword），还要用鼠标去点"连接"太别扭。
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &RdpPanel::onConnectClicked);
    connect(m_client, &RdpClient::stateChanged, this, &RdpPanel::onStateChanged);
    connect(m_client, &RdpClient::errorOccurred, this, &RdpPanel::onError);
    connect(m_client, &RdpClient::frameUpdated, this, &RdpPanel::onFrameUpdated);

    // 输入事件接收配置（对应Python: RDPWidget.__init__ lines 571-574）
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setCursor(Qt::ArrowCursor);
}

RdpSettings RdpPanel::currentSettings() const
{
    RdpSettings settings;
    settings.host = m_hostEdit->text().trimmed();
    settings.port = m_portSpin->value();
    settings.username = m_userEdit->text().trimmed();
    settings.password = m_passwordEdit->text();
    settings.domain = m_domainEdit->text().trimmed();
    const QStringList parts =
        m_resolutionCombo->currentText().split(QLatin1Char('x'));
    if (parts.size() == 2) {
        settings.width = parts.at(0).toInt();
        settings.height = parts.at(1).toInt();
    }
    return settings;
}

void RdpPanel::setSettings(const RdpSettings &settings)
{
    m_hostEdit->setText(settings.host);
    m_portSpin->setValue(settings.port);
    m_userEdit->setText(settings.username);
    m_passwordEdit->setText(settings.password);
    m_domainEdit->setText(settings.domain);
    const QString resolution =
        QStringLiteral("%1x%2").arg(settings.width).arg(settings.height);
    const int index = m_resolutionCombo->findText(resolution);
    if (index >= 0)
        m_resolutionCombo->setCurrentIndex(index);
}

void RdpPanel::promptForPassword()
{
    m_statusLabel->setText(tr("请输入密码后点击「连接」"));
    // setFocus 而不只是提示：面板本身是 StrongFocus 的（要收键鼠转发给远端），
    // 打开标签页时焦点在面板上，用户看到光标才知道该往哪儿打字。
    m_passwordEdit->setFocus();
}

void RdpPanel::onConnectClicked()
{
    const RdpSettings settings = currentSettings();
    if (settings.host.isEmpty()) {
        m_statusLabel->setText(tr("请填写主机地址"));
        return;
    }
    // 对应Python: RDPWidget.__init__ 里的 "正在连接远程桌面，请稍候…"
    m_canvas->setText(tr("正在连接远程桌面，请稍候…"));
    m_client->connectToHost(settings);
}

void RdpPanel::onDisconnectClicked()
{
    m_client->disconnectFromHost();
}

void RdpPanel::onStateChanged(RdpClient::State state)
{
    switch (state) {
    case RdpClient::State::Connecting:
        m_statusLabel->setText(tr("正在连接 %1…").arg(m_client->settings().host));
        break;
    case RdpClient::State::Connected:
        m_statusLabel->setText(tr("已连接 %1").arg(m_client->settings().host));
        // 库后端（内嵌渲染）：连接成功后隐藏表单，画面占满整个面板，
        // 与 Python 版一致；断开靠关闭标签页（closeTab 已有 disconnect）。
        // 命令行后备画面在外部窗口，保留表单和断开按钮。
        if (RdpClient::backend() == RdpClient::Backend::FreeRdp) {
            m_formPanel->setVisible(false);
            setFocus();   // 键盘直接落到远程桌面
            // 表单隐藏后画布变大但面板尺寸不变（resizeEvent 不触发），
            // 置脏让定时器按新尺寸重缩
            m_frameDirty = m_hasFrame;
        }
        break;
    case RdpClient::State::Disconnected:
        m_statusLabel->setText(tr("未连接"));
        m_formPanel->setVisible(true);   // 断开/失败后恢复表单以便重连
        m_repaintTimer->stop();
        // 定时器已停：等布局稳定后按恢复后的画布尺寸重缩最后一帧
        if (m_hasFrame) {
            m_frameDirty = true;
            QTimer::singleShot(0, this, &RdpPanel::flushFrame);
        }
        // 不清除m_buffer/m_hasFrame，保留最后一帧显示
        break;
    }
    const bool busy = state != RdpClient::State::Disconnected;
    setFormEnabled(!busy);
    m_connectButton->setEnabled(!busy);
    m_disconnectButton->setEnabled(busy);
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

    if (fw != m_videoWidth || fh != m_videoHeight) {
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
}

void RdpPanel::setFormEnabled(bool enabled)
{
    m_hostEdit->setEnabled(enabled);
    m_portSpin->setEnabled(enabled);
    m_userEdit->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled);
    m_domainEdit->setEnabled(enabled);
    m_resolutionCombo->setEnabled(enabled);
}

// ---------------------------------------------------------------------------
// 键盘输入
// ---------------------------------------------------------------------------

void RdpPanel::sendKey(QKeyEvent *e, bool isPressed)
{
    // 对应Python: send_key (lines 779-848)
    if (m_client->state() != RdpClient::State::Connected)
        return;

    // Ctrl+V：把本地剪贴板文本同步到远程（对应Python lines 784-788）
    if (isPressed && (e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_V) {
        const QString text = QGuiApplication::clipboard()->text();
        if (!text.isEmpty())
            m_client->sendClipboardText(text);
        return;
    }

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
}

} // namespace cubeshell
