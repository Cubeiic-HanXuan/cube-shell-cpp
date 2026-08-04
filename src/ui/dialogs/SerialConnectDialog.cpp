// SerialConnectDialog.cpp — “新建串口连接”对话框 + 串口参数下拉框辅助。
// 见 SerialConnectDialog.h。

#include "SerialConnectDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

namespace serialcombo {

void fillPorts(QComboBox *combo, const QString &preferred)
{
    if (!combo)
        return;
    const QString keep = preferred.isEmpty() ? combo->currentData().toString()
                                             : preferred;
    combo->clear();
    const auto ports = SerialClient::availablePorts();
    for (const SerialPortDesc &p : ports)
        combo->addItem(p.displayName(), p.portName);

    if (ports.isEmpty()) {
        // 没有可用串口时留一条提示项，比空下拉框更好懂。
        // 下拉框是可编辑的，用户仍可手输设备路径。
        combo->addItem(QObject::tr("(未检测到串口)"), QString());
    }
    if (!keep.isEmpty()) {
        const int idx = combo->findData(keep);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
        else if (combo->isEditable())
            combo->setCurrentText(keep);   // 端口暂时不在（未插入），保留用户的选择
    }
}

void fillBaudRates(QComboBox *combo)
{
    if (!combo)
        return;
    const QList<qint32> rates = {1200, 2400, 4800, 9600, 19200, 38400,
                                 57600, 115200, 230400, 460800, 921600};
    for (qint32 r : rates)
        combo->addItem(QString::number(r), r);
    combo->setCurrentIndex(combo->findData(115200));
}

void fillDataBits(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QStringLiteral("5"), int(QSerialPort::Data5));
    combo->addItem(QStringLiteral("6"), int(QSerialPort::Data6));
    combo->addItem(QStringLiteral("7"), int(QSerialPort::Data7));
    combo->addItem(QStringLiteral("8"), int(QSerialPort::Data8));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::Data8)));
}

void fillParity(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QObject::tr("无 (N)"),   int(QSerialPort::NoParity));
    combo->addItem(QObject::tr("偶 (E)"),   int(QSerialPort::EvenParity));
    combo->addItem(QObject::tr("奇 (O)"),   int(QSerialPort::OddParity));
    combo->addItem(QObject::tr("恒1 (M)"),  int(QSerialPort::MarkParity));
    combo->addItem(QObject::tr("恒0 (S)"),  int(QSerialPort::SpaceParity));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::NoParity)));
}

void fillStopBits(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QStringLiteral("1"),   int(QSerialPort::OneStop));
    combo->addItem(QStringLiteral("1.5"), int(QSerialPort::OneAndHalfStop));
    combo->addItem(QStringLiteral("2"),   int(QSerialPort::TwoStop));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::OneStop)));
}

void fillFlowControl(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QObject::tr("无"),           int(QSerialPort::NoFlowControl));
    combo->addItem(QObject::tr("硬件 RTS/CTS"), int(QSerialPort::HardwareControl));
    combo->addItem(QObject::tr("软件 XON/XOFF"), int(QSerialPort::SoftwareControl));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::NoFlowControl)));
}

void fillNewlineMode(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QStringLiteral("CR (\\r)"),     int(SerialSettings::NewlineMode::Cr));
    combo->addItem(QStringLiteral("LF (\\n)"),     int(SerialSettings::NewlineMode::Lf));
    combo->addItem(QStringLiteral("CRLF (\\r\\n)"), int(SerialSettings::NewlineMode::CrLf));
    combo->setCurrentIndex(combo->findData(int(SerialSettings::NewlineMode::Cr)));
}

qint32 baudRateOf(const QComboBox *combo)
{
    if (!combo)
        return 115200;
    // 可编辑下拉框：用户手输的波特率没有 userData，回退到解析文本。
    bool ok = false;
    const qint32 fromData = combo->currentData().toInt(&ok);
    if (ok && fromData > 0)
        return fromData;
    const qint32 fromText = combo->currentText().trimmed().toInt(&ok);
    return (ok && fromText > 0) ? fromText : 115200;
}

QSerialPort::DataBits dataBitsOf(const QComboBox *combo)
{
    if (!combo)
        return QSerialPort::Data8;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? QSerialPort::DataBits(v) : QSerialPort::Data8;
}

QSerialPort::Parity parityOf(const QComboBox *combo)
{
    if (!combo)
        return QSerialPort::NoParity;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? QSerialPort::Parity(v) : QSerialPort::NoParity;
}

QSerialPort::StopBits stopBitsOf(const QComboBox *combo)
{
    if (!combo)
        return QSerialPort::OneStop;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? QSerialPort::StopBits(v) : QSerialPort::OneStop;
}

QSerialPort::FlowControl flowControlOf(const QComboBox *combo)
{
    if (!combo)
        return QSerialPort::NoFlowControl;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? QSerialPort::FlowControl(v) : QSerialPort::NoFlowControl;
}

SerialSettings::NewlineMode newlineModeOf(const QComboBox *combo)
{
    if (!combo)
        return SerialSettings::NewlineMode::Cr;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? SerialSettings::NewlineMode(v) : SerialSettings::NewlineMode::Cr;
}

void selectData(QComboBox *combo, const QVariant &value)
{
    if (!combo)
        return;
    const int idx = combo->findData(value);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

// --- 字符串 ↔ 枚举（DeviceEntry 持久化用） ---

QSerialPort::Parity parityFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("even"))  return QSerialPort::EvenParity;
    if (v == QLatin1String("odd"))   return QSerialPort::OddParity;
    if (v == QLatin1String("mark"))  return QSerialPort::MarkParity;
    if (v == QLatin1String("space")) return QSerialPort::SpaceParity;
    return QSerialPort::NoParity;
}

QString parityToString(QSerialPort::Parity p)
{
    switch (p) {
    case QSerialPort::EvenParity:  return QStringLiteral("even");
    case QSerialPort::OddParity:   return QStringLiteral("odd");
    case QSerialPort::MarkParity:  return QStringLiteral("mark");
    case QSerialPort::SpaceParity: return QStringLiteral("space");
    default:                       return QStringLiteral("none");
    }
}

QSerialPort::StopBits stopBitsFromString(const QString &s)
{
    const QString v = s.trimmed();
    if (v == QLatin1String("1.5")) return QSerialPort::OneAndHalfStop;
    if (v == QLatin1String("2"))   return QSerialPort::TwoStop;
    return QSerialPort::OneStop;
}

QString stopBitsToString(QSerialPort::StopBits s)
{
    switch (s) {
    case QSerialPort::OneAndHalfStop: return QStringLiteral("1.5");
    case QSerialPort::TwoStop:        return QStringLiteral("2");
    default:                          return QStringLiteral("1");
    }
}

QSerialPort::FlowControl flowControlFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("hardware")) return QSerialPort::HardwareControl;
    if (v == QLatin1String("software")) return QSerialPort::SoftwareControl;
    return QSerialPort::NoFlowControl;
}

QString flowControlToString(QSerialPort::FlowControl f)
{
    switch (f) {
    case QSerialPort::HardwareControl: return QStringLiteral("hardware");
    case QSerialPort::SoftwareControl: return QStringLiteral("software");
    default:                           return QStringLiteral("none");
    }
}

QSerialPort::DataBits dataBitsFromInt(int bits)
{
    switch (bits) {
    case 5:  return QSerialPort::Data5;
    case 6:  return QSerialPort::Data6;
    case 7:  return QSerialPort::Data7;
    default: return QSerialPort::Data8;
    }
}

SerialSettings::NewlineMode newlineModeFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("lf"))   return SerialSettings::NewlineMode::Lf;
    if (v == QLatin1String("crlf")) return SerialSettings::NewlineMode::CrLf;
    return SerialSettings::NewlineMode::Cr;
}

QString newlineModeToString(SerialSettings::NewlineMode m)
{
    switch (m) {
    case SerialSettings::NewlineMode::Lf:   return QStringLiteral("lf");
    case SerialSettings::NewlineMode::CrLf: return QStringLiteral("crlf");
    default:                                return QStringLiteral("cr");
    }
}

} // namespace serialcombo

SerialSettings serialSettingsFromDevice(const DeviceEntry &device)
{
    SerialSettings s;
    s.portName    = device.portName;
    s.baudRate    = device.baudRate > 0 ? device.baudRate : 115200;
    s.dataBits    = serialcombo::dataBitsFromInt(device.dataBits);
    s.parity      = serialcombo::parityFromString(device.parity);
    s.stopBits    = serialcombo::stopBitsFromString(device.stopBits);
    s.flowControl = serialcombo::flowControlFromString(device.flowControl);
    s.newlineMode = serialcombo::newlineModeFromString(device.newlineMode);
    s.localEcho   = device.localEcho;
    return s;
}

// --- SerialConnectDialog ---

SerialConnectDialog::SerialConnectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("新建串口连接"));
    setMinimumWidth(420);

    m_port = new QComboBox(this);
    m_port->setEditable(true);   // 允许手输设备路径（如 /dev/cu.usbserial-A50285BI）
    m_port->setInsertPolicy(QComboBox::NoInsert);
    serialcombo::fillPorts(m_port);

    auto *refresh = new QPushButton(tr("刷新"), this);
    auto *portRow = new QWidget(this);
    auto *portLay = new QHBoxLayout(portRow);
    portLay->setContentsMargins(0, 0, 0, 0);
    portLay->addWidget(m_port, 1);
    portLay->addWidget(refresh);

    m_baud = new QComboBox(this);
    m_baud->setEditable(true);   // 非标准波特率（如 250000）可手输
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

    m_localEcho = new QCheckBox(tr("本地回显（设备不回显输入时勾选）"), this);

    auto *form = new QFormLayout;
    form->addRow(tr("串口设备："), portRow);
    form->addRow(tr("波特率："),   m_baud);
    form->addRow(tr("数据位："),   m_dataBits);
    form->addRow(tr("校验位："),   m_parity);
    form->addRow(tr("停止位："),   m_stopBits);
    form->addRow(tr("流控："),     m_flow);
    form->addRow(tr("换行符："),   m_newline);
    form->addRow(QString(), m_localEcho);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(refresh, &QPushButton::clicked, this, &SerialConnectDialog::onRefreshPorts);
    connect(buttons, &QDialogButtonBox::accepted, this, &SerialConnectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SerialConnectDialog::reject);
}

void SerialConnectDialog::onRefreshPorts()
{
    serialcombo::fillPorts(m_port);
}

SerialSettings SerialConnectDialog::settings() const
{
    SerialSettings s;
    // 可编辑下拉框：手输时 currentData() 是空的，回退到文本。
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

void SerialConnectDialog::setSettings(const SerialSettings &s)
{
    serialcombo::fillPorts(m_port, s.portName);
    m_baud->setCurrentText(QString::number(s.baudRate));
    serialcombo::selectData(m_dataBits, int(s.dataBits));
    serialcombo::selectData(m_parity,   int(s.parity));
    serialcombo::selectData(m_stopBits, int(s.stopBits));
    serialcombo::selectData(m_flow,     int(s.flowControl));
    serialcombo::selectData(m_newline,  int(s.newlineMode));
    m_localEcho->setChecked(s.localEcho);
}

void SerialConnectDialog::accept()
{
    if (settings().portName.isEmpty()) {
        QMessageBox::warning(this, tr("新建串口连接"),
                             tr("请选择或输入串口设备。"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
