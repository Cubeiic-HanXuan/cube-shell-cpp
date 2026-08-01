#pragma once

// ServiceSearchDialog.h — 从内置 docker-compose-full.yml 搜索并添加预定义服务。
// 对应Python: core/docker/docker_compose_editor.py:448-527 (ServiceSearchDialog)
//
// 数据源走 ComposeYaml::loadComposeServices(defaultComposeFullPath())，
// 对应 Python 的 load_predefined_services（conf/docker-compose-full.yml）。

#include <QDialog>
#include <QList>
#include <QVariantMap>

#include "docker/ComposeYaml.h"

class QLineEdit;
class QTreeWidget;

namespace cubeshell {

class ServiceSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServiceSearchDialog(QWidget *parent = nullptr);

    // 对应Python: get_selected_service（无选中项时返回空串/空 map）
    QString selectedServiceName() const;
    QVariantMap selectedServiceConfig() const;

private slots:
    // 名称/描述子串过滤，不区分大小写。对应Python: filter_services
    void filterServices();

private:
    void loadPredefinedServices(); // 对应Python: load_predefined_services
    void updateServiceList();      // 对应Python: update_service_list

    QLineEdit *m_searchEdit = nullptr;
    QTreeWidget *m_serviceList = nullptr;
    QList<ComposeYaml::ComposeServiceInfo> m_services;
};

} // namespace cubeshell
