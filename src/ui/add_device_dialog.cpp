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
#include "ConnectionTester.h"             // 测试连接后台探测
#ifdef CUBESHELL_WITH_SERIAL
#include "dialogs/SerialConnectDialog.h"   // serialcombo 辅助函数
#endif

namespace cubeshell {

// 让 QStackedWidget 只按**当前页**要高度。
//
// QStackedLayout::sizeHint() 取的是所有页的最大值，与正在显示哪一页无关。
// 认证栈的私钥页有两行（私钥类型 + 私钥文件）、密码页只有一行，于是停在密码页
// 时密码框下面会空出整一行（实测 33px）——SSH 的「密码登录」，以及固定停在密码
// 页的 RDP / Telnet，三者都吃这个亏。
//
// QWidgetItem::sizeHint() 对 Ignored 的方向返回 0，所以把非当前页的**纵向**策略
// 设成 Ignored，就能把它们从那个 max 里摘掉。横向刻意不动：横向也 Ignored 的话，
// 最宽的那页（私钥文件行还带个「浏览…」按钮）不再参与宽度计算，切到私钥页时
// 对话框会突然变宽。
static void fitStackToCurrentPage(QStackedWidget *stack)
{
    if (!stack)
        return;
    const QWidget *cur = stack->currentWidget();
    for (int i = 0; i < stack->count(); ++i) {
        QWidget *page = stack->widget(i);
        if (!page)
            continue;
        page->setSizePolicy(QSizePolicy::Preferred,
                            page == cur ? QSizePolicy::Preferred
                                        : QSizePolicy::Ignored);
    }
    // 必须显式通知栈控件自己"我的尺寸建议变了"。
    //
    // 上面改的是**页**的策略，而 QStackedWidget 把非当前页设成了 hidden，
    // QWidgetPrivate::updateGeometry_helper 对隐藏控件不会向上层布局传播失效。
    // 于是外层 QFormLayout 里缓存栈控件尺寸建议的那个 QWidgetItemV2 不会被清，
    // 本函数返回后 dialog->sizeHint() 还是**换页前**的高度（实测：切到私钥页读到
    // 397、切回密码页读到 430，正好差一行），refitHeight() 按这个陈旧值 resize
    // 就等于没收。加这一句才让缓存同步失效——单靠再多调几次 invalidate()/
    // activate() 是没用的，那个缓存只认 updateGeometry()。
    stack->updateGeometry();
}

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
    // 占位符随协议变（见 onProtocolChanged）；这里只给个通用初值。
    m_password->setPlaceholderText(tr("留空则在连接时输入"));
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

    // 代理（仅 SSH 可见）。放在认证之后、协议专属字段之前：它属于"怎么连到
    // 这台机器"，而下面那些是各协议自己的参数。
    //
    // 无标签的整行（同 m_authStack）而不是 addRow("代理：", m_proxy)：控件
    // 内部第一行就是"代理类型："，塞进字段列的话它会被外层标签宽度再缩进一次。
    m_proxy = new ProxySettingsWidget(this);
    form->addRow(m_proxy);
    // 类型切换会改变行数，跟着重算对话框高度。
    connect(m_proxy, &ProxySettingsWidget::typeChanged, this,
            &AddDeviceDialog::refitHeight);
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
    formLabels += m_proxy->formLabels();   // 代理控件内部那张表也纳入同一列
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
    m_buttonBox = buttons;
    connect(buttons, &QDialogButtonBox::accepted, this, &AddDeviceDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // --- 测试连接（左对齐的 ActionRole 按钮 + 行内结果条） ---
    // ActionRole 不触发 accept/reject，点它不会关对话框。结果走行内 label
    //（绿✓/红✗）而不是弹窗：探测是轻量反馈，不该打断填表。
    m_tester = new ConnectionTester(this);
    m_testButton = new QPushButton(tr("测试连接"), this);
    buttons->addButton(m_testButton, QDialogButtonBox::ActionRole);
    m_testStatus = new QLabel(this);
    m_testStatus->setWordWrap(true);
    m_testStatus->setVisible(false);   // 只在有结果/进行中时占位

    connect(m_testButton, &QPushButton::clicked, this, &AddDeviceDialog::onTestConnection);
    connect(m_tester, &ConnectionTester::finished, this, &AddDeviceDialog::onTestFinished);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_testStatus);
    layout->addWidget(buttons);

    connect(m_authMethod, &QComboBox::currentIndexChanged,
            this, &AddDeviceDialog::onAuthMethodChanged);
    // textEdited 而非 textChanged：只有用户敲键盘才算「动过密码」，
    // setDevice 的程序性回填不算。这个区分决定了保存时要不要覆盖已存密码。
    connect(m_password, &QLineEdit::textEdited,
            this, [this]() { m_passwordEdited = true; });
    connect(m_browseKey, &QPushButton::clicked, this, &AddDeviceDialog::onBrowseKey);
    // 对应Python: protoCombo.currentIndexChanged.connect(self._on_protocol_changed)
    connect(m_protocol, &QComboBox::currentIndexChanged,
            this, &AddDeviceDialog::onProtocolChanged);
    onProtocolChanged(m_protocol->currentIndex());
}

void AddDeviceDialog::setDevice(const DeviceEntry &e)
{
    m_id = e.id;   // 编辑既有条目：id 必须原样带回，它是钥匙串的索引
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
    // 代理。setProxyDeviceCatalog 由调用方在 setDevice 前后任意时机调用都行
    //（控件内部两个函数都会拿存着的目录重建下拉）。
    m_proxy->setConfig(e.proxy);
    // 回填不算用户编辑——setText 会触发 textEdited 之外的信号，但我们连的是
    // textEdited（仅用户输入才发），这里再清一次是为了防止将来改用 textChanged。
    m_passwordEdited = false;
}

void AddDeviceDialog::setHasStoredProxyPassword(bool has)
{
    m_proxy->setHasStoredPassword(has);
}

bool AddDeviceDialog::proxyPasswordEdited() const
{
    return m_proxy->passwordEdited();
}

void AddDeviceDialog::setProxyDeviceCatalog(const QList<ProxyDeviceItem> &devices,
                                            const QString &excludeId)
{
    m_proxy->setDeviceCatalog(devices, excludeId);
}

void AddDeviceDialog::setProxyPasswordResolver(std::function<QString(const QString &)> resolver)
{
    m_proxyPasswordResolver = std::move(resolver);
}

void AddDeviceDialog::setJumpHostPublisher(std::function<void(const QStringList &)> publisher)
{
    m_jumpHostPublisher = std::move(publisher);
}

void AddDeviceDialog::setHasStoredPassword(bool has)
{
    m_hasStoredPassword = has;
    if (has && m_password->text().isEmpty())
        m_password->setPlaceholderText(tr("已保存，留空则不修改"));
}

void AddDeviceDialog::setPasswordResolver(std::function<QString(const QString &)> resolver)
{
    m_passwordResolver = std::move(resolver);
}

void AddDeviceDialog::onTestConnection()
{
    // 探测中再点 = 取消。cancel() 不发 finished()，这里自己收尾 UI。
    if (m_tester->isRunning()) {
        m_tester->cancel();
        setTestingUi(false);
        m_testStatus->setText(tr("已取消测试"));
        m_testStatus->setStyleSheet(QString());
        m_testStatus->setVisible(true);
        return;
    }

    // 先过与「保存」同一套必填校验：主机为空之类的低级错误不必真发连接。
    QString err;
    if (!validate(&err)) {
        QMessageBox::warning(this, tr("配置不完整"), err);
        return;
    }

    DeviceEntry e = device();
    // 编辑既有设备且密码框留空时，device() 带出的是空密码（语义是"没改"），
    // 得拿钥匙串里的真实密码去测，否则一改端口再测就误报认证失败。
    if (e.password.isEmpty() && !e.usesKey() && m_passwordResolver)
        e.password = m_passwordResolver(e.id);
    // 代理口令同理，语义逐字对应。不补的话"代理需要认证"的设备一测就红，
    // 而真正连接时明明是通的——这个按钮给出与实际相反的结论比没有它更糟。
    if (e.proxy.password.isEmpty() && m_proxyPasswordResolver)
        e.proxy.password = m_proxyPasswordResolver(e.id);

    // 密码非必填（见 validate）：SSH 密码登录却一个密码都没有时别真去连——
    // 「测试连接」不做交互式应答（promptCb 传 nullptr），空密码只会撞回
    // 「认证失败：用户名或密码错误」，把"压根没填密码"说成"密码错了"。
    // 就地填一个即可测；留空保存也行，连接时终端里会问（见 TerminalPrompt）。
    if (e.isSsh() && !e.usesKey() && e.password.isEmpty()) {
        onTestFinished(false, tr("未填写密码，无法测试；可先填一个用于测试，"
                                 "或留空保存、连接时在终端中输入。"));
        return;
    }

    setTestingUi(true);
    m_testStatus->setText(tr("正在测试连接…"));
    m_testStatus->setStyleSheet(QString());
    m_testStatus->setVisible(true);

    // 把这一刻选中的跳板机推给建连路径。必须在起测之**前**：建链跑在工作线程上，
    // 按 id 查的是 GlobalState 里那份快照，而快照只认已保存的设备，对话框里刚
    // 选好还没保存的这一跳不在其中（详见 setJumpHostPublisher 的说明）。
    // 只在类型确实是「跳转服务器」时推：别的类型下 hopIds 可能是切类型留下的
    // 残留，照着它去解析凭据等于白读一次钥匙串。
    if (e.proxy.type == ProxyType::JumpHost && m_jumpHostPublisher)
        m_jumpHostPublisher(e.proxy.hopIds);

    bool started;
#ifdef CUBESHELL_WITH_SERIAL
    if (e.isSerial()) {
        started = m_tester->testSerial(serialSettingsFromDevice(e));
    } else
#endif
    if (e.isSsh()) {
        started = m_tester->testSsh(e);
    } else {
        // telnet / tcp / rdp：只做 host:port 的可达性探测。
        started = m_tester->testTcp(e);
    }
    // 串口是同步 open，finished 可能已在本调用内回报完（onTestFinished 已把
    // UI 复位）；started=false 是理论兜底（m_running 闸门），同样复位。
    if (!started)
        setTestingUi(false);
}

void AddDeviceDialog::onTestFinished(bool ok, const QString &message)
{
    setTestingUi(false);
    m_testStatus->setText((ok ? tr("✓ ") : tr("✗ ")) + message);
    m_testStatus->setStyleSheet(ok ? QStringLiteral("color: #1a7f37;")
                                   : QStringLiteral("color: #cf222e;"));
    m_testStatus->setToolTip(message);
    m_testStatus->setVisible(true);
}

void AddDeviceDialog::setTestingUi(bool testing)
{
    m_testButton->setText(testing ? tr("取消") : tr("测试连接"));
    // 探测期间禁用 OK（接受一个还没验过的配置容易误存），但保留 Cancel：
    // 用户随时能关对话框，m_tester 随对话框析构安全收尾（worker detach）。
    if (QPushButton *ok = m_buttonBox->button(QDialogButtonBox::Ok))
        ok->setEnabled(!testing);
}

DeviceEntry AddDeviceDialog::device() const
{
    DeviceEntry e;
    // id 必须在这里赋 —— 本函数下面有 4 个提前 return（串口 / telnet+tcp /
    // RDP / SSH-密钥），放到末尾会让 5 个协议里的 3 个拿到空 id，
    // 密码就存不进钥匙串了。
    e.id = m_id.isEmpty() ? DeviceConfigStore::newDeviceId() : m_id;
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
    // 代理只对 SSH 取值：上面 4 个提前 return（串口 / telnet+tcp / RDP）都不
    // 经过这里，于是那些协议的条目 proxy.type 恒为 None——与本轮"只接线 SSH"
    // 的范围一致（见 SshClient::setProxyConfig），也不会在 devices.json 里
    // 留下一份永远不会被读的代理配置。
    e.proxy = m_proxy->config();
    return e;
}

void AddDeviceDialog::onAuthMethodChanged(int index)
{
    m_authStack->setCurrentIndex(index);
    // 密码页一行、私钥页两行，换页后高度真的变了，得重新收一次——
    // 不收的话，私钥页切回密码页会在密码框下面留出那一行空白。
    fitStackToCurrentPage(m_authStack);
    refitHeight();
}

void AddDeviceDialog::refitHeight()
{
    // invalidate() 丢弃各层缓存的 sizeHint，activate() 立即重算。
    m_form->invalidate();
    if (QLayout *top = layout()) {
        top->invalidate();
        top->activate();
    }
    resize(width(), sizeHint().height());
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
    // 代理只有 SSH 需要：本轮只给 SSH 接线（Telnet/TCP 走 TcpClient，是另一条
    // 建连路径）。给 RDP/串口显示一个连接期不会被读的代理配置，比不显示更糟。
    m_form->setRowVisible(m_proxy, isSsh);
    if (!isSsh)
        m_authMethod->setCurrentIndex(0);
    m_authStack->setCurrentIndex(isSsh ? m_authMethod->currentIndex() : 0);
    fitStackToCurrentPage(m_authStack);

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

    // 密码框的占位符：密码非必填，得告诉用户"留空之后在哪儿输"——
    // 三个协议的答案不一样（SSH 在终端里、RDP 在面板里、Telnet 在提示符处）。
    // 已存密码的设备不动它：那里的"留空则不修改"是更要紧的提示
    //（见 setHasStoredPassword）。
    if (!m_hasStoredPassword) {
        if (isRdp)
            m_password->setPlaceholderText(tr("留空则在连接面板中输入"));
        else if (isTelnet)
            m_password->setPlaceholderText(tr("留空则在终端里自己输入"));
        else
            m_password->setPlaceholderText(tr("留空则在连接时于终端输入"));
    }

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

    // 行显隐后把对话框收回到内容高度（见 refitHeight 的说明）。
    refitHeight();
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
        // 密码不再必填：留空就等连接时在 RDP 面板里现填（见 RdpPanel::promptForPassword）。
        // Python 版这里会拦下（'RDP 连接需要提供密码！'，cube-shell.py:6109-6111），
        // C++ 侧不强制把密码落进钥匙串。
        return true;
    }
#endif
    if (m_authMethod->currentIndex() == 1) {
        if (m_keyFile->text().trimmed().isEmpty()) { *err = tr("请选择私钥文件。"); return false; }
    }
    // 代理（只有 SSH 会走到这里）。填了一半的代理配置比没填更糟：连接期只会
    // 得到一句"连接超时"，看不出是代理地址空着。
    if (!m_proxy->validate(err))
        return false;
    // 密码登录：密码可留空——连接时在终端里就地输入（见 TerminalPrompt），
    // 只在本次会话内有效、不写钥匙串，比强制预存更安全。
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
