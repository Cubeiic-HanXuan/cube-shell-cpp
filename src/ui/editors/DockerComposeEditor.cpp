// DockerComposeEditor.cpp — see DockerComposeEditor.h.
// 对应Python: core/docker/docker_compose_editor.py:592-1100 (DockerComposeEditor)

#include "DockerComposeEditor.h"

#include <QCloseEvent>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "docker/ComposeYaml.h"
#include "docker/DockerManager.h"
#include "DockerDaemonConfigDialog.h"
#include "ServiceConfigWidget.h"
#include "ServiceSearchDialog.h"

namespace cubeshell {

DockerComposeEditor::DockerComposeEditor(DockerManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("Docker Compose 可视化编辑器"));

    // 创建主布局
    auto *mainLayout = new QHBoxLayout(this);

    // 创建垂直分割器（上下分割）
    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    mainLayout->addWidget(verticalSplitter);

    // 创建水平分割器（左右分割）
    auto *horizontalSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);
    verticalSplitter->addWidget(horizontalSplitter);

    // 左侧服务列表
    auto *leftWidget = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftWidget);

    // 添加服务列表标题（qss 与 Python 版一致）
    auto *servicesTitle = new QLabel(tr("服务列表"), leftWidget);
    servicesTitle->setStyleSheet(QStringLiteral(R"(
            font-size: 16px; 
            font-weight: bold; 
            color: #333; 
            padding: 5px;
            background-color: #f0f0f0;
            border-bottom: 1px solid #ccc;
        )"));
    servicesTitle->setAlignment(Qt::AlignCenter);
    leftLayout->addWidget(servicesTitle);

    m_servicesTree = new QTreeWidget(leftWidget);
    m_servicesTree->setHeaderHidden(true); // 隐藏表头
    m_servicesTree->setAnimated(true);     // 展开/折叠动画
    m_servicesTree->setAlternatingRowColors(false); // 禁用交替行颜色，避免显示问题
    m_servicesTree->setStyleSheet(QStringLiteral(R"(
            QTreeWidget {
                border: 1px solid #ccc;
                border-radius: 4px;
                padding: 5px;
            }
            QTreeWidget::item {
                padding: 10px;
                border-bottom: 1px solid #eee;
            }
            QTreeWidget::item:selected {
                background-color: #0078d7;
                color: #ffffff;
            }
            QTreeWidget::item:hover {
                background-color: rgba(0, 120, 215, 0.1);
            }
            QTreeWidget::item:selected:hover {
                background-color: #3297e6;
                color: #ffffff;
            }
        )"));
    m_servicesTree->setRootIsDecorated(false);
    m_servicesTree->setIndentation(0);
    m_servicesTree->setIconSize(QSize(24, 24));
    connect(m_servicesTree, &QTreeWidget::itemClicked,
            this, &DockerComposeEditor::onServiceSelected);
    leftLayout->addWidget(m_servicesTree);

    auto *addServiceBtn = new QPushButton(tr("添加服务"), leftWidget);
    addServiceBtn->setStyleSheet(QStringLiteral(R"(
            QPushButton {
                background-color: #4CAF50;
                color: white;
                font-weight: bold;
                border: none;
                padding: 8px;
                border-radius: 4px;
            }
            QPushButton:hover {
                background-color: #45a049;
            }
            QPushButton:pressed {
                background-color: #3d8b40;
            }
        )"));
    addServiceBtn->setCursor(Qt::PointingHandCursor);
    connect(addServiceBtn, &QPushButton::clicked, this, &DockerComposeEditor::addService);
    leftLayout->addWidget(addServiceBtn);

    // 右侧配置编辑区
    auto *rightWidget = new QWidget;
    m_rightLayout = new QVBoxLayout(rightWidget);

    m_configWidget = new QWidget(rightWidget);
    m_rightLayout->addWidget(m_configWidget);

    // 添加水平分割器部件
    horizontalSplitter->addWidget(leftWidget);
    horizontalSplitter->addWidget(rightWidget);
    horizontalSplitter->setSizes({300, 900}); // 设置左右区域的比例

    // 创建命令执行区域
    auto *commandWidget = new QWidget;
    auto *commandLayout = new QVBoxLayout(commandWidget);

    // 命令按钮组
    auto *buttonLayout = new QHBoxLayout();
    auto *upBtn = new QPushButton(tr("启动服务"), commandWidget);
    connect(upBtn, &QPushButton::clicked, this,
            [this]() { executeCommand(QStringLiteral("up -d")); });
    auto *downBtn = new QPushButton(tr("停止服务"), commandWidget);
    connect(downBtn, &QPushButton::clicked, this,
            [this]() { executeCommand(QStringLiteral("stop")); });
    auto *restartBtn = new QPushButton(tr("重启服务"), commandWidget);
    connect(restartBtn, &QPushButton::clicked, this,
            [this]() { executeCommand(QStringLiteral("restart")); });
    auto *psBtn = new QPushButton(tr("查看状态"), commandWidget);
    connect(psBtn, &QPushButton::clicked, this,
            [this]() { executeCommand(QStringLiteral("ps")); });
    m_logsBtn = new QPushButton(tr("查看日志"), commandWidget);
    connect(m_logsBtn, &QPushButton::clicked, this, &DockerComposeEditor::toggleLogs);
    auto *configDockerBtn = new QPushButton(tr("配置Docker"), commandWidget);
    connect(configDockerBtn, &QPushButton::clicked,
            this, &DockerComposeEditor::onConfigDockerClicked);
    buttonLayout->addWidget(upBtn);
    buttonLayout->addWidget(downBtn);
    buttonLayout->addWidget(restartBtn);
    buttonLayout->addWidget(psBtn);
    buttonLayout->addWidget(m_logsBtn);
    buttonLayout->addWidget(configDockerBtn);
    commandLayout->addLayout(buttonLayout);

    // 输出显示区域（Python 用 pygments 高亮，这里按计划简化为纯文本深色）
    m_outputText = new QTextEdit(commandWidget);
    m_outputText->setReadOnly(true);
    m_outputText->setMinimumHeight(200);
    // 设置等宽字体
    m_outputText->setFont(QFont(QStringLiteral("Courier New"), 10));
    // 设置深色背景
    m_outputText->setStyleSheet(
        QStringLiteral("background-color: #1e1e1e; color: #d4d4d4;"));
    commandLayout->addWidget(m_outputText);

    // 添加垂直分割器部件
    verticalSplitter->addWidget(commandWidget);
    verticalSplitter->setSizes({600, 200}); // 设置上下区域的比例

    // DockerManager 信号可能来自工作线程 —— 显式排队接入
    connect(m_manager, &DockerManager::composeLoaded,
            this, &DockerComposeEditor::onComposeLoaded, Qt::QueuedConnection);
    connect(m_manager, &DockerManager::composeSaved,
            this, &DockerComposeEditor::onComposeSaved, Qt::QueuedConnection);
    connect(m_manager, &DockerManager::commandOutput,
            this, &DockerComposeEditor::onCommandOutput, Qt::QueuedConnection);
    connect(m_manager, &DockerManager::errorOccurred,
            this, &DockerComposeEditor::onErrorOccurred, Qt::QueuedConnection);

    // 加载配置（异步，结果经 composeLoaded 回到 onComposeLoaded）
    // 对应Python: load_config（905-948 行，文件读写下沉到 DockerManager）
    m_manager->loadComposeFile();
}

// 对应Python: closeEvent（894-903 行）
void DockerComposeEditor::closeEvent(QCloseEvent *event)
{
    // 停止日志查看
    m_manager->stopComposeLogs();
    QWidget::closeEvent(event);
}

// 对应Python: load_config 939-946 行（yaml.safe_load + 填树 + 默认选中第一个）
void DockerComposeEditor::onComposeLoaded(const QString &yamlText)
{
    QString error;
    QVariantMap config = ComposeYaml::parseCompose(yamlText, &error);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, tr("错误"), tr("加载配置文件时出错: %1").arg(error));
        return;
    }
    if (config.isEmpty()) {
        // 对应Python: load_config 的 default_config（912-917 行）
        config.insert(QStringLiteral("version"), QStringLiteral("3.8"));
        config.insert(QStringLiteral("services"), QVariantMap());
        config.insert(QStringLiteral("volumes"), QVariantMap());
        config.insert(QStringLiteral("networks"), QVariantMap());
    }
    m_config = config;
    updateServicesTree();

    // 默认选择第一个服务
    if (m_servicesTree->topLevelItemCount() > 0) {
        QTreeWidgetItem *firstItem = m_servicesTree->topLevelItem(0);
        m_servicesTree->setCurrentItem(firstItem);
        onServiceSelected(firstItem);
    }
}

// 对应Python: update_service_config 的成功提示（1006-1010 行），
// 改为保存真正落盘（composeSaved）后弹出
void DockerComposeEditor::onComposeSaved()
{
    if (m_pendingSavedService.isEmpty())
        return;
    const QString serviceName = m_pendingSavedService;
    m_pendingSavedService.clear();
    QMessageBox::information(this, tr("成功"),
                             tr("服务 %1 的配置已更新").arg(serviceName));
}

// 对应Python: append_text（764-771 行，追加输出并滚动到底部）
void DockerComposeEditor::onCommandOutput(const QString &text)
{
    m_outputText->append(text);
    m_outputText->verticalScrollBar()->setValue(
        m_outputText->verticalScrollBar()->maximum());
}

void DockerComposeEditor::onErrorOccurred(const QString &message)
{
    onCommandOutput(tr("错误:\n%1").arg(message));
}

// 对应Python: on_service_selected（984-1004 行，替换右侧配置部件）
void DockerComposeEditor::onServiceSelected(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const QString serviceName = item->text(0);
    const QVariantMap serviceConfig =
        m_config.value(QStringLiteral("services")).toMap().value(serviceName).toMap();

    auto *newWidget = new ServiceConfigWidget(serviceName, serviceConfig, this);
    connect(newWidget, &ServiceConfigWidget::configSaved,
            this, &DockerComposeEditor::onServiceConfigSaved);

    // 替换配置部件
    m_rightLayout->replaceWidget(m_configWidget, newWidget);
    m_configWidget->deleteLater();
    m_configWidget = newWidget;
}

// 对应Python: ServiceConfigWidget.save_config 441-443 行 + update_service_config
void DockerComposeEditor::onServiceConfigSaved(const QString &serviceName,
                                               const QVariantMap &config)
{
    QVariantMap services = m_config.value(QStringLiteral("services")).toMap();
    services.insert(serviceName, config);
    m_config.insert(QStringLiteral("services"), services);
    saveConfig(serviceName);
}

// 对应Python: save_config（950-962 行，yaml.dump + SFTP 写回下沉到 DockerManager）
void DockerComposeEditor::saveConfig(const QString &savedServiceName)
{
    m_pendingSavedService = savedServiceName;
    m_manager->saveComposeFile(ComposeYaml::dumpCompose(m_config));
}

// 对应Python: update_services_tree（964-982 行）
void DockerComposeEditor::updateServicesTree()
{
    m_servicesTree->clear();
    const QVariantMap services = m_config.value(QStringLiteral("services")).toMap();
    for (auto it = services.constBegin(); it != services.constEnd(); ++it) {
        const QString serviceName = it.key();
        auto *item = new QTreeWidgetItem(QStringList{serviceName});

        // 设置图标（qrc 中无对应服务图标时回退默认 docker 图标）
        const QString iconPath = QStringLiteral(":/%1_128.png").arg(serviceName);
        if (QFile::exists(iconPath))
            item->setIcon(0, QIcon(iconPath));
        else
            item->setIcon(0, QIcon(QStringLiteral(":/icons8-docker-48.png")));

        // 设置提示信息
        const QVariantMap serviceInfo = it.value().toMap();
        QString tooltip = tr("服务: %1\n").arg(serviceName);
        if (serviceInfo.contains(QStringLiteral("image")))
            tooltip += tr("镜像: %1\n").arg(
                serviceInfo.value(QStringLiteral("image")).toString());
        const QVariantList ports = serviceInfo.value(QStringLiteral("ports")).toList();
        if (!ports.isEmpty()) {
            QStringList portTexts;
            for (const QVariant &port : ports)
                portTexts << port.toString();
            tooltip += tr("端口: %1\n").arg(portTexts.join(QStringLiteral(", ")));
        }
        item->setToolTip(0, tooltip);

        m_servicesTree->addTopLevelItem(item);
    }
}

// 对应Python: add_service 1094-1099 行的选中循环
void DockerComposeEditor::selectServiceByName(const QString &serviceName)
{
    for (int i = 0; i < m_servicesTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_servicesTree->topLevelItem(i);
        if (item->text(0) == serviceName) {
            m_servicesTree->setCurrentItem(item);
            onServiceSelected(item);
            break;
        }
    }
}

// 对应Python: execute_command（773-825 行，命令构造/sudo/线程读取均在 DockerManager）
void DockerComposeEditor::executeCommand(const QString &subcommand)
{
    // 清空输出区域
    m_outputText->clear();
    m_manager->composeCommand(subcommand);
}

// 对应Python: toggle_logs（888-892 行）+ start_logs/stop_logs（下沉到 DockerManager）
void DockerComposeEditor::toggleLogs()
{
    if (m_manager->isStreamingLogs()) {
        m_manager->stopComposeLogs();
        m_logsBtn->setText(tr("查看日志"));
    } else {
        m_outputText->clear();
        m_manager->startComposeLogs();
        m_logsBtn->setText(tr("停止日志"));
    }
}

// 对应Python: on_config_docker_clicked（1012-1085 行，应用流程下沉到 DockerManager）
void DockerComposeEditor::onConfigDockerClicked()
{
    DockerDaemonConfigDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString configText = dialog.configText();
    if (!DockerManager::validateDaemonJson(configText)) {
        QMessageBox::warning(this, tr("警告"), tr("配置内容为空或格式错误"));
        return;
    }
    // 清空输出区域，进度经 commandOutput 送达
    m_outputText->clear();
    m_manager->applyDaemonConfig(configText);
}

// 对应Python: add_service（1087-1099 行）
void DockerComposeEditor::addService()
{
    ServiceSearchDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString serviceName = dialog.selectedServiceName();
    if (serviceName.isEmpty())
        return;
    QVariantMap services = m_config.value(QStringLiteral("services")).toMap();
    services.insert(serviceName, dialog.selectedServiceConfig());
    m_config.insert(QStringLiteral("services"), services);
    saveConfig(QString()); // 静默保存（不弹"已更新"提示）
    updateServicesTree();
    // 选择新添加的服务
    selectServiceByName(serviceName);
}

} // namespace cubeshell
