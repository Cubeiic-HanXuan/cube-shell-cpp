#pragma once

// DockerManagerDialog.h — Docker 容器管理对话框（树形分组 + 右键菜单）。
// 对应Python: cube-shell.py:1007-1045 _ensure_docker_manager_dialog（窗口/树/刷新按钮）
//           + cube-shell.py:3705-3765 treeDocker（右键菜单 停止/重启/删除/终端/日志）
//           + cube-shell.py:3767-3785 execDockerTerminal / viewDockerLogs
//           + cube-shell.py:4693-4804 refreshDokerInfo / update_docker_ui / _add_container_item
//           + cube-shell.py:5221-5362 _start_docker_operation / _mark_containers_operating /
//             _update_container_info_in_tree / _remove_containers_from_tree /
//             _on_docker_operation_finished
//
// 后端为 core/docker/DockerManager：分组容器列表通过 groupedContainersUpdated()
// 送达，容器操作结果通过 containerOperationFinished() 送达。
// DockerManager 的信号可能来自工作线程 —— 全部以 Qt::QueuedConnection 接入。

#include <QDialog>
#include <QList>
#include <QStringList>

#include "docker/DockerManager.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class DockerManagerDialog : public QDialog {
    Q_OBJECT
public:
    // manager 不被本对话框持有，须比对话框存活更久。
    explicit DockerManagerDialog(DockerManager *manager, QWidget *parent = nullptr);

public slots:
    // 重新拉取分组容器列表（供刷新按钮与主窗口调用）。
    // 对应Python: cube-shell.py:4693 refreshDokerInfo
    void refreshInfo();

signals:
    // 请求在当前 SSH 终端中执行命令（docker exec / docker logs），由主窗口转发。
    // 对应Python: cube-shell.py:3767-3785 的 terminal.sendText
    void terminalCommandRequested(const QString &command);

private slots:
    // 对应Python: cube-shell.py:4748 update_docker_ui
    void onGroupedContainersUpdated(const QList<cubeshell::DockerComposeGroup> &groups);
    // 对应Python: cube-shell.py:5340 _on_docker_operation_finished
    void onContainerOperationFinished(bool success, const QString &op,
                                      const cubeshell::DockerStatePortsMap &idToStatePorts);
    // 对应Python: cube-shell.py:3705 treeDocker
    void onTreeContextMenu(const QPoint &position);

private:
    // 清树并放置单行提示（"没有可用的docker容器" 等占位行）。
    void showPlaceholderRow(const QString &text);
    // 对应Python: cube-shell.py:4786 _add_container_item
    void addContainerItem(const DockerContainerRow &row, QTreeWidgetItem *parentItem);
    // 对应Python: cube-shell.py:5241 _start_docker_operation
    void startDockerOperation(const QString &op, const QStringList &ids);
    // 对应Python: cube-shell.py:5259 _mark_containers_operating
    void markContainersOperating(const QStringList &ids, const QString &statusText,
                                 bool highlight = true);
    // 对应Python: cube-shell.py:5278 _update_container_info_in_tree
    void updateContainerInfoInTree(const DockerStatePortsMap &idToStatePorts);
    // 对应Python: cube-shell.py:5307 _remove_containers_from_tree
    void removeContainersFromTree(const QStringList &ids);

    DockerManager *m_manager;      // not owned
    QTreeWidget *m_tree = nullptr; // objectName "treeWidgetDocker"
    // 刷新中忽略并发刷新。对应Python: cube-shell.py:4738 docker_thread.isRunning() 守卫
    bool m_refreshing = false;
    // 最近一次操作的容器ID：失败时 manager 发来的 map 为空，
    // 用它把状态列标回"操作失败"。对应Python: cube-shell.py:5359-5361
    QStringList m_lastOperationIds;
};

} // namespace cubeshell
