#pragma once

// DockerComposeEditor.h — Docker Compose 可视化编辑器（独立窗口）。
// 对应Python: core/docker/docker_compose_editor.py:592-1100 (DockerComposeEditor)
//
// 布局：垂直 QSplitter(600/200) → 上部水平 QSplitter(300/900)
//   左：服务列表（QTreeWidget）+ 添加服务按钮
//   右：ServiceConfigWidget（选中服务的配置表单）
//   下：compose 命令按钮行 + 只读输出区
//
// 所有远端操作走 DockerManager（调用方保证已 setRemoteExecutor/setRemoteUser），
// 其信号可能来自工作线程 —— 本类全部以 Qt::QueuedConnection 接入。
// YAML 解析/序列化走 ComposeYaml（对应 Python 的 yaml.safe_load/yaml.dump）。
//
// 视觉简化（唯一允许项）：Python 用 pygments 高亮命令输出，C++ 侧输出区为
// 纯文本深色样式（background #1e1e1e / color #d4d4d4）。

#include <QVariantMap>
#include <QWidget>

class QPushButton;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace cubeshell {

class DockerManager;
class ServiceConfigWidget;

class DockerComposeEditor : public QWidget {
    Q_OBJECT
public:
    // manager 不被本窗口持有，须比窗口存活更久。
    explicit DockerComposeEditor(DockerManager *manager, QWidget *parent = nullptr);

protected:
    // 关闭时停止日志流。对应Python: closeEvent（894-903 行）
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onComposeLoaded(const QString &yamlText); // 对应Python: load_config
    void onComposeSaved();                         // 保存成功后的"配置已更新"提示
    void onCommandOutput(const QString &text);     // 对应Python: append_text
    void onErrorOccurred(const QString &message);
    void onServiceSelected(QTreeWidgetItem *item); // 对应Python: on_service_selected
    void onServiceConfigSaved(const QString &serviceName, const QVariantMap &config);
    void toggleLogs();                             // 对应Python: toggle_logs
    void onConfigDockerClicked();                  // 对应Python: on_config_docker_clicked
    void addService();                             // 对应Python: add_service

private:
    void executeCommand(const QString &subcommand); // 对应Python: execute_command
    void updateServicesTree();                      // 对应Python: update_services_tree
    void selectServiceByName(const QString &serviceName);
    void saveConfig(const QString &savedServiceName); // 对应Python: save_config

    DockerManager *m_manager;          // not owned
    QVariantMap m_config;              // 对应Python: self.config（compose 全文）

    QTreeWidget *m_servicesTree = nullptr;
    QWidget *m_configWidget = nullptr; // 右侧当前配置部件（初始为占位 QWidget）
    QVBoxLayout *m_rightLayout = nullptr;
    QPushButton *m_logsBtn = nullptr;
    QTextEdit *m_outputText = nullptr;
    QString m_pendingSavedService;     // 保存中待提示的服务名（空则静默保存）
};

} // namespace cubeshell
