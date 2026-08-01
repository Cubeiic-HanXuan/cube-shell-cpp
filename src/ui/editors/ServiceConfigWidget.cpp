// ServiceConfigWidget.cpp — see ServiceConfigWidget.h.
// 对应Python: core/docker/docker_compose_editor.py:19-445 (ServiceConfigWidget)

#include "ServiceConfigWidget.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace cubeshell {

namespace {

// 对应Python: str(value)（环境变量/构建参数的值展示）
QString variantToText(const QVariant &value)
{
    return value.toString();
}

// list 或 dict 键 → 字符串列表（depends_on / networks 两种 YAML 写法）
// 对应Python: 207-214 / 229-236 行的 isinstance(list/dict) 分支
QStringList namesFromListOrMap(const QVariant &value)
{
    QStringList names;
    if (value.typeId() == QMetaType::QVariantMap) {
        const QVariantMap map = value.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            names << it.key();
    } else if (value.typeId() == QMetaType::QVariantList
               || value.typeId() == QMetaType::QStringList) {
        const QVariantList list = value.toList();
        for (const QVariant &v : list)
            names << v.toString();
    }
    return names;
}

} // namespace

ServiceConfigWidget::ServiceConfigWidget(const QString &serviceName,
                                         const QVariantMap &config, QWidget *parent)
    : QWidget(parent)
    , m_serviceName(serviceName)
    , m_config(config)
{
    // 创建主布局
    auto *mainLayout = new QVBoxLayout(this);

    // 服务名称标签
    auto *title = new QLabel(tr("服务: %1").arg(serviceName), this);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold;"));
    mainLayout->addWidget(title);

    // 创建滚动区域
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    mainLayout->addWidget(scroll);

    // 创建配置容器
    auto *configContainer = new QWidget;
    scroll->setWidget(configContainer);

    // 配置表单布局
    auto *formLayout = new QFormLayout(configContainer);
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    formLayout->setLabelAlignment(Qt::AlignRight);

    // 基本配置
    m_imageEdit = new QLineEdit(m_config.value(QStringLiteral("image")).toString());
    m_imageEdit->setMinimumWidth(300);
    formLayout->addRow(tr("镜像:"), m_imageEdit);

    // 容器名称
    m_containerNameEdit = new QLineEdit(m_config.value(QStringLiteral("container_name")).toString());
    m_containerNameEdit->setMinimumWidth(300);
    formLayout->addRow(tr("容器名称:"), m_containerNameEdit);

    // 重启策略（下拉选择）
    m_restartCombo = new QComboBox;
    m_restartCombo->setMinimumWidth(300);
    // 添加常用的重启策略选项
    m_restartCombo->addItems({QString(),
                              QStringLiteral("no"),
                              QStringLiteral("always"),
                              QStringLiteral("on-failure"),
                              QStringLiteral("unless-stopped")});
    // 设置当前值
    const int restartIndex =
        m_restartCombo->findText(m_config.value(QStringLiteral("restart")).toString());
    if (restartIndex >= 0)
        m_restartCombo->setCurrentIndex(restartIndex);
    // 添加提示
    m_restartCombo->setToolTip(tr("no: 不自动重启\n"
                                  "always: 总是重启\n"
                                  "on-failure: 非正常退出时重启\n"
                                  "unless-stopped: 除非手动停止，否则总是重启"));
    formLayout->addRow(tr("重启策略:"), m_restartCombo);

    // 命令 - 支持字符串或列表类型（列表转为空格拼接的字符串展示）
    const QVariant commandVar = m_config.value(QStringLiteral("command"));
    QString command;
    if (commandVar.typeId() == QMetaType::QVariantList
        || commandVar.typeId() == QMetaType::QStringList) {
        QStringList parts;
        const QVariantList list = commandVar.toList();
        for (const QVariant &v : list)
            parts << v.toString();
        command = parts.join(QLatin1Char(' '));
    } else {
        command = commandVar.toString();
    }
    m_commandEdit = new QLineEdit(command);
    m_commandEdit->setMinimumWidth(300);
    m_commandEdit->setPlaceholderText(tr("例如: nginx -g 'daemon off;'"));
    formLayout->addRow(tr("命令:"), m_commandEdit);

    // Build 配置
    auto *buildContainer = new QWidget;
    auto *buildLayout = new QVBoxLayout(buildContainer);
    const QVariantMap buildConfig = m_config.value(QStringLiteral("build")).toMap();

    // Context 路径
    m_contextEdit = new QLineEdit(buildConfig.value(QStringLiteral("context")).toString());
    m_contextEdit->setMinimumWidth(300);
    buildLayout->addWidget(new QLabel(tr("Context 路径:")));
    buildLayout->addWidget(m_contextEdit);

    // Dockerfile 路径
    m_dockerfileEdit = new QLineEdit(buildConfig.value(QStringLiteral("dockerfile")).toString());
    m_dockerfileEdit->setMinimumWidth(300);
    buildLayout->addWidget(new QLabel(tr("Dockerfile 路径:")));
    buildLayout->addWidget(m_dockerfileEdit);

    // Build 参数（dict 与 "k=v" 列表两种格式）
    auto *buildArgsContainer = new QWidget;
    m_buildArgsLayout = new QVBoxLayout(buildArgsContainer);

    auto *addBuildArgBtn = new QPushButton(tr("添加构建参数"));
    connect(addBuildArgBtn, &QPushButton::clicked, this, [this]() {
        addPairRow(m_buildArgsLayout, &m_buildArgsList, QString(), QString());
    });
    m_buildArgsLayout->addWidget(addBuildArgBtn);

    const QVariant buildArgs = buildConfig.value(QStringLiteral("args"));
    if (buildArgs.typeId() == QMetaType::QVariantMap) {
        // 处理字典格式
        const QVariantMap map = buildArgs.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            addPairRow(m_buildArgsLayout, &m_buildArgsList, it.key(), variantToText(it.value()));
    } else if (buildArgs.typeId() == QMetaType::QVariantList
               || buildArgs.typeId() == QMetaType::QStringList) {
        // 处理列表格式
        const QVariantList list = buildArgs.toList();
        for (const QVariant &v : list) {
            const QString arg = v.toString();
            const int eq = arg.indexOf(QLatin1Char('='));
            if (eq >= 0)
                addPairRow(m_buildArgsLayout, &m_buildArgsList, arg.left(eq), arg.mid(eq + 1));
        }
    }

    buildLayout->addWidget(new QLabel(tr("构建参数:")));
    buildLayout->addWidget(buildArgsContainer);

    formLayout->addRow(tr("构建配置:"), buildContainer);

    // 端口配置
    auto *portsContainer = new QWidget;
    m_portsLayout = new QVBoxLayout(portsContainer);

    auto *addPortBtn = new QPushButton(tr("添加端口"));
    connect(addPortBtn, &QPushButton::clicked, this, [this]() {
        addSingleRow(m_portsLayout, &m_portsList, QString(), QStringLiteral("—"));
    });
    m_portsLayout->addWidget(addPortBtn);

    const QVariantList ports = m_config.value(QStringLiteral("ports")).toList();
    for (const QVariant &port : ports)
        addSingleRow(m_portsLayout, &m_portsList, port.toString(), QStringLiteral("—"));

    formLayout->addRow(tr("端口:"), portsContainer);

    // 环境变量（dict 与 "k=v" 列表两种格式）
    auto *envContainer = new QWidget;
    m_envLayout = new QVBoxLayout(envContainer);

    auto *addEnvBtn = new QPushButton(tr("添加环境变量"));
    connect(addEnvBtn, &QPushButton::clicked, this, [this]() {
        addPairRow(m_envLayout, &m_envList, QString(), QString());
    });
    m_envLayout->addWidget(addEnvBtn);

    const QVariant envConfig = m_config.value(QStringLiteral("environment"));
    if (envConfig.typeId() == QMetaType::QVariantMap) {
        // 处理字典格式
        const QVariantMap map = envConfig.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            addPairRow(m_envLayout, &m_envList, it.key(), variantToText(it.value()));
    } else if (envConfig.typeId() == QMetaType::QVariantList
               || envConfig.typeId() == QMetaType::QStringList) {
        // 处理列表格式
        const QVariantList list = envConfig.toList();
        for (const QVariant &v : list) {
            const QString env = v.toString();
            const int eq = env.indexOf(QLatin1Char('='));
            if (eq >= 0)
                addPairRow(m_envLayout, &m_envList, env.left(eq), env.mid(eq + 1));
        }
    }

    formLayout->addRow(tr("环境变量:"), envContainer);

    // 卷挂载
    auto *volumesContainer = new QWidget;
    m_volumesLayout = new QVBoxLayout(volumesContainer);

    auto *addVolumeBtn = new QPushButton(tr("添加卷"));
    connect(addVolumeBtn, &QPushButton::clicked, this, [this]() {
        addSingleRow(m_volumesLayout, &m_volumesList, QString(), QStringLiteral("—"));
    });
    m_volumesLayout->addWidget(addVolumeBtn);

    const QVariantList volumes = m_config.value(QStringLiteral("volumes")).toList();
    for (const QVariant &volume : volumes)
        addSingleRow(m_volumesLayout, &m_volumesList, volume.toString(), QStringLiteral("—"));

    formLayout->addRow(tr("卷:"), volumesContainer);

    // 依赖服务（list 与 dict 两种格式）
    auto *dependsOnContainer = new QWidget;
    m_dependsOnLayout = new QVBoxLayout(dependsOnContainer);

    auto *addDependsOnBtn = new QPushButton(tr("添加依赖服务"));
    connect(addDependsOnBtn, &QPushButton::clicked, this, [this]() {
        addSingleRow(m_dependsOnLayout, &m_dependsOnList, QString(), QStringLiteral("-"));
    });
    m_dependsOnLayout->addWidget(addDependsOnBtn);

    const QStringList dependsOn =
        namesFromListOrMap(m_config.value(QStringLiteral("depends_on")));
    for (const QString &service : dependsOn)
        addSingleRow(m_dependsOnLayout, &m_dependsOnList, service, QStringLiteral("-"));

    formLayout->addRow(tr("依赖服务:"), dependsOnContainer);

    // 网络配置（list 与 dict 两种格式）
    auto *networksContainer = new QWidget;
    m_networksLayout = new QVBoxLayout(networksContainer);

    auto *addNetworkBtn = new QPushButton(tr("添加网络"));
    connect(addNetworkBtn, &QPushButton::clicked, this, [this]() {
        addSingleRow(m_networksLayout, &m_networksList, QString(), QStringLiteral("-"));
    });
    m_networksLayout->addWidget(addNetworkBtn);

    const QStringList networks = namesFromListOrMap(m_config.value(QStringLiteral("networks")));
    for (const QString &network : networks)
        addSingleRow(m_networksLayout, &m_networksList, network, QStringLiteral("-"));

    formLayout->addRow(tr("网络:"), networksContainer);

    // 保存按钮
    auto *saveBtn = new QPushButton(tr("保存配置"), this);
    connect(saveBtn, &QPushButton::clicked, this, &ServiceConfigWidget::onSaveClicked);
    mainLayout->addWidget(saveBtn);
}

// 对应Python: add_build_arg_item / add_env_item（249-265 / 295-310 行）
void ServiceConfigWidget::addPairRow(QVBoxLayout *layout,
                                     QList<QPair<QLineEdit *, QLineEdit *>> *list,
                                     const QString &key, const QString &value)
{
    auto *row = new QWidget;
    auto *rowLayout = new QHBoxLayout(row);
    auto *keyEdit = new QLineEdit(key);
    keyEdit->setMinimumWidth(150);
    auto *valueEdit = new QLineEdit(value);
    valueEdit->setMinimumWidth(150);
    auto *deleteBtn = new QPushButton(QStringLiteral("—"));
    deleteBtn->setStyleSheet(QStringLiteral("color: red; font-weight: bold;"));
    // 对应Python: remove_build_arg / remove_env
    connect(deleteBtn, &QPushButton::clicked, this, [list, row, keyEdit, valueEdit]() {
        list->removeOne(qMakePair(keyEdit, valueEdit));
        row->deleteLater();
    });
    rowLayout->addWidget(keyEdit);
    rowLayout->addWidget(valueEdit);
    rowLayout->addWidget(deleteBtn);
    list->append(qMakePair(keyEdit, valueEdit));
    // "添加XX"按钮始终位于末尾，行插入到按钮之前
    layout->insertWidget(layout->count() - 1, row);
}

// 对应Python: add_port_item / add_volume_item / add_depends_on_item /
//             add_network_item（275-288 / 323-336 / 346-359 / 369-382 行）
void ServiceConfigWidget::addSingleRow(QVBoxLayout *layout, QList<QLineEdit *> *list,
                                       const QString &value, const QString &deleteText)
{
    auto *row = new QWidget;
    auto *rowLayout = new QHBoxLayout(row);
    auto *edit = new QLineEdit(value);
    edit->setMinimumWidth(300);
    auto *deleteBtn = new QPushButton(deleteText);
    deleteBtn->setStyleSheet(QStringLiteral("color: red; font-weight: bold;"));
    // 对应Python: remove_port / remove_volume / remove_depends_on / remove_network
    connect(deleteBtn, &QPushButton::clicked, this, [list, row, edit]() {
        list->removeOne(edit);
        row->deleteLater();
    });
    rowLayout->addWidget(edit);
    rowLayout->addWidget(deleteBtn);
    list->append(edit);
    layout->insertWidget(layout->count() - 1, row);
}

// 对应Python: save_config 392-438 行（逐条对应，剔除空值）
QVariantMap ServiceConfigWidget::collectConfig() const
{
    // 处理环境变量
    QVariantMap envDict;
    for (const auto &pair : m_envList) {
        const QString key = pair.first->text().trimmed();
        const QString value = pair.second->text().trimmed();
        if (!key.isEmpty() && !value.isEmpty())
            envDict.insert(key, value);
    }

    // 处理构建参数
    QVariantMap buildArgs;
    for (const auto &pair : m_buildArgsList) {
        const QString key = pair.first->text().trimmed();
        const QString value = pair.second->text().trimmed();
        if (!key.isEmpty() && !value.isEmpty())
            buildArgs.insert(key, value);
    }

    // 构建配置
    QVariantMap buildConfig;
    if (!m_contextEdit->text().trimmed().isEmpty())
        buildConfig.insert(QStringLiteral("context"), m_contextEdit->text().trimmed());
    if (!m_dockerfileEdit->text().trimmed().isEmpty())
        buildConfig.insert(QStringLiteral("dockerfile"), m_dockerfileEdit->text().trimmed());
    if (!buildArgs.isEmpty())
        buildConfig.insert(QStringLiteral("args"), buildArgs);

    // 处理端口 / 卷 / 依赖服务 / 网络（过滤空行）
    QVariantList ports;
    for (const QLineEdit *edit : m_portsList)
        if (!edit->text().isEmpty())
            ports << edit->text();
    QVariantList volumes;
    for (const QLineEdit *edit : m_volumesList)
        if (!edit->text().isEmpty())
            volumes << edit->text();
    QVariantList dependsOn;
    for (const QLineEdit *edit : m_dependsOnList)
        if (!edit->text().isEmpty())
            dependsOn << edit->text();
    QVariantList networks;
    for (const QLineEdit *edit : m_networksList)
        if (!edit->text().isEmpty())
            networks << edit->text();

    // 更新当前服务的配置并移除空值（与 Python 一致：image/ports/environment/
    // volumes 恒保留，其余为空则剔除；command 保持字符串形式）
    QVariantMap config;
    config.insert(QStringLiteral("image"), m_imageEdit->text());
    if (!m_containerNameEdit->text().isEmpty())
        config.insert(QStringLiteral("container_name"), m_containerNameEdit->text());
    if (!m_restartCombo->currentText().isEmpty())
        config.insert(QStringLiteral("restart"), m_restartCombo->currentText());
    if (!buildConfig.isEmpty())
        config.insert(QStringLiteral("build"), buildConfig);
    config.insert(QStringLiteral("ports"), ports);
    config.insert(QStringLiteral("environment"), envDict);
    if (!m_commandEdit->text().isEmpty())
        config.insert(QStringLiteral("command"), m_commandEdit->text());
    config.insert(QStringLiteral("volumes"), volumes);
    if (!dependsOn.isEmpty())
        config.insert(QStringLiteral("depends_on"), dependsOn);
    if (!networks.isEmpty())
        config.insert(QStringLiteral("networks"), networks);
    return config;
}

void ServiceConfigWidget::onSaveClicked()
{
    m_config = collectConfig();
    emit configSaved(m_serviceName, m_config);
}

} // namespace cubeshell
