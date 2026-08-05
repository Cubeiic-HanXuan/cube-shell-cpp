// SerialConnectDialog.cpp — “新建串口连接”对话框 + 串口参数下拉框辅助。
// 见 SerialConnectDialog.h。

#include "SerialConnectDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QAbstractItemView>
#include <QSizePolicy>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVBoxLayout>

namespace cubeshell {

namespace serialcombo {

namespace {

// 可编辑下拉框判定「当前文本是不是用户手输的」。
//
// 关键点：QComboBox 可编辑时，用户在输入框里打字**不会**改动 currentIndex，
// 它仍停在上一次选中的那一项上。于是 currentData() 返回的是那一项的值，
// 而不是用户刚输入的内容 —— 直接用 currentData() 会把手输值悄悄丢掉。
// 由于本模块的端口下拉框显示文本（"cu.usbserial-0001 — CP2102"）与 userData
// （"cu.usbserial-0001"）刻意不同，这个坑不会自己暴露成空值。
// 判据：当前文本若已不等于该项的显示文本，就是手输。
bool isCustomText(const QComboBox *combo)
{
    const int idx = combo->currentIndex();
    return idx < 0
        || combo->itemText(idx).trimmed() != combo->currentText().trimmed();
}

// 让下拉框宽到能完整显示**最宽的一项**，而不只是当前选中项。
//
// 两个叠加的原因让这些框显示不全：
//  1. QComboBox 默认的 sizeHint 只按当前项算宽度。"校验"选中"无 (N)"时框就
//     照"无 (N)"定宽，而列表里的"恒1 (M)"更宽，展开后被省略号截断。
//  2. 主题 QSS 给 QComboBox 设了 padding 和 drop-down 负 margin，
//     style()->sizeFromContents 算出的宽度正好卡在文本宽度上（实测文本区
//     与最宽文本同为 40px / 64px，一点余量都没有）。字体、DPI 稍有差异就露馅。
//
// shrinkable=true 用于端口下拉框：端口名可以很长
//（"tty.Bluetooth-Incoming-Port"），按最宽项定死会把整条工具栏撑到分栏放不下。
// 这类框只放宽弹出列表，输入框本身允许被压缩——反正展开时看得全。
void fitToWidestItem(QComboBox *combo, bool shrinkable = false)
{
    if (!combo || combo->count() == 0)
        return;
    const QFontMetrics fm(combo->fontMetrics());
    int textWidth = 0;
    for (int i = 0; i < combo->count(); ++i) {
        // horizontalAdvance 与省略号判定用的是同一套度量，别用 boundingRect。
        textWidth = qMax(textWidth, fm.horizontalAdvance(combo->itemText(i)));
    }
    // 一个 'M' 的宽度作余量，吸收 QSS padding 与字体度量的误差。
    textWidth += fm.horizontalAdvance(QLatin1Char('M'));

    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    opt.editable = combo->isEditable();
    const int full = combo->style()
                         ->sizeFromContents(QStyle::CT_ComboBox, &opt,
                                            QSize(textWidth, fm.height()), combo)
                         .width();

    // 弹出列表宽度独立于输入框，长名字/长描述在展开时始终看得全。
    if (QAbstractItemView *view = combo->view())
        view->setMinimumWidth(full);

    if (shrinkable) {
        // 给个够用的下限（约 16 个字符），再窄就真的没法认了。
        const int floorW = qMin(full, fm.horizontalAdvance(QLatin1Char('0')) * 16);
        combo->setMinimumWidth(floorW);
        combo->setSizePolicy(QSizePolicy::Expanding, combo->sizePolicy().verticalPolicy());
    } else {
        combo->setMinimumWidth(full);
    }
}

} // namespace

QString portNameOf(const QComboBox *combo)
{
    if (!combo)
        return QString();
    const QString text = combo->currentText().trimmed();
    if (isCustomText(combo))
        return text;   // 用户手输的设备路径，原样返回
    // 选中的是列表项：取 userData 里的纯端口名（显示文本还带着描述后缀）。
    // "(未检测到串口)" 占位项的 data 是空的，返回空串交给调用方校验拦截。
    return combo->itemData(combo->currentIndex()).toString();
}

void fillPorts(QComboBox *combo, const QString &preferred)
{
    if (!combo)
        return;
    const QString keep = preferred.isEmpty() ? portNameOf(combo) : preferred;
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
    fitToWidestItem(combo, /*shrinkable=*/true);
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
    fitToWidestItem(combo);
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
    fitToWidestItem(combo);
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
    fitToWidestItem(combo);
}

void fillStopBits(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QStringLiteral("1"),   int(QSerialPort::OneStop));
    combo->addItem(QStringLiteral("1.5"), int(QSerialPort::OneAndHalfStop));
    combo->addItem(QStringLiteral("2"),   int(QSerialPort::TwoStop));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::OneStop)));
    fitToWidestItem(combo);
}

void fillFlowControl(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QObject::tr("无"),           int(QSerialPort::NoFlowControl));
    combo->addItem(QObject::tr("硬件 RTS/CTS"), int(QSerialPort::HardwareControl));
    combo->addItem(QObject::tr("软件 XON/XOFF"), int(QSerialPort::SoftwareControl));
    combo->setCurrentIndex(combo->findData(int(QSerialPort::NoFlowControl)));
    fitToWidestItem(combo);
}

void fillNewlineMode(QComboBox *combo)
{
    if (!combo)
        return;
    combo->addItem(QStringLiteral("CR (\\r)"),     int(SerialSettings::NewlineMode::Cr));
    combo->addItem(QStringLiteral("LF (\\n)"),     int(SerialSettings::NewlineMode::Lf));
    combo->addItem(QStringLiteral("CRLF (\\r\\n)"), int(SerialSettings::NewlineMode::CrLf));
    combo->setCurrentIndex(combo->findData(int(SerialSettings::NewlineMode::Cr)));
    fitToWidestItem(combo);
}

qint32 baudRateOf(const QComboBox *combo)
{
    if (!combo)
        return 115200;
    // 同 portNameOf：可编辑下拉框手输时 currentIndex 不动，currentData() 仍是
    // 上一次选中项的值，非标准波特率（31250 等）会被悄悄改回选中项。
    bool ok = false;
    if (!isCustomText(combo)) {
        const qint32 fromData = combo->currentData().toInt(&ok);
        if (ok && fromData > 0)
            return fromData;
    }
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
    s.rxImplicitCr = device.rxImplicitCr;
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
    m_rxImplicitCr = new QCheckBox(tr("接收时给孤立的 LF 补 CR（设备发裸 \\n 时勾选）"), this);
    m_rxImplicitCr->setChecked(true);

    auto *form = new QFormLayout;
    form->addRow(tr("串口设备："), portRow);
    form->addRow(tr("波特率："),   m_baud);
    form->addRow(tr("数据位："),   m_dataBits);
    form->addRow(tr("校验位："),   m_parity);
    form->addRow(tr("停止位："),   m_stopBits);
    form->addRow(tr("流控："),     m_flow);
    // 标签写明"发送"：这个下拉框只管发出去的字节，接收方向由下面的复选框负责。
    // 详见 serial_terminal_widget.cpp 里同一处的说明。
    form->addRow(tr("发送换行符："), m_newline);
    form->addRow(QString(), m_localEcho);
    form->addRow(QString(), m_rxImplicitCr);

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
    s.portName    = serialcombo::portNameOf(m_port);
    s.baudRate    = serialcombo::baudRateOf(m_baud);
    s.dataBits    = serialcombo::dataBitsOf(m_dataBits);
    s.parity      = serialcombo::parityOf(m_parity);
    s.stopBits    = serialcombo::stopBitsOf(m_stopBits);
    s.flowControl = serialcombo::flowControlOf(m_flow);
    s.newlineMode = serialcombo::newlineModeOf(m_newline);
    s.localEcho   = m_localEcho->isChecked();
    s.rxImplicitCr = m_rxImplicitCr->isChecked();
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
    m_rxImplicitCr->setChecked(s.rxImplicitCr);
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
