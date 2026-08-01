#pragma once

// DockerSoftDialog.h — 常用容器安装对话框（卡片网格）。
// 对应Python: cube-shell.py:1047-1077 _ensure_docker_soft_dialog / showDockerSoftDialog
//           + cube-shell.py:4807-4908 refresh_docker_common_containers /
//             update_common_containers_ui
//           + cube-shell.py:6396-6458 CustomWidget（95x143 卡片、安装/已安装按钮）
//
// 数据来源 DockerManager::checkCommonContainers（对应 CommonContainersThread），
// commonContainersReady() 可能来自工作线程 —— 以 Qt::QueuedConnection 接入。
// "安装"按钮打开 DockerComposeEditor（复用同一 DockerManager），对应Python:
// CustomWidget.container_orchestration 每次点击新建一个编辑器窗口。

#include <QDialog>
#include <QList>

#include "docker/DockerManager.h" // CommonServiceInfo（moc 需要完整类型）

class QGridLayout;

namespace cubeshell {

class DockerSoftDialog : public QDialog {
    Q_OBJECT
public:
    // manager 不被本对话框持有，须比对话框存活更久。
    explicit DockerSoftDialog(DockerManager *manager, QWidget *parent = nullptr);

public slots:
    // 对应Python: cube-shell.py:4807 refresh_docker_common_containers
    void refreshInfo();

private slots:
    // 对应Python: cube-shell.py:4840 update_common_containers_ui
    void onCommonContainersReady(const QList<cubeshell::CommonServiceInfo> &services,
                                 bool hasDocker);
    // 对应Python: cube-shell.py:6456 CustomWidget.container_orchestration
    void openComposeEditor();

private:
    // 对应Python: util.clear_grid_layout(self.ui.gridLayout_7)
    void clearGrid();
    // 对应Python: cube-shell.py:6396-6454 CustomWidget（图标 + 安装/已安装按钮）
    QWidget *createCard(const CommonServiceInfo &service);

    DockerManager *m_manager;      // not owned
    QGridLayout *m_grid = nullptr; // 对应Python: self.ui.gridLayout_7
};

} // namespace cubeshell
