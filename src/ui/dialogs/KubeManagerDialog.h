#pragma once

// KubeManagerDialog.h — Kubernetes 集群管理对话框。
//
// 对应 docs/Kubernetes功能实现方案.md §5.1：
//   顶部工具条（后端 / 上下文 / 命名空间 / 刷新）+ 三级资源树
//   （分组 → 资源类型 → 对象行）+ 按 kind 动态构建的右键菜单。
//
// 架构对齐 ui/dialogs/DockerManagerDialog：
//   - manager 不被本对话框持有，须比对话框存活更久；
//   - KubeManager 的信号可能来自工作线程 —— 全部以 Qt::QueuedConnection 接入；
//   - 刷新中忽略并发刷新（m_refreshing 守卫）；无数据/无 kubectl 显示占位行。
//
// Pod exec / docker logs 式交互命令经 terminalCommandRequested 转发到当前
// 终端标签页（命令字符串由主窗口送进 QTermWidget，同 Docker 对话框）。

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "kube/KubeManager.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class KubeLogViewer;
class KubeYamlEditor;

class KubeManagerDialog : public QDialog {
    Q_OBJECT
public:
    // manager 不被本对话框持有，须比对话框存活更久。
    explicit KubeManagerDialog(KubeManager *manager, QWidget *parent = nullptr);

    // 主窗口在显示前按当前环境设置远程后端可用性与标签
    //（"当前 SSH 会话 (user@host)"；无活动会话时移除该项）。
    void setRemoteBackendAvailable(bool available, const QString &label);
    // 主窗口换完 executor 后回调，同步下拉选中态。
    void setBackendSelection(bool useRemote);
    // 显示当前 kubeconfig 路径（按钮 tooltip）；空表示 kubectl 默认解析。
    void setKubeconfigPathDisplay(const QString &path);

public slots:
    // 重新走 探测→上下文→命名空间→全量资源 流程（刷新按钮与主窗口调用）。
    void refreshInfo();

signals:
    // 请求在当前终端中执行命令（kubectl exec），由主窗口转发。
    void terminalCommandRequested(const QString &command);
    // 用户切换后端下拉；主窗口负责换 executor 后再调 refreshInfo()。
    void backendChangeRequested(bool useRemote);
    // 用户选定/输入了 kubeconfig（空串 = 恢复默认解析）；按当前后端分别持久化。
    void kubeconfigSelected(const QString &path);

private slots:
    void onAvailabilityReady(bool available, const QString &versionText);
    void onContextsUpdated(const QList<cubeshell::KubeContextInfo> &contexts);
    void onNamespacesUpdated(const QStringList &namespaces);
    void onResourcesUpdated(const QString &group, const QString &apiPlural,
                            const QList<cubeshell::KubeResourceRow> &rows);
    void onRefreshFinished();
    void onYamlReady(const cubeshell::KubeObjectRef &ref, const QString &yamlText);
    void onApplyFinished(bool success, const QString &message);
    void onOperationFinished(bool success, const QString &op,
                             const cubeshell::KubeObjectRef &ref,
                             const QString &message);
    void onTextReady(const QString &title, const QString &text);
    void onErrorOccurred(const QString &message);
    void onTreeContextMenu(const QPoint &position);
    // kubeconfig 按钮：按当前后端弹菜单（选择/输入 + 恢复默认）。
    void onKubeconfigButtonClicked();

private:
    void showPlaceholderRow(const QString &text);
    void showLoadingRow();
    void rebuildTree();
    void addRowItem(const cubeshell::KubeResourceRow &row,
                    const QString &apiPlural, QTreeWidgetItem *parentItem);
    // 当前选中对象行的 ref；未选中对象返回 false。
    bool selectedRef(cubeshell::KubeObjectRef *refOut) const;
    // 行的"详情"列文本（extra 按 key 排序拼接；events 特化）。
    static QString detailsText(const QString &apiPlural,
                               const cubeshell::KubeResourceRow &row);

    void showLogsForPod(const cubeshell::KubeObjectRef &pod, const QString &container);
    void execIntoPod(const cubeshell::KubeObjectRef &pod, const QString &container);
    void showPortForwardDialog(const cubeshell::KubeObjectRef &target);
    KubeLogViewer *logViewer();    // 懒建
    KubeYamlEditor *yamlEditor();  // 懒建

    KubeManager *m_manager;        // not owned
    QComboBox *m_backendCombo = nullptr;
    QComboBox *m_contextCombo = nullptr;
    QComboBox *m_namespaceCombo = nullptr;
    QPushButton *m_kubeconfigBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTreeWidget *m_tree = nullptr; // objectName "treeWidgetKube"

    // 资源数据：apiPlural → 行集（resourcesUpdated 逐类到达，累积成树）。
    QHash<QString, QList<cubeshell::KubeResourceRow>> m_rowsByKind;
    bool m_refreshing = false;     // 刷新中忽略并发刷新（Docker 对话框同款守卫）
    bool m_firstBuildDone = false; // 首轮建树 expandAll，之后保留用户展开态
    bool m_yamlEditRequested = false; // fetchYaml 的意图：查看 or 编辑并应用
    bool m_backendIsRemote = false;  // 当前生效后端（kubeconfig 按钮按此分流）
    QString m_backendLabelRemote;  // 远程后端项文本（含会话标识）

    KubeLogViewer *m_logViewer = nullptr;   // 懒建
    KubeYamlEditor *m_yamlEditor = nullptr; // 懒建
};

} // namespace cubeshell
