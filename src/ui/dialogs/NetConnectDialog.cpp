// NetConnectDialog.cpp — “新建 Telnet/TCP 连接”对话框 + 参数下拉框辅助。
// 见 NetConnectDialog.h。

#include "NetConnectDialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVBoxLayout>

namespace cubeshell {

namespace netcombo {

namespace {

// 让下拉框宽到能完整显示最宽的一项。与 SerialConnectDialog.cpp 里的同名函数
// 同因同解（主题 QSS 给 QComboBox 加了 padding，sizeFromContents 算出的宽度
// 一点余量都没有，字体/DPI 稍变就被省略号截断），那边的注释有完整说明。
// 没有共用是因为 SerialConnectDialog 只在 CUBESHELL_WITH_SERIAL=ON 时编译。
void fitToWidestItem(QComboBox *combo)
{
    if (!combo || combo->count() == 0)
        return;
    const QFontMetrics fm(combo->fontMetrics());
    int textWidth = 0;
    for (int i = 0; i < combo->count(); ++i)
        textWidth = qMax(textWidth, fm.horizontalAdvance(combo->itemText(i)));
    textWidth += fm.horizontalAdvance(QLatin1Char('M'));   // 余量

    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    opt.editable = combo->isEditable();
    const int full = combo->style()
                         ->sizeFromContents(QStyle::CT_ComboBox, &opt,
                                            QSize(textWidth, fm.height()), combo)
                         .width();
    if (QAbstractItemView *view = combo->view())
        view->setMinimumWidth(full);
    combo->setMinimumWidth(full);
}

} // namespace

void fillMode(QComboBox *combo, const QString &preferred)
{
    if (!combo)
        return;
    combo->clear();
    // 协议名是专有名词，不翻译（与 SSH/RDP/Serial 一致）。
    combo->addItem(QStringLiteral("Telnet"), QStringLiteral("telnet"));
    combo->addItem(QStringLiteral("TCP"),    QStringLiteral("tcp"));
    const int idx = combo->findData(preferred.isEmpty() ? QStringLiteral("telnet")
                                                        : preferred);
    combo->setCurrentIndex(idx < 0 ? 0 : idx);
    fitToWidestItem(combo);
}

QString modeOf(const QComboBox *combo)
{
    if (!combo)
        return QStringLiteral("telnet");
    const QString v = combo->currentData().toString();
    return v == QLatin1String("tcp") ? v : QStringLiteral("telnet");
}

void fillNewlineMode(QComboBox *combo, TcpSettings::NewlineMode preferred)
{
    if (!combo)
        return;
    combo->clear();
    combo->addItem(QStringLiteral("CR (\\r)"),      int(TcpSettings::NewlineMode::Cr));
    combo->addItem(QStringLiteral("LF (\\n)"),      int(TcpSettings::NewlineMode::Lf));
    combo->addItem(QStringLiteral("CRLF (\\r\\n)"), int(TcpSettings::NewlineMode::CrLf));
    const int idx = combo->findData(int(preferred));
    combo->setCurrentIndex(idx < 0 ? 0 : idx);
    fitToWidestItem(combo);
}

TcpSettings::NewlineMode newlineModeOf(const QComboBox *combo)
{
    if (!combo)
        return TcpSettings::NewlineMode::CrLf;
    bool ok = false;
    const int v = combo->currentData().toInt(&ok);
    return ok ? TcpSettings::NewlineMode(v) : TcpSettings::NewlineMode::CrLf;
}

void fillTermTypes(QComboBox *combo, const QByteArray &preferred)
{
    if (!combo)
        return;
    combo->clear();
    combo->setEditable(true);   // 设备可能只认某个私有终端名，允许手输
    combo->setInsertPolicy(QComboBox::NoInsert);
    // 顺序即优先级：xterm-256color 是现代默认，vt100/ansi 留给老网络设备。
    combo->addItems({QStringLiteral("xterm-256color"), QStringLiteral("xterm"),
                     QStringLiteral("vt100"), QStringLiteral("vt220"),
                     QStringLiteral("ansi"), QStringLiteral("linux")});
    const QString want = preferred.isEmpty() ? QStringLiteral("xterm-256color")
                                             : QString::fromUtf8(preferred);
    const int idx = combo->findText(want);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
    else
        combo->setCurrentText(want);
    fitToWidestItem(combo);
}

QByteArray termTypeOf(const QComboBox *combo)
{
    if (!combo)
        return QByteArrayLiteral("xterm-256color");
    const QByteArray v = combo->currentText().trimmed().toUtf8();
    return v.isEmpty() ? QByteArrayLiteral("xterm-256color") : v;
}

void selectData(QComboBox *combo, const QVariant &value)
{
    if (!combo)
        return;
    const int idx = combo->findData(value);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

TcpSettings::NewlineMode newlineModeFromString(const QString &s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("cr")) return TcpSettings::NewlineMode::Cr;
    if (v == QLatin1String("lf")) return TcpSettings::NewlineMode::Lf;
    // 空串/未知才落 CRLF（RFC 854 规定的 Telnet 行尾）。显式的 "cr" 必须原样
    // 尊重——CR 是换行下拉框里用户可主动选的一项（fillNewlineMode），在这里
    // 纠成 CRLF 会让用户的选择存进去又读不回来。配置缺键的场景由 core 层的
    // defaultNewlineModeFor(protocol) 按协议回落，不归这里管。
    return TcpSettings::NewlineMode::CrLf;
}

QString newlineModeToString(TcpSettings::NewlineMode m)
{
    switch (m) {
    case TcpSettings::NewlineMode::Cr: return QStringLiteral("cr");
    case TcpSettings::NewlineMode::Lf: return QStringLiteral("lf");
    default:                           return QStringLiteral("crlf");
    }
}

} // namespace netcombo

TcpSettings netSettingsFromDevice(const DeviceEntry &device)
{
    TcpSettings s;
    s.mode = device.isTcp() ? QStringLiteral("tcp") : QStringLiteral("telnet");
    const HostPort hp = device.hostPort();
    s.host = hp.host;
    s.port = hp.port;
    s.newlineMode  = netcombo::newlineModeFromString(device.newlineMode);
    s.localEcho    = device.localEcho;
    s.rxImplicitCr = device.rxImplicitCr;
    s.negotiate    = device.telnetNegotiate;
    s.termType     = device.termType.isEmpty()
                         ? QByteArrayLiteral("xterm-256color")
                         : device.termType.toUtf8();
    s.autoLogin    = device.autoLogin;
    s.username     = device.username;
    s.password     = device.password;
    return s;
}

// --- NetConnectDialog ---

NetConnectDialog::NetConnectDialog(const QString &mode, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode == QLatin1String("tcp") ? QStringLiteral("tcp")
                                          : QStringLiteral("telnet"))
{
    setWindowTitle(isTelnet() ? tr("新建 Telnet 连接") : tr("新建 TCP 连接"));
    setMinimumWidth(420);

    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("请输入主机名或IP地址（支持IPv6）"));

    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(defaultPortFor(m_mode));

    m_newline = new QComboBox(this);
    // Telnet 的行尾由 RFC 854 规定为 CR LF；裸 TCP 对端多半是个 Unix 侧的
    // 服务（nc/socat/自研 server），发 CR 过去会在对端显示成 ^M，故默认 LF。
    netcombo::fillNewlineMode(m_newline, isTelnet() ? TcpSettings::NewlineMode::CrLf
                                                    : TcpSettings::NewlineMode::Lf);

    m_localEcho = new QCheckBox(tr("本地回显（对端不回显输入时勾选）"), this);
    // 裸 TCP 对端一般不回显（nc 就不回），默认打开才看得见自己输入；
    // Telnet 服务端通常会 WILL ECHO 接管回显，默认关闭避免双回显。
    m_localEcho->setChecked(!isTelnet());
    m_rxImplicitCr = new QCheckBox(
        tr("接收时给孤立的 LF 补 CR（对端发裸 \\n 时勾选）"), this);
    m_rxImplicitCr->setChecked(true);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("主机："), m_host);
    form->addRow(tr("端口："), m_port);

    if (isTelnet()) {
        m_username = new QLineEdit(this);
        m_username->setPlaceholderText(tr("自动登录用，可留空"));
        m_password = new QLineEdit(this);
        m_password->setEchoMode(QLineEdit::Password);
        m_password->setPlaceholderText(tr("自动登录用，可留空"));

        m_termType = new QComboBox(this);
        netcombo::fillTermTypes(m_termType);

        m_negotiate = new QCheckBox(tr("选项协商（NAWS / 终端类型 / SGA）"), this);
        m_negotiate->setChecked(true);
        m_autoLogin = new QCheckBox(tr("自动登录（匹配 login: / Password: 提示）"), this);
        m_autoLogin->setChecked(false);
        // 自动登录必须有用户名才有意义，勾上时把焦点引到用户名框。
        connect(m_autoLogin, &QCheckBox::toggled, this, [this](bool on) {
            if (on && m_username->text().trimmed().isEmpty())
                m_username->setFocus();
        });

        form->addRow(tr("用户名："),   m_username);
        form->addRow(tr("密码："),     m_password);
        form->addRow(tr("终端类型："), m_termType);
        form->addRow(QString(), m_negotiate);
        form->addRow(QString(), m_autoLogin);
    }

    // 标签写明"发送"：这个下拉框只管发出去的字节，接收方向由下面的复选框负责。
    // 与串口两处保持一致的措辞。
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

    connect(buttons, &QDialogButtonBox::accepted, this, &NetConnectDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NetConnectDialog::reject);
}

TcpSettings NetConnectDialog::settings() const
{
    TcpSettings s;
    s.mode = m_mode;
    s.host = m_host->text().trimmed();
    s.port = quint16(m_port->value());
    s.newlineMode  = netcombo::newlineModeOf(m_newline);
    s.localEcho    = m_localEcho->isChecked();
    s.rxImplicitCr = m_rxImplicitCr->isChecked();
    if (isTelnet()) {
        s.negotiate = m_negotiate->isChecked();
        s.termType  = netcombo::termTypeOf(m_termType);
        s.autoLogin = m_autoLogin->isChecked();
        s.username  = m_username->text().trimmed();
        s.password  = m_password->text();
    }
    return s;
}

void NetConnectDialog::setSettings(const TcpSettings &s)
{
    m_host->setText(s.host);
    m_port->setValue(s.port ? s.port : defaultPortFor(m_mode));
    netcombo::selectData(m_newline, int(s.newlineMode));
    m_localEcho->setChecked(s.localEcho);
    m_rxImplicitCr->setChecked(s.rxImplicitCr);
    if (isTelnet()) {
        m_negotiate->setChecked(s.negotiate);
        netcombo::fillTermTypes(m_termType, s.termType);
        m_autoLogin->setChecked(s.autoLogin);
        m_username->setText(s.username);
        m_password->setText(s.password);
    }
}

void NetConnectDialog::accept()
{
    const QString title = isTelnet() ? tr("新建 Telnet 连接") : tr("新建 TCP 连接");
    if (m_host->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, title, tr("请输入主机名或IP地址。"));
        return;
    }
    if (isTelnet() && m_autoLogin->isChecked()
            && m_username->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, title, tr("启用自动登录时必须填写用户名。"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
