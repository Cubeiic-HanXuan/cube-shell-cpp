// ServiceSearchDialog.cpp — see ServiceSearchDialog.h.
// 对应Python: core/docker/docker_compose_editor.py:448-527 (ServiceSearchDialog)

#include "ServiceSearchDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

ServiceSearchDialog::ServiceSearchDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("搜索并添加服务"));
    setMinimumWidth(600);
    setMinimumHeight(400);

    auto *layout = new QVBoxLayout(this);

    // 搜索框
    auto *searchLayout = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("输入服务名称搜索..."));
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &ServiceSearchDialog::filterServices);
    searchLayout->addWidget(m_searchEdit);
    layout->addLayout(searchLayout);

    // 服务列表
    m_serviceList = new QTreeWidget(this);
    m_serviceList->setHeaderLabels({tr("服务名称"), tr("描述")});
    connect(m_serviceList, &QTreeWidget::itemDoubleClicked,
            this, &ServiceSearchDialog::accept);
    layout->addWidget(m_serviceList);

    // 按钮
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ServiceSearchDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ServiceSearchDialog::reject);
    layout->addWidget(buttons);

    // 从本地文件加载预定义服务
    loadPredefinedServices();
    updateServiceList();
}

// 对应Python: load_predefined_services（484-506 行，含加载失败警告弹窗）
void ServiceSearchDialog::loadPredefinedServices()
{
    QString error;
    m_services = ComposeYaml::loadComposeServices(
        ComposeYaml::defaultComposeFullPath(), &error);
    if (!error.isEmpty())
        QMessageBox::warning(this, tr("警告"), tr("加载预定义服务失败: %1").arg(error));
}

// 对应Python: update_service_list（508-512 行）
void ServiceSearchDialog::updateServiceList()
{
    m_serviceList->clear();
    for (const ComposeYaml::ComposeServiceInfo &info : std::as_const(m_services))
        m_serviceList->addTopLevelItem(
            new QTreeWidgetItem(QStringList{info.name, info.description}));
}

// 对应Python: filter_services（514-520 行，lower() 后子串匹配名称或描述）
void ServiceSearchDialog::filterServices()
{
    const QString searchText = m_searchEdit->text().toLower();
    for (int i = 0; i < m_serviceList->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_serviceList->topLevelItem(i);
        const QString name = item->text(0).toLower();
        const QString description = item->text(1).toLower();
        item->setHidden(!(name.contains(searchText) || description.contains(searchText)));
    }
}

// 对应Python: get_selected_service（522-527 行）
QString ServiceSearchDialog::selectedServiceName() const
{
    const QList<QTreeWidgetItem *> selected = m_serviceList->selectedItems();
    return selected.isEmpty() ? QString() : selected.first()->text(0);
}

QVariantMap ServiceSearchDialog::selectedServiceConfig() const
{
    const QString name = selectedServiceName();
    for (const ComposeYaml::ComposeServiceInfo &info : m_services)
        if (info.name == name)
            return info.config;
    return QVariantMap();
}

} // namespace cubeshell
