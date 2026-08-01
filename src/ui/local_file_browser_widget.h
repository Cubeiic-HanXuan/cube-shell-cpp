#pragma once

// local_file_browser_widget.h — 本机终端左侧的本地文件目录面板。
// 对应Python: cube-shell.py 本机终端 (is_local) 分支的文件树
//（refreshDirs → _get_local_dir_now 本地目录列表）
// 外观与 SftpBrowserWidget 统一：路径栏 + 五列平铺列表（".." 行返回上级）。

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;

namespace cubeshell {

class LocalFileBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit LocalFileBrowserWidget(QWidget *parent = nullptr);

    // 切换显示的根目录（follow_folder 联动 / 双击进入子目录）。
    void setRootPath(const QString &path);
    QString rootPath() const { return m_rootPath; }

signals:
    // “新建位于文件夹位置的终端窗口”：由外层（main_window）接线打开新本机终端。
    // 对应Python: cube-shell.py::open_local_terminal_in_selected_folder
    void newTerminalRequested(const QString &dir);

private slots:
    void goUp();
    void goHome();
    void onPathEdited();
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    // 右键菜单。对应Python: cube-shell.py::treeRight（已连接 + is_local 分支）
    void showContextMenu(const QPoint &pos);
    void downloadSelected();      // 对应Python: downloadFile（本机=复制到目标目录）
    void uploadFiles();           // 对应Python: uploadFile（本机=复制进当前目录）
    void editSelected();          // 对应Python: editFile（is_local 分支）
    void createDirHere();         // 对应Python: createDir
    void createFileHere();        // 对应Python: createFile
    void refresh();               // 对应Python: refresh → refreshDirs
    void openTerminalHere();      // 对应Python: open_local_terminal_in_selected_folder
    void showInFileManager();     // 对应Python: show_file_in_explorer
    void showPermissions();       // 对应Python: show_auth + Auth.ok_auth（os.chmod）
    void removeSelected();        // 对应Python: remove
    void renameSelected();        // 对应Python: rename（os.rename）
    void decompressSelected();    // 对应Python: unzip → DecompressThread（is_local）
    void compressSelected();      // 对应Python: zip → CompressThread（is_local）

private:
    void populate();
    // 选中项的绝对路径（排除 ".." 行）。
    QString selectedPath() const;
    QStringList selectedPaths() const;

    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QString m_rootPath;
};

} // namespace cubeshell
