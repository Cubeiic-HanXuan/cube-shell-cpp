#include "AddTunnelDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: ui/add_tunnel_config.py::Ui_AddTunnelConfig.setupUi
AddTunnelDialog::AddTunnelDialog(const QStringList &deviceNames, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("添加SSH隧道"));
    setMinimumWidth(480);
    resize(560, 360);
    setSizeGripEnabled(true);

    auto *form = new QFormLayout;
    // macOS 默认 FieldsStayAtSizeHint 会让短选项下拉框缩到内容宽度。
    // 字段统一占满右侧列；缩放窗口和字体时仍保持相同的左右边界。
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);

    // 转发模式（本地/远程/动态，与 tunnel.json 的 tunnel_type 值一致）。
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("tunnelType"));
    m_type->addItem(tr("本地"), QStringLiteral("本地"));
    m_type->addItem(tr("远程"), QStringLiteral("远程"));
    m_type->addItem(tr("动态"), QStringLiteral("动态"));
    m_type->setItemData(0, tr("本地转发"), Qt::ToolTipRole);
    m_type->setItemData(1, tr("远程转发"), Qt::ToolTipRole);
    m_type->setItemData(2, tr("动态转发（SOCKS5）"), Qt::ToolTipRole);
    form->addRow(tr("转发模式"), m_type);

    m_device = new QComboBox(this);
    m_device->setObjectName(QStringLiteral("tunnelDevice"));
    m_device->setPlaceholderText(tr("请选择 SSH 服务器"));
    m_device->addItems(deviceNames);
    if (m_device->count() > 0) m_device->setCurrentIndex(0);
    for (int index = 0; index < m_device->count(); ++index)
        m_device->setItemData(index, m_device->itemText(index), Qt::ToolTipRole);
    connect(m_device, &QComboBox::currentTextChanged, m_device, &QWidget::setToolTip);
    m_device->setToolTip(m_device->currentText());
    form->addRow(tr("SSH 服务器"), m_device);
    for (auto *combo : {m_type, m_device}) {
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // 两者使用相同的基础内容宽度，并为箭头留空间；超长设备名不撑大整个窗口。
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(24);
    }

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("tunnelName"));
    m_name->setPlaceholderText(tr("例如：nginx"));
    form->addRow(tr("隧道名称"), m_name);

    m_remoteBind = new QLineEdit(this);
    m_remoteBind->setObjectName(QStringLiteral("remoteBindAddress"));
    m_remoteBind->setToolTip(tr("使用 主机:端口 格式，例如 localhost:8080；动态转发无需填写。"));
    form->addRow(tr("远程绑定地址"), m_remoteBind);

    m_localBind = new QLineEdit(this);
    m_localBind->setObjectName(QStringLiteral("localBindAddress"));
    m_localBind->setPlaceholderText(tr("例如：127.0.0.1:8080"));
    form->addRow(tr("本地绑定地址"), m_localBind);

    m_browserOpen = new QLineEdit(this);
    m_browserOpen->setObjectName(QStringLiteral("browserOpen"));
    m_browserOpen->setPlaceholderText(tr("可选：http://127.0.0.1:8080"));
    m_browserOpen->setToolTip(tr("隧道启动后自动在浏览器中打开的 URL，可留空。"));
    form->addRow(tr("浏览器打开"), m_browserOpen);
    for (auto *edit : {m_name, m_remoteBind, m_localBind, m_browserOpen})
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddTunnelDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(16);
    layout->addLayout(form);
    layout->addStretch(1);
    layout->addWidget(buttons);

    connect(m_type, &QComboBox::currentIndexChanged, this, &AddTunnelDialog::onTypeChanged);
    onTypeChanged(m_type->currentIndex());
}

void AddTunnelDialog::setEntry(const QString &name, const TunnelEntry &entry)
{
    m_name->setText(name);
    m_name->setReadOnly(true);
    const int idx = m_type->findData(entry.tunnelType);
    if (idx >= 0)
        m_type->setCurrentIndex(idx);
    const int devIdx = m_device->findText(entry.deviceName);
    if (devIdx >= 0)
        m_device->setCurrentIndex(devIdx);
    m_remoteBind->setText(entry.remoteBindAddress);
    m_localBind->setText(entry.localBindAddress);
    m_browserOpen->setText(entry.browserOpen);
}

QString AddTunnelDialog::tunnelName() const
{
    return m_name->text().trimmed();
}

// 对应Python: cube-shell.py::AddTunnelConfig.addTunnel 组装的 dic
TunnelEntry AddTunnelDialog::entry() const
{
    TunnelEntry e;
    e.tunnelType = m_type->currentData().toString();
    e.deviceName = m_device->currentText();
    e.remoteBindAddress = m_remoteBind->text().trimmed();
    e.localBindAddress = m_localBind->text().trimmed();
    e.browserOpen = m_browserOpen->text().trimmed();
    return e;
}

void AddTunnelDialog::onTypeChanged(int index)
{
    // 动态转发（SOCKS5）没有远程绑定地址。
    const bool dynamic = m_type->itemData(index).toString() == QStringLiteral("动态");
    m_type->setToolTip(m_type->itemData(index, Qt::ToolTipRole).toString());
    m_remoteBind->setEnabled(!dynamic);
    m_remoteBind->setPlaceholderText(dynamic ? tr("动态转发无需填写") : tr("例如：localhost:8080"));
    if (dynamic)
        m_remoteBind->clear();
}

void AddTunnelDialog::accept()
{
    if (tunnelName().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("请填写隧道名称"));
        return;
    }
    if (!entry().isValid()) {
        QMessageBox::warning(this, windowTitle(), tr("请填写本地绑定地址"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
