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
    setMinimumWidth(386);

    auto *form = new QFormLayout;

    // 转发模式（本地/远程/动态，与 tunnel.json 的 tunnel_type 值一致）。
    m_type = new QComboBox(this);
    m_type->addItem(tr("本地"), QStringLiteral("本地"));
    m_type->addItem(tr("远程"), QStringLiteral("远程"));
    m_type->addItem(tr("动态"), QStringLiteral("动态"));
    form->addRow(tr("转发模式"), m_type);

    m_device = new QComboBox(this);
    m_device->addItems(deviceNames);
    form->addRow(tr("SSH 服务器"), m_device);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("如:nginx"));
    form->addRow(tr("隧道名称"), m_name);

    m_remoteBind = new QLineEdit(this);
    m_remoteBind->setPlaceholderText(tr("请输入远程绑定地址，例如:localhost:8080"));
    form->addRow(tr("远程绑定地址"), m_remoteBind);

    m_localBind = new QLineEdit(this);
    m_localBind->setPlaceholderText(tr("请输入本地绑定地址，例如:localhost:8080"));
    form->addRow(tr("本地绑定地址"), m_localBind);

    m_browserOpen = new QLineEdit(this);
    m_browserOpen->setPlaceholderText(QStringLiteral("https://127.0.0.1:80"));
    form->addRow(tr("浏览器打开"), m_browserOpen);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddTunnelDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(m_type, &QComboBox::currentIndexChanged, this, &AddTunnelDialog::onTypeChanged);
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
    const bool dynamic = m_type->itemData(index).toString() == QLatin1String("动态");
    m_remoteBind->setEnabled(!dynamic);
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
