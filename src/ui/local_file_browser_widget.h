#pragma once

// local_file_browser_widget.h — 本机终端左侧的本地文件目录面板。
// 对应Python: cube-shell.py 本机终端 (is_local) 分支的文件树
//（refreshDirs → _get_local_dir_now 本地目录列表）
// 外观与 SftpBrowserWidget 统一：路径栏 + 五列平铺列表（".." 行返回上级）。

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;
class QToolButton;
class QEvent;

namespace cubeshell {

class LocalFileBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit LocalFileBrowserWidget(QWidget *parent = nullptr);

    // 切换显示的根目录（follow_folder 联动 / 双击进入子目录）。
    void setRootPath(const QString &path);
    QString rootPath() const { return m_rootPath; }

    // 在路径栏左侧显示/隐藏分屏序号徽章（与 SftpBrowserWidget 同款）。
    // paneNumber: 当前分屏序号（1-based）；totalPanes: 总分屏数（≤1 时隐藏徽章）；
    // tabTitle: 用于 tooltip 的标签名，展示完整的“分屏 N · 标签名”。
    void setPaneIndicator(int paneNumber, int totalPanes, const QString &tabTitle);

signals:
    // “新建位于文件夹位置的终端窗口”：由外层（main_window）接线打开新本机终端。
    // 对应Python: cube-shell.py::open_local_terminal_in_selected_folder
    void newTerminalRequested(const QString &dir);

private slots:
    void goUp();
    void goHome();
    void onPathEdited();
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    // 表头点击排序：同列切换升降序，换列重置为升序（与 SFTP 面板一致）。
    void onHeaderSectionClicked(int logical);
    // 筛选栏显隐（路径栏右侧筛选按钮 toggled）。打开即聚焦，关闭即清空还原。
    void setFilterBarVisible(bool on);
    // 按 m_filterEdit 当前文本重筛整棵树（textChanged 驱动）。
    void applyFilter();
    // 检索框回车：选中并滚动到第一个可见匹配项。
    void focusFirstFilterMatch();
    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接 + is_local 分支）
    void showContextMenu(const QPoint &pos);
    void downloadSelected();      // 对应Python: downloadFile（本机=复制到目标目录）
    void uploadFiles();           // 对应Python: uploadFile（本机=复制进当前目录）
    void editSelected();          // 对应Python: editFile（is_local 分支）
    void createDirHere();         // 对应Python: createDir
    void createFileHere();        // 对应Python: createFile
    void refresh();               // 对应Python: refresh → refreshDirs
#ifdef CUBESHELL_WITH_LOCALPTY
    void openTerminalHere();      // 对应Python: open_local_terminal_in_selected_folder
#endif
#ifndef CUBESHELL_PLATFORM_OHOS
    void showInFileManager();     // 对应Python: show_file_in_explorer
#endif
    void showPermissions();       // 对应Python: show_auth + Auth.ok_auth（os.chmod）
    void removeSelected();        // 对应Python: remove
    void renameSelected();        // 对应Python: rename（os.rename）
#ifndef CUBESHELL_PLATFORM_OHOS
    void decompressSelected();    // 对应Python: unzip → DecompressThread（is_local）
    void compressSelected();      // 对应Python: zip → CompressThread（is_local）
#endif

private:
    void populate();
    // 选中项的绝对路径（排除 ".." 行）。
    QString selectedPath() const;
    QStringList selectedPaths() const;
    // 检索框里的 Esc（QLineEdit 自身不消化 Esc）。
    bool eventFilter(QObject *obj, QEvent *event) override;

    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_paneBadge = nullptr;     // 路径栏左侧的分屏序号圆角徽章
    QToolButton *m_filterBtn = nullptr; // 路径栏右侧的筛选开关（弹出/收起检索框）
    QWidget *m_filterBar = nullptr;    // 检索框所在行（默认隐藏）
    QLineEdit *m_filterEdit = nullptr;
    QString m_rootPath;
    // 表头排序状态（跨目录保持；默认按文件名升序，与原 ls 风格预排序一致）。
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};

} // namespace cubeshell
