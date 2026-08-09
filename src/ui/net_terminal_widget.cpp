// net_terminal_widget.cpp — TCP/Telnet 终端面板。见 net_terminal_widget.h。

#include "net_terminal_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "qtermwidget.h"
#include "Session.h"

#include "net/TcpBridge.h"
#include "net/TelnetProtocol.h"

#include "config/GlobalState.h"

#include "dialogs/NetConnectDialog.h"   // netcombo 辅助函数

namespace cubeshell {

NetTerminalWidget::NetTerminalWidget(const QString &mode, QWidget *parent)
    : QWidget(parent)
    , m_mode(mode == QLatin1String("tcp") ? QStringLiteral("tcp")
                                          : QStringLiteral("telnet"))
    , m_client(new TcpClient(this))
{
    m_pending.mode = m_mode;

    // --- 顶部工具栏 ---
    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("主机名或IP地址"));
    m_host->setClearButtonEnabled(true);

    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(defaultPortFor(m_mode));

    m_newline = new QComboBox(this);
    // 默认值理由同 NetConnectDialog：Telnet 的行尾由 RFC 854 定为 CR LF，
    // 裸 TCP 对端多半是 Unix 侧服务，发 CR 过去会显示成 ^M。
    netcombo::fillNewlineMode(m_newline, isTelnet() ? TcpSettings::NewlineMode::CrLf
                                                    : TcpSettings::NewlineMode::Lf);
    m_pending.newlineMode = netcombo::newlineModeOf(m_newline);

    m_localEcho = new QCheckBox(tr("本地回显"), this);
    // 裸 TCP 对端一般不回显（nc 就不回），默认打开才看得见自己输入。
    // Telnet 服务端通常 WILL ECHO 接管回显，默认关闭避免双回显；真遇到不回显
    // 的设备，勾上即可（服务端若随后 WILL ECHO，桥会自动把这项关掉）。
    m_localEcho->setChecked(!isTelnet());
    m_rxImplicitCr = new QCheckBox(tr("接收补 CR"), this);
    m_rxImplicitCr->setChecked(true);
    m_rxImplicitCr->setToolTip(
        tr("接收到孤立的 LF(\\n) 时补一个 CR(\\r)，让光标回到行首。\n"
           "关闭后对端发裸 LF 会出现阶梯状输出——这是 VT 规范的正确行为\n"
           "（LF 只下移一行，回行首是 CR 的职责），但不少嵌入式实现并不发 CR。\n"
           "等价于 PuTTY 的 Implicit CR in every LF。"));
    m_logging = new QCheckBox(tr("录制日志"), this);
    m_logging->setToolTip(tr("把收到的原始字节（含 Telnet 协商序列）追加写入文件。"));

    m_connectButton = new QPushButton(tr("连接"), this);
    m_connectButton->setDefault(true);
    m_disconnectButton = new QPushButton(tr("断开"), this);
    m_disconnectButton->setEnabled(false);
    m_clearButton = new QPushButton(tr("清屏"), this);

    // 两行工具栏，与串口面板同一分行原则：第一行是链路参数（改这些要重连），
    // 第二行是纯终端行为（连接中也能改）。
    auto *row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    // 协议名是专有名词，不翻译。
    row1->addWidget(new QLabel(isTelnet() ? QStringLiteral("Telnet") : QStringLiteral("TCP"),
                               this));
    row1->addWidget(new QLabel(tr("主机："), this));
    row1->addWidget(m_host, 1);
    row1->addWidget(new QLabel(tr("端口："), this));
    row1->addWidget(m_port);
    row1->addWidget(m_connectButton);
    row1->addWidget(m_disconnectButton);

    auto *row2 = new QHBoxLayout;
    row2->setContentsMargins(0, 0, 0, 0);
    // 标签写明"发送"：这个下拉框只作用于发出去的字节，管不到接收方向；
    // 接收方向由旁边的"接收补 CR"负责。措辞与串口面板保持一致。
    row2->addWidget(new QLabel(tr("发送换行："), this));
    row2->addWidget(m_newline);
    row2->addWidget(m_localEcho);
    row2->addWidget(m_rxImplicitCr);
    row2->addWidget(m_logging);
    row2->addStretch(1);
    row2->addWidget(m_clearButton);

    // --- 终端 ---
    // startnow=0：不拉起本地 shell，Session 空转等 TcpBridge 喂字节。
    // 与 SshTerminalWidget / SerialTerminalWidget 同理。
    m_term = new QTermWidget(0, this);
    QFont font(GlobalState::instance().fontFamily(), GlobalState::instance().fontSize());
    if (font.family().isEmpty())
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_term->setTerminalFont(font);
    connect(m_term, &QTermWidget::fontSizeChanged, this,
            [](int size) { GlobalState::instance().setFontSize(size); });
    m_term->setColorScheme(GlobalState::instance().terminalTheme());
    m_term->setScrollBarPosition(ScrollBarRight);

    // 不挂 TerminalCommandSuggest：命令提示是 shell 语义，对面可能是交换机
    // 的命令行或一个根本没有 shell 的裸 TCP 服务，弹 Linux 命令候选只会干扰。

    // --- 状态栏 ---
    m_statusLabel = new QLabel(tr("未连接"), this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(row1);
    layout->addLayout(row2);
    layout->addWidget(m_term, 1);
    layout->addWidget(m_statusLabel);

    // --- 桥接 ---
    m_bridge = new TcpBridge(m_term->session(), m_client, this);
    m_bridge->applySettings(currentSettings());
    m_bridge->start();

    // --- 信号接线 ---
    connect(m_connectButton, &QPushButton::clicked,
            this, &NetTerminalWidget::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &NetTerminalWidget::onDisconnectClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &NetTerminalWidget::onClearClicked);
    // 主机框里回车 = 点连接（连接中不重复触发，按钮此时是禁用的）。
    connect(m_host, &QLineEdit::returnPressed, this, [this] {
        if (m_connectButton->isEnabled())
            onConnectClicked();
    });
    connect(m_newline, &QComboBox::currentIndexChanged,
            this, &NetTerminalWidget::onNewlineModeChanged);
    connect(m_localEcho, &QCheckBox::toggled,
            this, &NetTerminalWidget::onLocalEchoToggled);
    connect(m_rxImplicitCr, &QCheckBox::toggled,
            this, &NetTerminalWidget::onRxImplicitCrToggled);
    connect(m_logging, &QCheckBox::toggled,
            this, &NetTerminalWidget::onLogToggled);

    connect(m_client, &TcpClient::stateChanged,
            this, &NetTerminalWidget::onStateChanged);
    connect(m_client, &TcpClient::errorOccurred,
            this, &NetTerminalWidget::onError);
    connect(m_bridge, &TcpBridge::localEchoChanged,
            this, &NetTerminalWidget::onBridgeLocalEchoChanged);
    connect(m_bridge, &TcpBridge::loginPhaseChanged,
            this, [this](TcpBridge::LoginPhase) { updateStatus(); });
}

NetTerminalWidget::~NetTerminalWidget()
{
    // 先停桥（断开与 Session 的连接），再让 client 随父对象析构关 socket。
    // 顺序同 SerialTerminalWidget。
    if (m_bridge)
        m_bridge->stop();
}

TcpSettings NetTerminalWidget::currentSettings() const
{
    // 以 m_pending 为底（凭据/协商/终端类型），工具栏上的项覆盖上去。
    TcpSettings s = m_pending;
    s.mode = m_mode;
    s.host = m_host->text().trimmed();
    s.port = quint16(m_port->value());
    s.newlineMode  = netcombo::newlineModeOf(m_newline);
    s.localEcho    = m_localEcho->isChecked();
    s.rxImplicitCr = m_rxImplicitCr->isChecked();
    return s;
}

void NetTerminalWidget::setSettings(const TcpSettings &s)
{
    m_pending = s;
    m_pending.mode = m_mode;   // 面板的模式在构造时定死，不被传入值改写
    m_host->setText(s.host);
    m_port->setValue(s.port ? s.port : defaultPortFor(m_mode));
    netcombo::selectData(m_newline, int(s.newlineMode));
    m_localEcho->setChecked(s.localEcho);
    m_rxImplicitCr->setChecked(s.rxImplicitCr);
    // setChecked/selectData 会触发对应槽，bridge 状态自动跟着更新。
}

void NetTerminalWidget::connectToHost()
{
    onConnectClicked();
}

void NetTerminalWidget::onConnectClicked()
{
    const TcpSettings s = currentSettings();
    if (s.host.isEmpty()) {
        m_lastError = tr("请输入主机名或IP地址。");
        m_statusLabel->setText(m_lastError);
        emit connectionFailed(m_lastError);
        return;
    }

    m_lastError.clear();
    // 协商/凭据/换行等一并交给桥；必须在建连之前——connected() 一到就要用
    // 已就位的协商配置去应答对端的 DO/WILL。
    m_bridge->applySettings(s);
    m_statusLabel->setText(tr("正在连接 %1…").arg(s.displayTarget()));
    if (!m_client->connectToHost(s)) {
        // 参数非法（空主机/0 端口），原因已由 onError 存进 m_lastError。
        const QString message = m_lastError.isEmpty()
                                    ? tr("无法连接 %1。").arg(s.displayTarget())
                                    : m_lastError;
        m_statusLabel->setText(message);
        emit connectionFailed(message);
        return;
    }
    m_term->setFocus();
}

void NetTerminalWidget::onDisconnectClicked()
{
    m_client->disconnectFromHost();
}

void NetTerminalWidget::onStateChanged(TcpClient::State state)
{
    const bool idle = (state == TcpClient::State::Disconnected);
    // 连接中（Connecting）也锁住链路参数：此时改主机不会生效，还会让
    // 超时报错里的地址与框里显示的不一致。
    setFormEnabled(idle);
    m_connectButton->setEnabled(idle);
    m_disconnectButton->setEnabled(!idle);
    updateStatus();

    if (state == TcpClient::State::Connected)
        emit connected();
    else if (idle)
        emit disconnected();
}

void NetTerminalWidget::onError(const QString &message)
{
    m_lastError = message;
    m_statusLabel->setText(message);
    // 与串口面板不同：TCP 建连是异步的，失败只能在这里外报，
    // onConnectClicked 返回时还不知道结果。
    emit connectionFailed(message);
}

void NetTerminalWidget::onNewlineModeChanged()
{
    if (m_bridge)
        m_bridge->setNewlineMode(netcombo::newlineModeOf(m_newline));
}

void NetTerminalWidget::onLocalEchoToggled(bool enabled)
{
    if (m_bridge)
        m_bridge->setLocalEcho(enabled);
}

void NetTerminalWidget::onRxImplicitCrToggled(bool enabled)
{
    if (m_bridge)
        m_bridge->setRxImplicitCr(enabled);
}

void NetTerminalWidget::onBridgeLocalEchoChanged(bool enabled)
{
    // 服务端 WILL ECHO 时桥已经把本地回显关了，这里只同步复选框的显示。
    // setChecked 会重入 onLocalEchoToggled → setLocalEcho(enabled)，值相同，
    // 幂等，不会绕成环。
    if (m_localEcho->isChecked() != enabled)
        m_localEcho->setChecked(enabled);
    updateStatus();
}

void NetTerminalWidget::onLogToggled(bool enabled)
{
    if (!enabled) {
        m_client->setLogFile(QString());
        updateStatus();
        return;
    }

    const QString host = m_host->text().trimmed();
    const QString suggested = QStringLiteral("%1-%2.log")
        .arg(m_mode, host.isEmpty()
                         ? QStringLiteral("session")
                         // IPv6 字面量里的 ':' 在 Windows 上不是合法文件名字符。
                         : QString(host).replace(QLatin1Char(':'), QLatin1Char('_')));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("选择日志文件"), suggested, tr("日志文件 (*.log);;所有文件 (*)"));
    if (path.isEmpty()) {
        // 用户取消 → 复选框回弹。setChecked 会重入本槽，但 enabled=false
        // 分支只是关日志文件（本就没开），无副作用。
        m_logging->setChecked(false);
        return;
    }
    if (!m_client->setLogFile(path)) {
        m_logging->setChecked(false);   // 打开失败，原因已由 onError 显示
        return;
    }
    updateStatus();
}

void NetTerminalWidget::onClearClicked()
{
    if (m_term)
        m_term->clear();
}

void NetTerminalWidget::setFormEnabled(bool enabled)
{
    // 链路参数改了要重连，连接中锁死；换行/回显/日志是纯终端行为，不锁。
    m_host->setEnabled(enabled);
    m_port->setEnabled(enabled);
}

void NetTerminalWidget::updateStatus()
{
    switch (m_client->state()) {
    case TcpClient::State::Disconnected:
        // 有错误时保留错误文案，别用"未连接"把它冲掉。
        m_statusLabel->setText(m_lastError.isEmpty() ? tr("未连接") : m_lastError);
        return;
    case TcpClient::State::Connecting:
        m_statusLabel->setText(
            tr("正在连接 %1…").arg(m_client->settings().displayTarget()));
        return;
    case TcpClient::State::Connected:
        break;
    }

    const TcpSettings s = m_client->settings();
    QString text = tr("已连接 %1").arg(s.displayTarget());
    if (isTelnet()) {
        if (!s.negotiate)
            text += tr("　协商已关闭");
        else if (m_bridge && m_bridge->protocol()->serverEcho())
            text += tr("　服务端回显");
        switch (m_bridge ? m_bridge->loginPhase() : TcpBridge::LoginPhase::Idle) {
        case TcpBridge::LoginPhase::WaitUser:
            text += tr("　自动登录：等待提示"); break;
        case TcpBridge::LoginPhase::SentUser:
            text += tr("　自动登录：已发用户名"); break;
        case TcpBridge::LoginPhase::SentPass:
            text += tr("　自动登录：已发密码"); break;
        case TcpBridge::LoginPhase::Idle:
        case TcpBridge::LoginPhase::Done:
            break;
        }
    }
    if (m_client->isLogging())
        text += tr("　录制中：%1").arg(m_client->logFilePath());
    m_statusLabel->setText(text);
}

} // namespace cubeshell
