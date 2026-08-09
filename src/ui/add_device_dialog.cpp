#include "add_device_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "dialogs/NetConnectDialog.h"      // netcombo 辅助函数（无条件可用）
#ifdef CUBESHELL_WITH_SERIAL
#include "dialogs/SerialConnectDialog.h"   // serialcombo 辅助函数
#endif

namespace cubeshell {

AddDeviceDialog::AddDeviceDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("添加设备"));
    setMinimumWidth(420);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("请输入配置名称"));
    m_username = new QLineEdit(this);
    m_username->setPlaceholderText(tr("请输入终端用户名"));
    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("请输入IP地址（支持IPv6）"));
    m_port = new QLineEdit(QStringLiteral("22"), this);
    m_portDefaultFor = QStringLiteral("ssh");

    m_authMethod = new QComboBox(this);
    m_authMethod->addItem(tr("密码登录"));
    m_authMethod->addItem(tr("私钥登录"));

    // --- auth stack ---
    m_authStack = new QStackedWidget(this);

    // page 0: password
    auto *pwPage = new QWidget(this);
    auto *pwForm = new QFormLayout(pwPage);
    pwForm->setContentsMargins(0, 0, 0, 0);
    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("终端密码可以不输入"));
    pwForm->addRow(tr("密码："), m_password);

    // page 1: private key
    auto *keyPage = new QWidget(this);
    auto *keyForm = new QFormLayout(keyPage);
    keyForm->setContentsMargins(0, 0, 0, 0);
    m_keyType = new QComboBox(this);
    m_keyType->addItems({QStringLiteral("Ed25519Key"), QStringLiteral("RSAKey"),
                         QStringLiteral("ECDSAKey"), QStringLiteral("DSSKey")});
    m_keyFile = new QLineEdit(this);
    m_keyFile->setPlaceholderText(tr("私钥文件路径"));
    m_browseKey = new QPushButton(tr("浏览…"), this);
    auto *keyFileRow = new QWidget(this);
    auto *keyFileLay = new QHBoxLayout(keyFileRow);
    keyFileLay->setContentsMargins(0, 0, 0, 0);
    keyFileLay->addWidget(m_keyFile, 1);
    keyFileLay->addWidget(m_browseKey);
    keyForm->addRow(tr("私钥类型："), m_keyType);
    keyForm->addRow(tr("私钥文件："), keyFileRow);

    m_authStack->addWidget(pwPage);
    m_authStack->addWidget(keyPage);

    // --- main form ---
    auto *form = new QFormLayout;
    m_form = form;
    // Row 0: 连接类型选择器（配置名之前）。
    // 对应Python: _inject_protocol_fields（cube-shell.py:6017-6022）
    // 协议名是专有名词，不翻译；userData 才是判定依据（见 selectedProtocol）。
    m_protocol = new QComboBox(this);
    // 图标与设备列表树的取图标分支保持一致（device_list_widget.cpp）。
    m_protocol->addItem(QIcon(QStringLiteral(":/icons8-ssh-48.png")),
                        QStringLiteral("SSH"), QStringLiteral("ssh"));
#ifdef CUBESHELL_WITH_RDP
    m_protocol->addItem(QIcon(QStringLiteral(":/icons8-windows-48.png")),
                        QStringLiteral("RDP"), QStringLiteral("rdp"));
#endif
#ifdef CUBESHELL_WITH_SERIAL
    m_protocol->addItem(QIcon(QStringLiteral(":/icons8-serial-48.png")),
                        QStringLiteral("Serial"), QStringLiteral("serial"));
#endif
    m_protocol->addItem(QIcon(QStringLiteral(":/icons8-telnet-48.png")),
                        QStringLiteral("Telnet"), QStringLiteral("telnet"));
    m_protocol->addItem(QIcon(QStringLiteral(":/icons8-tcp-48.png")),
                        QStringLiteral("TCP"), QStringLiteral("tcp"));
    form->addRow(tr("连接类型："), m_protocol);

    form->addRow(tr("配置名："), m_name);
    form->addRow(tr("用户名："), m_username);
    form->addRow(tr("IP地址："), m_host);
    form->addRow(tr("端口："), m_port);
    form->addRow(tr("认证方式："), m_authMethod);
    form->addRow(m_authStack);
#ifdef CUBESHELL_WITH_RDP
    // RDP 认证方式 + 域（仅 RDP 时可见）。
    // 对应Python: _inject_protocol_fields（cube-shell.py:6024-6033）
    m_rdpAuth = new QComboBox(this);
    m_rdpAuth->addItem(tr("NTLM 密码"), QStringLiteral("ntlm"));
    m_rdpAuth->addItem(tr("明文(无 NLA)"), QStringLiteral("plain"));
    m_domain = new QLineEdit(this);
    m_domain->setPlaceholderText(tr("Windows 域，可留空"));
    form->addRow(tr("RDP认证方式："), m_rdpAuth);
    form->addRow(tr("域(可选)："), m_domain);
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // 串口参数（仅 Serial 时可见）。选项集合与串口终端工具栏共用 serialcombo。
    m_serialPort = new QComboBox(this);
    m_serialPort->setEditable(true);   // 允许手输设备路径
    m_serialPort->setInsertPolicy(QComboBox::NoInsert);
    serialcombo::fillPorts(m_serialPort);
    auto *refreshPorts = new QPushButton(tr("刷新"), this);
    m_serialPortRow = new QWidget(this);
    auto *serialPortLay = new QHBoxLayout(m_serialPortRow);
    serialPortLay->setContentsMargins(0, 0, 0, 0);
    serialPortLay->addWidget(m_serialPort, 1);
    serialPortLay->addWidget(refreshPorts);

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
    m_localEcho = new QCheckBox(tr("本地回显（设备不回显输入时勾选）"), this);
    m_rxImplicitCr = new QCheckBox(tr("接收时给孤立的 LF 补 CR（设备发裸 \\n 时勾选）"), this);
    m_rxImplicitCr->setChecked(true);

    form->addRow(tr("串口设备："), m_serialPortRow);
    form->addRow(tr("波特率："),   m_baud);
    form->addRow(tr("数据位："),   m_dataBits);
    form->addRow(tr("校验位："),   m_parity);
    form->addRow(tr("停止位："),   m_stopBits);
    form->addRow(tr("流控："),     m_flow);
    // 标签写明"发送"，与另外两处保持一致：这个下拉框管不到接收方向。
    form->addRow(tr("发送换行符："), m_newline);
    form->addRow(QString(), m_localEcho);
    form->addRow(QString(), m_rxImplicitCr);

    connect(refreshPorts, &QPushButton::clicked,
            this, &AddDeviceDialog::onRefreshPorts);
#endif

    // TCP/Telnet 参数。终端类型/协商/自动登录只在 Telnet 下可见（裸 TCP 没有
    // 这些概念）；换行/回显/接收补 CR 两种模式都可见。
    m_termType = new QComboBox(this);
    netcombo::fillTermTypes(m_termType);
    m_negotiate = new QCheckBox(tr("选项协商（NAWS / 终端类型 / SGA）"), this);
    m_negotiate->setChecked(true);
    m_autoLogin = new QCheckBox(tr("自动登录（匹配 login: / Password: 提示）"), this);
    m_netNewline = new QComboBox(this);
    netcombo::fillNewlineMode(m_netNewline);
    m_netLocalEcho = new QCheckBox(tr("本地回显（对端不回显输入时勾选）"), this);
    m_netRxImplicitCr = new QCheckBox(tr("接收时给孤立的 LF 补 CR（对端发裸 \\n 时勾选）"), this);
    m_netRxImplicitCr->setChecked(true);

    form->addRow(tr("终端类型："), m_termType);
    form->addRow(QString(), m_negotiate);
    form->addRow(QString(), m_autoLogin);
    form->addRow(tr("发送换行符："), m_netNewline);
    form->addRow(QString(), m_netLocalEcho);
    form->addRow(QString(), m_netRxImplicitCr);

    // --- 排版统一 ---
    // 三个 QFormLayout（主表单 + 认证堆叠页里两个）各自独立计算布局，
    // 需统一标签对齐、字段拉伸策略与标签列宽，视觉上才是同一张表。
    // 项目既定风格参考 editors/ServiceConfigWidget.cpp（右对齐标签）。
    for (QFormLayout *f : {form, pwForm, keyForm}) {
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // macOS 默认 FieldsStayAtSizeHint：下拉框/输入框按内容定宽导致
        // 长短不一，改为非固定字段占满第二列
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    const QList<QComboBox *> combos = {
        m_authMethod, m_keyType, m_protocol, m_termType, m_netNewline,
#ifdef CUBESHELL_WITH_RDP
        m_rdpAuth,
#endif
    };
    for (QComboBox *cb : combos)
        cb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // 标签列等宽：取所有标签的最大宽度设为统一最小宽，右对齐后
    // 主表单与堆叠页内表单的标签视觉上成为同一列
    QList<QWidget *> formLabels;
    const auto collectLabel = [&formLabels](QFormLayout *f, QWidget *field) {
        if (QWidget *label = f->labelForField(field))
            formLabels.append(label);
    };
    collectLabel(form, m_protocol);
    collectLabel(form, m_name);
    collectLabel(form, m_username);
    collectLabel(form, m_host);
    collectLabel(form, m_port);
    collectLabel(form, m_authMethod);
    collectLabel(pwForm, m_password);
    collectLabel(keyForm, m_keyType);
    collectLabel(keyForm, keyFileRow);
    collectLabel(form, m_termType);
    collectLabel(form, m_netNewline);
#ifdef CUBESHELL_WITH_RDP
    collectLabel(form, m_rdpAuth);
    collectLabel(form, m_domain);
#endif
#ifdef CUBESHELL_WITH_SERIAL
    collectLabel(form, m_serialPortRow);
    collectLabel(form, m_baud);
    collectLabel(form, m_dataBits);
    collectLabel(form, m_parity);
    collectLabel(form, m_stopBits);
    collectLabel(form, m_flow);
    collectLabel(form, m_newline);
#endif
    int labelWidth = 0;
    for (QWidget *label : formLabels)
        labelWidth = qMax(labelWidth, label->sizeHint().width());
    for (QWidget *label : formLabels)
        label->setMinimumWidth(labelWidth);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddDeviceDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_authMethod, &QComboBox::currentIndexChanged,
            this, &AddDeviceDialog::onAuthMethodChanged);
    connect(m_browseKey, &QPushButton::clicked, this, &AddDeviceDialog::onBrowseKey);
    // 对应Python: protoCombo.currentIndexChanged.connect(self._on_protocol_changed)
    connect(m_protocol, &QComboBox::currentIndexChanged,
            this, &AddDeviceDialog::onProtocolChanged);
    onProtocolChanged(m_protocol->currentIndex());
}

void AddDeviceDialog::setDevice(const DeviceEntry &e)
{
    // 先回填协议再填 host/port，避免 onProtocolChanged 覆盖实际端口。
    // 对应Python: set_protocol（cube-shell.py:6086-6088）+ domain/auth 回填
    // isSsh() 兜住 protocol 为空的旧配置。
    const QString proto = e.isSsh() ? QStringLiteral("ssh") : e.protocol;
    const int protoIdx = m_protocol->findData(proto);
    // 找不到 = 该协议在本次构建里没编进来（如关掉 RDP 的鸿蒙构建打开一条 RDP
    // 配置）。保持 SSH 不动，至少让对话框能打开、名字能改。
    m_protocol->setCurrentIndex(protoIdx < 0 ? 0 : protoIdx);
#ifdef CUBESHELL_WITH_RDP
    m_domain->setText(e.domain);
    const int authIdx = m_rdpAuth->findData(e.auth.isEmpty() ? QStringLiteral("ntlm") : e.auth);
    m_rdpAuth->setCurrentIndex(authIdx < 0 ? 0 : authIdx);
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // DeviceEntry 存的是字符串形态，经 serialSettingsFromDevice 映射回枚举。
    const SerialSettings ss = serialSettingsFromDevice(e);
    serialcombo::fillPorts(m_serialPort, ss.portName);
    m_baud->setCurrentText(QString::number(ss.baudRate));
    serialcombo::selectData(m_dataBits, int(ss.dataBits));
    serialcombo::selectData(m_parity,   int(ss.parity));
    serialcombo::selectData(m_stopBits, int(ss.stopBits));
    serialcombo::selectData(m_flow,     int(ss.flowControl));
    serialcombo::selectData(m_newline,  int(ss.newlineMode));
    m_localEcho->setChecked(ss.localEcho);
    m_rxImplicitCr->setChecked(ss.rxImplicitCr);
#endif
    // TCP/Telnet 同理，映射走 netSettingsFromDevice。
    const TcpSettings ns = netSettingsFromDevice(e);
    netcombo::fillTermTypes(m_termType, ns.termType);
    m_negotiate->setChecked(ns.negotiate);
    m_autoLogin->setChecked(ns.autoLogin);
    netcombo::selectData(m_netNewline, int(ns.newlineMode));
    m_netLocalEcho->setChecked(ns.localEcho);
    m_netRxImplicitCr->setChecked(ns.rxImplicitCr);

    m_name->setText(e.name);
    m_username->setText(e.username);
    const HostPort hp = e.hostPort();
    m_host->setText(hp.host);
    m_port->setText(QString::number(hp.port));
    // 回填过实际端口后，端口框里就不再是"某协议的默认值"了——切协议时
    // 不该把用户存的端口冲掉。
    m_portDefaultFor.clear();
    if (e.usesKey()) {
        m_authMethod->setCurrentIndex(1);
        m_keyType->setCurrentText(e.keyType);
        m_keyFile->setText(e.keyFile);
    } else {
        m_authMethod->setCurrentIndex(0);
        m_password->setText(e.password);
    }
    onAuthMethodChanged(m_authMethod->currentIndex());
}

DeviceEntry AddDeviceDialog::device() const
{
    DeviceEntry e;
    e.name = m_name->text().trimmed();
#ifdef CUBESHELL_WITH_SERIAL
    if (serialSelected()) {
        // 串口没有 host/username/凭据，只写协议 + 串口参数，其余留空。
        e.protocol = QStringLiteral("serial");
        e.portName    = serialcombo::portNameOf(m_serialPort);
        e.baudRate    = serialcombo::baudRateOf(m_baud);
        e.dataBits    = int(serialcombo::dataBitsOf(m_dataBits));
        e.parity      = serialcombo::parityToString(serialcombo::parityOf(m_parity));
        e.stopBits    = serialcombo::stopBitsToString(serialcombo::stopBitsOf(m_stopBits));
        e.flowControl = serialcombo::flowControlToString(serialcombo::flowControlOf(m_flow));
        e.newlineMode = serialcombo::newlineModeToString(serialcombo::newlineModeOf(m_newline));
        e.localEcho   = m_localEcho->isChecked();
        e.rxImplicitCr = m_rxImplicitCr->isChecked();
        return e;
    }
#endif
    e.username = m_username->text().trimmed();
    const QString host = m_host->text().trimmed();
    const quint16 port = quint16(m_port->text().trimmed().toUShort());
    // Store host in the pickle-compatible "host:port" string form.
    e.host = formatHostPort(host, port);
    e.port = port;

    if (telnetSelected() || tcpSelected()) {
        e.protocol = selectedProtocol();
        e.newlineMode = netcombo::newlineModeToString(
            netcombo::newlineModeOf(m_netNewline));
        e.localEcho    = m_netLocalEcho->isChecked();
        e.rxImplicitCr = m_netRxImplicitCr->isChecked();
        if (telnetSelected()) {
            e.telnetNegotiate = m_negotiate->isChecked();
            e.termType = QString::fromUtf8(netcombo::termTypeOf(m_termType));
            e.autoLogin = m_autoLogin->isChecked();
            e.password = m_password->text();
        } else {
            // 裸 TCP 没有登录概念：用户名/密码不落盘，避免误导。
            e.username.clear();
        }
        e.keyType.clear();
        e.keyFile.clear();
        return e;
    }

#ifdef CUBESHELL_WITH_RDP
    if (rdpSelected()) {
        // 对应Python: addDev 的 RDP 保存分支（cube-shell.py:6108-6119）
        e.protocol = QStringLiteral("rdp");
        e.domain = m_domain->text().trimmed();
        const QString auth = m_rdpAuth->currentData().toString();
        e.auth = auth.isEmpty() ? QStringLiteral("ntlm") : auth;
        e.password = m_password->text();
        e.keyType.clear();
        e.keyFile.clear();
        return e;
    }
#endif
    if (m_authMethod->currentIndex() == 1) {
        e.keyType = m_keyType->currentText();
        e.keyFile = m_keyFile->text().trimmed();
        e.password.clear();
    } else {
        e.password = m_password->text();
        e.keyType.clear();
        e.keyFile.clear();
    }
    return e;
}

void AddDeviceDialog::onAuthMethodChanged(int index)
{
    m_authStack->setCurrentIndex(index);
}

QString AddDeviceDialog::selectedProtocol() const
{
    if (!m_protocol)
        return QStringLiteral("ssh");
    const QString v = m_protocol->currentData().toString();
    return v.isEmpty() ? QStringLiteral("ssh") : v;
}

// 这四个判定无条件编译：某协议没编进来时下拉框里就没有对应项，
// currentData() 永远不会等于它，函数自然恒为 false。
bool AddDeviceDialog::rdpSelected() const
{
    return selectedProtocol() == QLatin1String("rdp");
}

bool AddDeviceDialog::serialSelected() const
{
    return selectedProtocol() == QLatin1String("serial");
}

bool AddDeviceDialog::telnetSelected() const
{
    return selectedProtocol() == QLatin1String("telnet");
}

bool AddDeviceDialog::tcpSelected() const
{
    return selectedProtocol() == QLatin1String("tcp");
}

#ifdef CUBESHELL_WITH_SERIAL

void AddDeviceDialog::onRefreshPorts()
{
    serialcombo::fillPorts(m_serialPort);
}

#endif // CUBESHELL_WITH_SERIAL

// 条件显隐 + 默认端口切换。对应Python: _on_protocol_changed（cube-shell.py:6073-6084）
// 五态版本：SSH / RDP / Serial / Telnet / TCP 各自显示自己的字段。
// 用一个 proto 字符串做比较而不是五个 bool——后者读起来像布尔代数题。
void AddDeviceDialog::onProtocolChanged(int /*index*/)
{
    const QString proto = selectedProtocol();
    const bool isRdp    = (proto == QLatin1String("rdp"));
    const bool isSerial = (proto == QLatin1String("serial"));
    const bool isTelnet = (proto == QLatin1String("telnet"));
    const bool isTcp    = (proto == QLatin1String("tcp"));
    const bool isNet    = isTelnet || isTcp;
    const bool isSsh    = !isRdp && !isSerial && !isNet;

    // 串口没有网络语义：IP/端口整组隐藏。用户名对裸 TCP 也没有意义
    //（对端不一定有登录概念），Telnet 则用它做自动登录。
    m_form->setRowVisible(m_username, isSsh || isRdp || isTelnet);
    m_form->setRowVisible(m_host, !isSerial);
    m_form->setRowVisible(m_port, !isSerial);

    // "密码 / 私钥"二选一只有 SSH 需要：RDP 只支持密码，Telnet 的密码是给
    // 自动登录用的，串口和裸 TCP 没有认证。认证方式行仅 SSH 可见；
    // 密码页在 SSH/RDP/Telnet 下可见（后两者固定停在密码页）。
    m_form->setRowVisible(m_authMethod, isSsh);
    m_form->setRowVisible(m_authStack, isSsh || isRdp || isTelnet);
    if (!isSsh)
        m_authMethod->setCurrentIndex(0);
    m_authStack->setCurrentIndex(isSsh ? m_authMethod->currentIndex() : 0);

#ifdef CUBESHELL_WITH_RDP
    m_form->setRowVisible(m_rdpAuth, isRdp);
    m_form->setRowVisible(m_domain, isRdp);
#endif
#ifdef CUBESHELL_WITH_SERIAL
    m_form->setRowVisible(m_serialPortRow, isSerial);
    m_form->setRowVisible(m_baud, isSerial);
    m_form->setRowVisible(m_dataBits, isSerial);
    m_form->setRowVisible(m_parity, isSerial);
    m_form->setRowVisible(m_stopBits, isSerial);
    m_form->setRowVisible(m_flow, isSerial);
    m_form->setRowVisible(m_newline, isSerial);
    m_form->setRowVisible(m_localEcho, isSerial);
    m_form->setRowVisible(m_rxImplicitCr, isSerial);
#endif
    m_form->setRowVisible(m_termType, isTelnet);
    m_form->setRowVisible(m_negotiate, isTelnet);
    m_form->setRowVisible(m_autoLogin, isTelnet);
    m_form->setRowVisible(m_netNewline, isNet);
    m_form->setRowVisible(m_netLocalEcho, isNet);
    m_form->setRowVisible(m_netRxImplicitCr, isNet);

    // 切协议时给出合理的默认端口（串口不用端口字段，跳过）。
    // 只在端口框为空、或里面放的还是上一个协议的默认值时才改——用户自己
    // 填过端口就一直保留。默认值收敛在 defaultPortFor()。
    if (!isSerial) {
        const QString cur = m_port->text().trimmed();
        const bool untouched =
            cur.isEmpty()
            || (!m_portDefaultFor.isEmpty()
                && cur == QString::number(defaultPortFor(m_portDefaultFor)));
        if (untouched) {
            m_port->setText(QString::number(defaultPortFor(proto)));
            m_portDefaultFor = proto;
        }
    }

    // 行显隐后把对话框收回到内容高度。
    // QFormLayout 隐藏行会让 sizeHint 变小，但 QDialog 不会自动跟着缩小
    //（窗口尺寸一旦被撑大就保持不变），多出来的高度会在表单和按钮之间
    // 显示成一块空白，且每次切到字段更多的协议再切回来就多留一截。
    // invalidate() 丢弃各层缓存的 sizeHint，activate() 立即重算。
    m_form->invalidate();
    if (QLayout *top = layout()) {
        top->invalidate();
        top->activate();
    }
    // 只收高度、保留当前宽度：adjustSize() 会把宽度一并打回 sizeHint，
    // 用户手动拉宽过对话框的话每次切协议都被打回去，很难用。
    resize(width(), sizeHint().height());
}

void AddDeviceDialog::onBrowseKey()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择私钥文件"), QString(), tr("所有文件 (*)"));
    if (!path.isEmpty())
        m_keyFile->setText(path);
}

bool AddDeviceDialog::validate(QString *err) const
{
    if (m_name->text().trimmed().isEmpty()) { *err = tr("配置名不能为空。"); return false; }
#ifdef CUBESHELL_WITH_SERIAL
    if (serialSelected()) {
        // 串口没有用户名/IP/凭据，只需要设备名。
        const QString portName = serialcombo::portNameOf(m_serialPort);
        if (portName.isEmpty()) { *err = tr("请选择或输入串口设备。"); return false; }
        return true;
    }
#endif
    if (tcpSelected()) {
        // 裸 TCP 只要能定位到对端就行，没有用户名/凭据的概念。
        if (m_host->text().trimmed().isEmpty()) { *err = tr("IP地址不能为空。"); return false; }
        return true;
    }
    if (telnetSelected()) {
        if (m_host->text().trimmed().isEmpty()) { *err = tr("IP地址不能为空。"); return false; }
        // 用户名平时可留空（很多设备只问密码，或直接给 shell），
        // 但勾了自动登录就必须有——状态机拿它去应答 login: 提示。
        if (m_autoLogin->isChecked() && m_username->text().trimmed().isEmpty()) {
            *err = tr("启用自动登录时必须填写用户名。");
            return false;
        }
        return true;
    }
    if (m_username->text().trimmed().isEmpty()) { *err = tr("用户名不能为空。"); return false; }
    if (m_host->text().trimmed().isEmpty()) { *err = tr("IP地址不能为空。"); return false; }
#ifdef CUBESHELL_WITH_RDP
    if (rdpSelected()) {
        // 对应Python: 'RDP 连接需要提供密码！'（cube-shell.py:6109-6111）
        if (m_password->text().isEmpty()) { *err = tr("RDP 连接需要提供密码。"); return false; }
        return true;
    }
#endif
    if (m_authMethod->currentIndex() == 1) {
        if (m_keyFile->text().trimmed().isEmpty()) { *err = tr("请选择私钥文件。"); return false; }
    } else {
        if (m_password->text().isEmpty()) { *err = tr("请输入密码（或切换为私钥登录）。"); return false; }
    }
    return true;
}

void AddDeviceDialog::accept()
{
    QString err;
    if (!validate(&err)) {
        QMessageBox::warning(this, tr("配置不完整"), err);
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
