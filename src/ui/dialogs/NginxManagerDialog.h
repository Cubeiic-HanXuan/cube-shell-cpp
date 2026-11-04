#pragma once

// NginxManagerDialog.h — Nginx 站点管理对话框。
//
// 见 docs/Nginx可视化管理实现方案.md §5.1：工具栏（后端 / 状态 / 刷新 / 新建 /
// 校验 / 重载 / 日志）+ 分组站点树（已启用 / 未启用）+ 右键菜单。双击行打开
// NginxSiteEditor 编辑。
//
// 架构对齐 ui/dialogs/KubeManagerDialog：manager 不被本对话框持有；NginxManager
// 的信号可能来自工作线程 —— 全部以 Qt::QueuedConnection 接入；刷新中忽略并发刷新。

#include <QDialog>
#include <QList>
#include <QString>

#include "nginx/NginxConfig.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class NginxLogViewer;
class NginxManager;
class NginxSiteEditor;

class NginxManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit NginxManagerDialog(NginxManager *manager, QWidget *parent = nullptr);

    // 主窗口在显示前按当前环境设置远程后端可用性与标签。
    void setRemoteBackendAvailable(bool available, const QString &label);
    void setBackendSelection(bool useRemote);
    void setConfDirDisplay(const QString &path);

public slots:
    void refreshInfo();

signals:
    // systemctl status nginx / tail -f 等命令转发到当前终端标签页。
    void terminalCommandRequested(const QString &command);
    void backendChangeRequested(bool useRemote);
    // 用户设定配置目录；主窗口负责持久化 + 立即生效（重新刷新）。
    void confDirSelected(const QString &path);
    // 用户设定 nginx 可执行文件路径（空串 = 恢复自动探测）。
    void nginxBinarySelected(const QString &path);

private slots:
    void onAvailabilityReady(bool available, const QString &versionText);
    void onServiceStatusReady(bool running, const QString &detail);
    void onSitesUpdated(const QList<cubeshell::nginx::SiteView> &sites);
    void onFileContentReady(const QString &path, const QString &text);
    void onFileSaved(const QString &path);
    void onTestResultReady(bool ok, const QString &output);
    void onReloadFinished(bool ok, const QString &message);
    void onOperationFinished(bool ok, const QString &op, const QString &message);
    void onErrorOccurred(const QString &message);

    void onBackendComboChanged();
    void onTreeContextMenu(const QPoint &position);
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onNewSite();
    void onTestConfig();
    void onReload();
    void onViewLogs();
    void onConfDirButtonClicked();
    void onNginxBinButtonClicked();
    void onStartService();
    void onStopService();
    void onRestartService();
    void onEditorSaveRequested(const QString &path, const QString &text);

private:
    void showPlaceholderRow(const QString &text);
    void rebuildTree();
    const nginx::SiteView *selectedSite() const;
    void beginEdit(const QString &path, bool isNew);
    void openEditor(const QString &path, const QString &text, bool isNew);
    void deleteSitePrompt(const nginx::SiteView &site);
    void toggleEnabled(const nginx::SiteView &site);
    void showLogWindow(const QString &kind, bool follow);
    NginxSiteEditor *editor();

    NginxManager *m_manager; // not owned
    QComboBox *m_backendCombo = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_confDirLabel = nullptr;
    QPushButton *m_confDirBtn = nullptr;
    QPushButton *m_testBtn = nullptr;
    QPushButton *m_reloadBtn = nullptr;
    QPushButton *m_binBtn = nullptr;
    QTreeWidget *m_tree = nullptr; // objectName "treeWidgetNginx"

    QList<nginx::SiteView> m_sites;
    bool m_refreshing = false;
    bool m_backendIsRemote = false;
    QString m_backendLabelRemote;
    QString m_versionText;
    QString m_serviceText;

    // 编辑/保存事务状态。
    QString m_pendingEditPath;   // 正在读取以打开编辑器的路径
    QString m_preSaveText;       // 保存前的内容（回滚用）
    bool m_pendingEditIsNew = false;
    QString m_pendingSavePath;
    bool m_saving = false;

    NginxSiteEditor *m_editor = nullptr;   // 懒建
    NginxLogViewer *m_logViewer = nullptr; // 懒建
};

} // namespace cubeshell
