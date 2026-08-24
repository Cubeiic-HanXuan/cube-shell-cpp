#pragma once

// LinuxCommandsDialog.h — Linux 常用命令查询对话框。
// 对应Python: core/frequently_used_commands.py::TreeSearchApp
//           + cube-shell.py::linux（帮助菜单 "Linux常用命令" 入口）
//
// 数据层为 core/config/FrequentlyUsedCommands（conf/linux_commands.json），
// 搜索过滤复用其 filter()（与 Python TreeFilterProxyModel 同语义）；点击
// 命令行即把 "命令 选项" 复制到剪贴板。

#include <QDialog>
#include <QList>

#include "config/FrequentlyUsedCommands.h"

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace cubeshell {

class LinuxCommandsDialog : public QDialog {
    Q_OBJECT
public:
    // jsonPath 为空时走 FrequentlyUsedCommands::defaultPath() 探测。
    explicit LinuxCommandsDialog(QWidget *parent = nullptr,
                                 const QString &jsonPath = QString());

private slots:
    void onSearchChanged(const QString &text);
    void onItemClicked(QTreeWidgetItem *item, int column);

private:
    void rebuildTree(const QString &searchText);
    void addEntries(QTreeWidgetItem *parent, const QList<CommandEntry> &entries);

    FrequentlyUsedCommands m_commands;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace cubeshell
