// serial_terminal_widget.cpp — 串口终端面板。见 serial_terminal_widget.h。

#include "serial_terminal_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "qtermwidget.h"
#include "Session.h"

#include "serial/SerialBridge.h"

#include "config/GlobalState.h"

#include "dialogs/SerialConnectDialog.h"   // serialcombo 辅助函数

namespace cubeshell {

SerialTerminalWidget::SerialTerminalWidget(QWidget *parent)
    : QWidget(parent)
    , m_client(new SerialClient(this))
{
    // --- 顶部工具栏 ---
    m_port = new QComboBox(this);
    m_port->setEditable(true);   // 允许手输设备路径
    m_port->setInsertPolicy(QComboBox::NoInsert);
    m_port->setMinimumWidth(180);
    serialcombo::fillPorts(m_port);

    m_baud = new QComboBox(this);
    m_baud->setEditable(true);   // 非标准波特率可手输
    m_baud->setInsertPolicy(QComboBox::NoInsert);
    serialcombo::fillBaudRates(m_baud);

    m_dataBits = new QComboBox(this);
    serialcombo::fillDataBits(m_dataBits);
    m_parity = new QComboBox(this);
    serialcombo::fillParity(m_parity);
    m_stopBits = new QComboBox(this);
    serialcombo::fillStopBits(m_stopBits);
    m_flow = new QComboBox(this);
    serialcombo::fillFlowControl(m_flow);
    m_newline = new QComboBox(this);
    serialcombo::fillNewlineMode(m_newline);

    m_localEcho = new QCheckBox(tr("本地回显"), this);
    m_logging = new QCheckBox(tr("录制日志"), this);

    m_connectButton = new QPushButton(tr("连接"), this);
    m_disconnectButton = new QPushButton(tr("断开"), this);
    m_disconnectButton->setEnabled(false);
    m_clearButton = new QPushButton(tr("清屏"), this);

    // 两行工具栏：参数一行，开关+按钮一行。挤在一行会在窄窗口下被裁掉。
    auto *row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    row1->addWidget(new QLabel(tr("串口："), this));
    row1->addWidget(m_port, 1);
    row1->addWidget(new QLabel(tr("波特率："), this));
    row1->addWidget(m_baud);
    row1->addWidget(new QLabel(tr("数据位："), this));
    row1->addWidget(m_dataBits);
    row1->addWidget(new QLabel(tr("校验："), this));
    row1->addWidget(m_parity);
    row1->addWidget(new QLabel(tr("停止位："), this));
    row1->addWidget(m_stopBits);
    row1->addWidget(new QLabel(tr("流控："), this));
    row1->addWidget(m_flow);

    auto *row2 = new QHBoxLayout;
    row2->setContentsMargins(0, 0, 0, 0);
    row2->addWidget(new QLabel(tr("换行："), this));
    row2->addWidget(m_newline);
    row2->addWidget(m_localEcho);
    row2->addWidget(m_logging);
    row2->addStretch(1);
    row2->addWidget(m_clearButton);
    row2->addWidget(m_connectButton);
    row2->addWidget(m_disconnectButton);

    // --- 终端 ---
    // startnow=0：不拉起本地 shell，Session 空转等 SerialBridge 喂字节。
    // 与 SshTerminalWidget 同理。
    m_term = new QTermWidget(0, this);
    QFont font(GlobalState::instance().fontFamily(), GlobalState::instance().fontSize());
    if (font.family().isEmpty())
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_term->setTerminalFont(font);
    connect(m_term, &QTermWidget::fontSizeChanged, this,
            [](int size) { GlobalState::instance().setFontSize(size); });
    m_term->setColorScheme(GlobalState::instance().terminalTheme());
    m_term->setScrollBarPosition(ScrollBarRight);

    // 不挂 TerminalCommandSuggest：命令提示是 shell 语义，串口对面
    // 可能是没有 shell 的裸机固件，弹 Linux 命令候选只会干扰。

    // --- 状态栏 ---
    m_statusLabel = new QLabel(tr("未连接"), this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(row1);
    layout->addLayout(row2);
    layout->addWidget(m_term, 1);
    layout->addWidget(m_statusLabel);

    // --- 桥接 ---
    m_bridge = new SerialBridge(m_term->session(), m_client, this);
    // echoData 走 Session::onReceiveBlock 的 lambda 已在 bridge 内部接好。
    m_bridge->start();

    // --- 信号接线 ---
    connect(m_connectButton, &QPushButton::clicked,
            this, &SerialTerminalWidget::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &SerialTerminalWidget::onDisconnectClicked);
    connect(m_clearButton, &QPushButton::clicked,
            this, &SerialTerminalWidget::onClearClicked);
    connect(m_newline, &QComboBox::currentIndexChanged,
            this, &SerialTerminalWidget::onNewlineModeChanged);
    connect(m_localEcho, &QCheckBox::toggled,
            this, &SerialTerminalWidget::onLocalEchoToggled);
    connect(m_logging, &QCheckBox::toggled,
            this, &SerialTerminalWidget::onLogToggled);

    connect(m_client, &SerialClient::stateChanged,
            this, &SerialTerminalWidget::onStateChanged);
    connect(m_client, &SerialClient::errorOccurred,
            this, &SerialTerminalWidget::onError);
    connect(m_client, &SerialClient::portsChanged,
            this, &SerialTerminalWidget::onPortsChanged);

    // 工具栏初值同步到 bridge（默认 CR + 不回显，与 SerialSettings 默认一致）。
    m_bridge->setNewlineMode(serialcombo::newlineModeOf(m_newline));
    m_bridge->setLocalEcho(m_localEcho->isChecked());
}

SerialTerminalWidget::~SerialTerminalWidget()
{
    // 先停桥（断开与 Session 的连接），再让 client 随父对象析构关端口。
    if (m_bridge)
        m_bridge->stop();
}

SerialSettings SerialTerminalWidget::currentSettings() const
{
    SerialSettings s;
    // 可编辑下拉框：手输时 currentData() 为空，回退到文本。
    const QString fromData = m_port->currentData().toString();
    s.portName    = fromData.isEmpty() ? m_port->currentText().trimmed() : fromData;
    s.baudRate    = serialcombo::baudRateOf(m_baud);
    s.dataBits    = serialcombo::dataBitsOf(m_dataBits);
    s.parity      = serialcombo::parityOf(m_parity);
    s.stopBits    = serialcombo::stopBitsOf(m_stopBits);
    s.flowControl = serialcombo::flowControlOf(m_flow);
    s.newlineMode = serialcombo::newlineModeOf(m_newline);
    s.localEcho   = m_localEcho->isChecked();
    return s;
}

void SerialTerminalWidget::setSettings(const SerialSettings &s)
{
    serialcombo::fillPorts(m_port, s.portName);
    if (s.baudRate > 0)
        m_baud->setCurrentText(QString::number(s.baudRate));
    serialcombo::selectData(m_dataBits, int(s.dataBits));
    serialcombo::selectData(m_parity,   int(s.parity));
    serialcombo::selectData(m_stopBits, int(s.stopBits));
    serialcombo::selectData(m_flow,     int(s.flowControl));
    serialcombo::selectData(m_newline,  int(s.newlineMode));
    m_localEcho->setChecked(s.localEcho);
    // setChecked/selectData 会触发对应槽，bridge 状态自动跟着更新。
}

void SerialTerminalWidget::connectToPort()
{
    onConnectClicked();
}

void SerialTerminalWidget::onConnectClicked()
{
    const SerialSettings s = currentSettings();
    if (s.portName.isEmpty()) {
        m_lastError = tr("请选择串口设备。");
        m_statusLabel->setText(m_lastError);
        emit connectionFailed(m_lastError);
        return;
    }

    m_lastError.clear();
    if (!m_client->open(s)) {
        // 具体原因由 onError 存进 m_lastError（open() 内部已 emit）。
        const QString message = m_lastError.isEmpty()
                                    ? tr("打开串口 %1 失败。").arg(s.portName)
                                    : m_lastError;
        m_statusLabel->setText(message);
        emit connectionFailed(message);
        return;
    }

    // 连上后把换行/回显设置同步给桥（open 不经过这两个槽）。
    m_bridge->setNewlineMode(s.newlineMode);
    m_bridge->setLocalEcho(s.localEcho);
    m_term->setFocus();
}

void SerialTerminalWidget::onDisconnectClicked()
{
    m_client->close();
}

void SerialTerminalWidget::onStateChanged(SerialClient::State state)
{
    const bool open = (state == SerialClient::State::Connected);
    setFormEnabled(!open);
    m_connectButton->setEnabled(!open);
    m_disconnectButton->setEnabled(open);
    updateStatus();

    if (open)
        emit connected();
    else
        emit disconnected();
}

void SerialTerminalWidget::onError(const QString &message)
{
    m_lastError = message;
    m_statusLabel->setText(message);
}

void SerialTerminalWidget::onPortsChanged()
{
    // 连接中不动端口下拉框：此时它是禁用的，重填会把用户看到的选择顶掉。
    // 断开后（含被拔线自动断开）再刷新，用户就能看到新的端口列表。
    if (m_client->isOpen())
        return;
    serialcombo::fillPorts(m_port);
}

void SerialTerminalWidget::onNewlineModeChanged()
{
    if (m_bridge)
        m_bridge->setNewlineMode(serialcombo::newlineModeOf(m_newline));
}

void SerialTerminalWidget::onLocalEchoToggled(bool enabled)
{
    if (m_bridge)
        m_bridge->setLocalEcho(enabled);
}

void SerialTerminalWidget::onLogToggled(bool enabled)
{
    if (!enabled) {
        m_client->setLogFile(QString());
        updateStatus();
        return;
    }

    const QString suggested = QStringLiteral("serial-%1.log")
        .arg(currentSettings().portName.isEmpty()
                 ? QStringLiteral("session")
                 // 端口名里的 / 会被当成路径分隔符（/dev/ttyUSB0），换掉。
                 : QString(currentSettings().portName).replace(QLatin1Char('/'),
                                                               QLatin1Char('_')));
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

void SerialTerminalWidget::onClearClicked()
{
    if (m_term)
        m_term->clear();
}

void SerialTerminalWidget::setFormEnabled(bool enabled)
{
    // 连接中锁死物理链路参数（改这些要重开端口）；换行/回显/日志是纯终端
    // 行为，连接中也允许改。
    m_port->setEnabled(enabled);
    m_baud->setEnabled(enabled);
    m_dataBits->setEnabled(enabled);
    m_parity->setEnabled(enabled);
    m_stopBits->setEnabled(enabled);
    m_flow->setEnabled(enabled);
}

void SerialTerminalWidget::updateStatus()
{
    if (!m_client->isOpen()) {
        m_statusLabel->setText(tr("未连接"));
        return;
    }
    const SerialSettings s = m_client->settings();
    QString text = tr("已连接 %1 @%2 %3")
                       .arg(s.portName)
                       .arg(s.baudRate)
                       .arg(s.frameFormat());
    if (m_client->isLogging())
        text += tr("　录制中：%1").arg(m_client->logFilePath());
    m_statusLabel->setText(text);
}

} // namespace cubeshell
