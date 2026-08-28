#include "ProxySettingsWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace cubeshell {

namespace {
// 堆叠页的序号。用具名常量而不是字面量：HTTP 与 SOCKS5 共用同一页，
// 写成 setCurrentIndex(1) 的话这个"共用"事实在代码里就看不见了。
constexpr int kPageNoParam  = 0;   // 直连 / 全局 / 系统
constexpr int kPageHostPort = 1;   // HTTP / SOCKS5
constexpr int kPageCommand  = 2;   // 代理命令
constexpr int kPageJump     = 3;   // 跳转服务器

// 下拉里"还没选"的那一项：data 为空串，validate() 靠它认出没填完的行。
const char kUnsetHopData[] = "";
} // namespace

ProxySettingsWidget::ProxySettingsWidget(QWidget *parent)
    : QWidget(parent)
{
    m_type = new QComboBox(this);
    // 顺序照 WindTerm 的下拉：不使用 → 全局 → 系统 → HTTP → SOCKS5 → 命令 → 跳转。
    // 显示文本可翻译，userData 是判定依据（proxyTypeToString 的产物，
    // 与 JSON 里存的字符串同一套）。
    m_type->addItem(tr("不使用代理"),   proxyTypeToString(ProxyType::None));
    m_type->addItem(tr("全局代理"),     proxyTypeToString(ProxyType::Global));
    m_type->addItem(tr("系统代理"),     proxyTypeToString(ProxyType::System));
    m_type->addItem(tr("HTTP 代理"),    proxyTypeToString(ProxyType::Http));
    m_type->addItem(tr("SOCKS v5 代理"), proxyTypeToString(ProxyType::Socks5));
#ifdef CUBESHELL_WITH_LOCALPROC
    // 代理命令要起本地子进程（nc / corkscrew 之类），鸿蒙沙箱禁 exec，
    // 故与 frp / docker CLI 同一个门控。关掉时下拉里就不出现这一项，
    // 处理方式与 RDP/串口行一致。
    m_type->addItem(tr("代理命令"),     proxyTypeToString(ProxyType::Command));
#endif
    m_type->addItem(tr("跳转服务器"),   proxyTypeToString(ProxyType::JumpHost));

    m_stack = new QStackedWidget(this);

    // --- page 0: 无参数 ---
    auto *noParamPage = new QWidget(this);
    auto *noParamLay = new QVBoxLayout(noParamPage);
    noParamLay->setContentsMargins(0, 0, 0, 0);
    m_noParamHint = new QLabel(noParamPage);
    m_noParamHint->setWordWrap(true);
    m_noParamHint->setStyleSheet(QStringLiteral("color: gray;"));
    noParamLay->addWidget(m_noParamHint);

    // --- page 1: HTTP / SOCKS5（字段完全相同，共用一页）---
    auto *hostPage = new QWidget(this);
    m_hostForm = new QFormLayout(hostPage);
    m_hostForm->setContentsMargins(0, 0, 0, 0);
    m_host = new QLineEdit(hostPage);
    m_host->setPlaceholderText(tr("代理服务器地址"));
    m_port = new QLineEdit(hostPage);
    m_port->setPlaceholderText(tr("端口"));
    m_username = new QLineEdit(hostPage);
    m_username->setPlaceholderText(tr("代理用户名，不需要认证则留空"));
    m_password = new QLineEdit(hostPage);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("代理口令，不需要认证则留空"));
    m_hostForm->addRow(tr("代理地址："), m_host);
    m_hostForm->addRow(tr("代理端口："), m_port);
    m_hostForm->addRow(tr("代理用户名："), m_username);
    m_hostForm->addRow(tr("代理口令："), m_password);

    // --- page 2: 代理命令 ---
    auto *cmdPage = new QWidget(this);
    m_commandForm = new QFormLayout(cmdPage);
    m_commandForm->setContentsMargins(0, 0, 0, 0);
    m_command = new QLineEdit(cmdPage);
    m_command->setPlaceholderText(QStringLiteral("nc -X 5 -x 127.0.0.1:1080 %h %p"));
    m_commandForm->addRow(tr("代理命令："), m_command);
    auto *cmdHint = new QLabel(
        tr("命令的标准输入/输出即为到目标主机的字节流（同 OpenSSH 的 ProxyCommand）。\n"
           "占位符：%h = 目标主机，%p = 目标端口，%r = 登录用户名，%% = 百分号。"),
        cmdPage);
    cmdHint->setWordWrap(true);
    cmdHint->setStyleSheet(QStringLiteral("color: gray;"));
    m_commandForm->addRow(QString(), cmdHint);

    // --- page 3: 跳转服务器 ---
    auto *jumpPage = new QWidget(this);
    auto *jumpLay = new QVBoxLayout(jumpPage);
    jumpLay->setContentsMargins(0, 0, 0, 0);
    m_hopLayout = new QVBoxLayout;
    m_hopLayout->setContentsMargins(0, 0, 0, 0);
    jumpLay->addLayout(m_hopLayout);
    m_addHop = new QPushButton(tr("添加跳板机"), jumpPage);
    auto *addRow = new QWidget(jumpPage);
    auto *addRowLay = new QHBoxLayout(addRow);
    addRowLay->setContentsMargins(0, 0, 0, 0);
    addRowLay->addWidget(m_addHop);
    addRowLay->addStretch(1);
    jumpLay->addWidget(addRow);
    m_hopHint = new QLabel(jumpPage);
    m_hopHint->setWordWrap(true);
    m_hopHint->setStyleSheet(QStringLiteral("color: gray;"));
    jumpLay->addWidget(m_hopHint);

    m_stack->addWidget(noParamPage);   // kPageNoParam
    m_stack->addWidget(hostPage);      // kPageHostPort
    m_stack->addWidget(cmdPage);       // kPageCommand
    m_stack->addWidget(jumpPage);      // kPageJump

    m_form = new QFormLayout(this);
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->addRow(tr("代理类型："), m_type);
    m_form->addRow(m_stack);

    // 排版与宿主对齐：标签右对齐、非固定字段占满第二列。
    // macOS 默认 FieldsStayAtSizeHint 会让输入框按内容定宽，长短不一。
    for (QFormLayout *f : {m_form, m_hostForm, m_commandForm}) {
        f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    m_type->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(m_type, &QComboBox::currentIndexChanged, this,
            [this]() { onTypeChanged(); });
    // textEdited 而非 textChanged：只有用户敲键盘才算「动过口令」，
    // setConfig 的程序性回填不算。这个区分决定保存时要不要覆盖已存口令。
    connect(m_password, &QLineEdit::textEdited, this,
            [this]() { m_passwordEdited = true; });
    connect(m_addHop, &QPushButton::clicked, this,
            [this]() { addHopRow(QString()); });

    onTypeChanged();
}

void ProxySettingsWidget::setGlobalOptionEnabled(bool on)
{
    const QString data = proxyTypeToString(ProxyType::Global);
    const int idx = m_type->findData(data);
    if (on == (idx >= 0))
        return;
    if (on) {
        m_type->insertItem(1, tr("全局代理"), data);   // 位置同构造时：紧跟"不使用"
    } else {
        // 正选着「全局代理」时先退回直连，否则移除后 currentIndex 会落到
        // 相邻项上，用户看到的类型被悄悄换成了别的。
        if (m_type->currentIndex() == idx)
            m_type->setCurrentIndex(0);
        m_type->removeItem(idx);
    }
}

void ProxySettingsWidget::setDeviceCatalog(const QList<ProxyDeviceItem> &devices,
                                           const QString &excludeId)
{
    m_catalog = devices;
    m_excludeId = excludeId;
    // 已有的行按当前选中值重填，选中项尽量保住。
    for (const HopRow &hop : m_hops)
        fillHopCombo(hop.combo, hop.combo->currentData().toString());
    onTypeChanged();   // 目录空/非空会改变提示文案
}

ProxyType ProxySettingsWidget::selectedType() const
{
    return proxyTypeFromString(m_type->currentData().toString());
}

ProxyConfig ProxySettingsWidget::config() const
{
    ProxyConfig cfg;
    cfg.type = selectedType();
    switch (cfg.type) {
    case ProxyType::Http:
    case ProxyType::Socks5:
        cfg.host = m_host->text().trimmed();
        cfg.port = quint16(m_port->text().trimmed().toUShort());
        cfg.username = m_username->text().trimmed();
        cfg.password = m_password->text();
        break;
    case ProxyType::Command:
        cfg.command = m_command->text().trimmed();
        break;
    case ProxyType::JumpHost:
        for (const HopRow &hop : m_hops) {
            const QString id = hop.combo->currentData().toString();
            if (!id.isEmpty())
                cfg.hopIds.append(id);
        }
        break;
    case ProxyType::None:
    case ProxyType::Global:
    case ProxyType::System:
        break;   // 无参数
    }
    // 刻意**不**把其它页的字段一并带出：只保存当前类型用得上的那些。
    // 否则用户试了一遍 HTTP 又改回直连，devices.json 里会留下一份看不见
    // 却仍会被导出/被"改回 HTTP"时复活的陈旧代理地址。
    return cfg;
}

void ProxySettingsWidget::setConfig(const ProxyConfig &cfg)
{
    const QString data = proxyTypeToString(cfg.type);
    int idx = m_type->findData(data);
    if (idx < 0) {
        // 这个类型在本次构建里没编进来（如关掉 LOCALPROC 的鸿蒙构建打开一条
        // 配了「代理命令」的设备）。现加一项而不是回落到直连：回落的话用户
        // 一按确定就把配置抹成直连了，而他压根没打算改代理。
        QString label;
        switch (cfg.type) {
        case ProxyType::Command: label = tr("代理命令"); break;
        default:                 label = proxyTypeToString(cfg.type); break;
        }
        m_type->addItem(tr("%1（本构建不支持）").arg(label), data);
        idx = m_type->count() - 1;
    }
    m_type->setCurrentIndex(idx);

    m_host->setText(cfg.host);
    m_port->setText(cfg.port > 0 ? QString::number(cfg.port) : QString());
    m_username->setText(cfg.username);
    m_password->setText(cfg.password);
    m_command->setText(cfg.command);
    // 回填过实际端口后，端口框里就不再是"某类型的默认值"了——切类型时
    // 不该把用户存的端口冲掉。
    m_portDefaultFor = ProxyType::None;

    while (!m_hops.isEmpty())
        removeHopRow(m_hops.first().row);
    for (const QString &id : cfg.hopIds)
        addHopRow(id);

    // 回填不算用户编辑（连的是 textEdited，不会被 setText 触发；
    // 这里再清一次是为了防止将来改用 textChanged）。
    m_passwordEdited = false;
    onTypeChanged();
}

void ProxySettingsWidget::setHasStoredPassword(bool has)
{
    m_hasStoredPassword = has;
    if (has && m_password->text().isEmpty())
        m_password->setPlaceholderText(tr("已保存，留空则不修改"));
}

QList<QWidget *> ProxySettingsWidget::formLabels() const
{
    QList<QWidget *> labels;
    const auto collect = [&labels](QFormLayout *f, QWidget *field) {
        if (QWidget *l = f->labelForField(field))
            labels.append(l);
    };
    collect(m_form, m_type);
    collect(m_hostForm, m_host);
    collect(m_hostForm, m_port);
    collect(m_hostForm, m_username);
    collect(m_hostForm, m_password);
    collect(m_commandForm, m_command);
    return labels;
}

// --- 跳板机行 ------------------------------------------------------------

void ProxySettingsWidget::addHopRow(const QString &deviceId)
{
    HopRow hop;
    hop.row = new QWidget(m_stack->widget(kPageJump));
    auto *lay = new QHBoxLayout(hop.row);
    lay->setContentsMargins(0, 0, 0, 0);
    hop.label = new QLabel(hop.row);
    hop.combo = new QComboBox(hop.row);
    hop.combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    fillHopCombo(hop.combo, deviceId);
    auto *del = new QPushButton(tr("移除"), hop.row);
    lay->addWidget(hop.label);
    lay->addWidget(hop.combo, 1);
    lay->addWidget(del);
    m_hopLayout->addWidget(hop.row);
    m_hops.append(hop);

    // 捕获 row 指针而不是下标：删中间一行之后下标会全体前移，
    // 捕获下标的按钮就会删错行。
    QWidget *rowPtr = hop.row;
    connect(del, &QPushButton::clicked, this, [this, rowPtr]() {
        removeHopRow(rowPtr);
        emit typeChanged();   // 行数变了，宿主可能要重算高度
    });

    relabelHops();
    // 到达上限就不给再加了：真正的防线是连接期的环检测，这个上限管的是
    // "没有环但链条荒谬地长"（见 kMaxJumpHops 的注释）。
    m_addHop->setEnabled(m_hops.size() < kMaxJumpHops);
    emit typeChanged();
}

void ProxySettingsWidget::removeHopRow(QWidget *row)
{
    for (int i = 0; i < m_hops.size(); ++i) {
        if (m_hops.at(i).row != row)
            continue;
        m_hops.removeAt(i);
        m_hopLayout->removeWidget(row);
        // deleteLater 而非 delete：本函数可能是从 row 自己的子按钮的
        // clicked 信号里进来的，就地 delete 会销毁正在派发信号的对象。
        row->setParent(nullptr);
        row->deleteLater();
        break;
    }
    relabelHops();
    m_addHop->setEnabled(m_hops.size() < kMaxJumpHops);
}

void ProxySettingsWidget::relabelHops()
{
    // 序号 = 拨号顺序（第 1 跳先连），与 hopIds 的存储顺序一致，
    // 也与 OpenSSH `-J a,b` 的语义一致。
    for (int i = 0; i < m_hops.size(); ++i)
        m_hops.at(i).label->setText(tr("第 %1 跳：").arg(i + 1));
}

void ProxySettingsWidget::fillHopCombo(QComboBox *combo, const QString &selectedId) const
{
    // 重填期间屏蔽信号：否则每 addItem 一次就发一轮 currentIndexChanged。
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(tr("（请选择跳板机）"), QLatin1String(kUnsetHopData));
    bool found = false;
    for (const ProxyDeviceItem &item : m_catalog) {
        if (item.id.isEmpty() || item.id == m_excludeId)
            continue;   // 把自己当跳板 = 成环的最短路径
        combo->addItem(item.name, item.id);
        if (item.id == selectedId)
            found = true;
    }
    if (!selectedId.isEmpty() && !found) {
        // 被引用的设备已经不在了。保留这个 id 并标出来，让 validate() 当场
        // 拦下要求修正——静默丢掉的话用户会以为配置还好着，直到某天连不上。
        combo->addItem(tr("（引用的设备已删除）"), selectedId);
    }
    const int idx = combo->findData(selectedId.isEmpty() ? QString(QLatin1String(kUnsetHopData))
                                                         : selectedId);
    combo->setCurrentIndex(idx < 0 ? 0 : idx);
}

// --- 显隐与校验 ----------------------------------------------------------

void ProxySettingsWidget::onTypeChanged()
{
    const ProxyType type = selectedType();

    switch (type) {
    case ProxyType::Http:
    case ProxyType::Socks5:
        m_stack->setCurrentIndex(kPageHostPort);
        break;
    case ProxyType::Command:
        m_stack->setCurrentIndex(kPageCommand);
        break;
    case ProxyType::JumpHost:
        m_stack->setCurrentIndex(kPageJump);
        break;
    default:
        m_stack->setCurrentIndex(kPageNoParam);
        break;
    }

    switch (type) {
    case ProxyType::None:
        m_noParamHint->setText(tr("直接连接目标主机，不经过任何代理。"));
        break;
    case ProxyType::Global:
        m_noParamHint->setText(tr("使用「设置 → 代理」里配置的那一份代理。\n"
                                  "改一处即对所有选了本项的设备生效。"));
        break;
    case ProxyType::System:
        m_noParamHint->setText(tr("使用操作系统的代理设置（含 PAC / 自动配置）。\n"
                                  "系统未配置代理时等同于直连。"));
        break;
    default:
        break;
    }

    // 切类型时给出合理的默认端口。只在端口框为空、或里面放的还是上一种
    // 类型的默认值时才改——用户自己填过端口就一直保留。
    if (type == ProxyType::Http || type == ProxyType::Socks5) {
        const QString cur = m_port->text().trimmed();
        const bool untouched =
            cur.isEmpty()
            || (m_portDefaultFor != ProxyType::None
                && cur == QString::number(proxyDefaultPort(m_portDefaultFor)));
        if (untouched) {
            m_port->setText(QString::number(proxyDefaultPort(type)));
            m_portDefaultFor = type;
        }
    }

    if (type == ProxyType::JumpHost) {
        m_hopHint->setText(
            m_catalog.isEmpty()
                ? tr("还没有可作跳板机的 SSH 设备。请先在左侧保存一台跳板机，再回来配置。")
                : tr("按拨号顺序排列：先连第 1 跳，再经它连下一跳，最后到本设备。\n"
                     "跳板机的用户名/口令直接复用它自己保存的那份，不必在这里重复录入。"));
    }

    emit typeChanged();
}

bool ProxySettingsWidget::validate(QString *err) const
{
    const ProxyType type = selectedType();
    switch (type) {
    case ProxyType::Http:
    case ProxyType::Socks5: {
        if (m_host->text().trimmed().isEmpty()) {
            *err = tr("代理地址不能为空。");
            return false;
        }
        // toUShort 失败静默返回 0，所以必须显式校验：写着 "http" 的端口框
        // 会被当成 0，然后在连接期表现成一句莫名的"连接被拒绝"。
        bool ok = false;
        const uint port = m_port->text().trimmed().toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            *err = tr("代理端口必须是 1-65535 之间的数字。");
            return false;
        }
        return true;
    }
    case ProxyType::Command:
        if (m_command->text().trimmed().isEmpty()) {
            *err = tr("代理命令不能为空。");
            return false;
        }
        return true;
    case ProxyType::JumpHost: {
        if (m_hops.isEmpty()) {
            *err = tr("请至少添加一台跳板机，或把代理类型改为「不使用代理」。");
            return false;
        }
        QSet<QString> seen;
        for (int i = 0; i < m_hops.size(); ++i) {
            const QString id = m_hops.at(i).combo->currentData().toString();
            if (id.isEmpty()) {
                *err = tr("第 %1 跳还没有选择跳板机。").arg(i + 1);
                return false;
            }
            // 目录里查不到 = 引用了已删除的设备（fillHopCombo 会把它显示成
            // 「引用的设备已删除」）。连接期这会是一句"跳板机已不存在"，
            // 不如在这里就要求修正。
            bool known = false;
            for (const ProxyDeviceItem &item : m_catalog) {
                if (item.id == id) { known = true; break; }
            }
            if (!known) {
                *err = tr("第 %1 跳引用的设备已被删除，请重新选择。").arg(i + 1);
                return false;
            }
            if (seen.contains(id)) {
                *err = tr("第 %1 跳与前面某一跳是同一台设备。").arg(i + 1);
                return false;
            }
            seen.insert(id);
        }
        return true;
    }
    case ProxyType::None:
    case ProxyType::Global:
    case ProxyType::System:
        return true;
    }
    return true;
}

} // namespace cubeshell
