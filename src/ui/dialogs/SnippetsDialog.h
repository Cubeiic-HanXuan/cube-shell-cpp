#pragma once

// SnippetsDialog.h — 用户自定义参数化片段（Snippets）管理对话框。
//
// FrequentlyUsedCommands/LinuxCommandsDialog 是只读静态命令库；本对话框是用户
// 可写的那一层：新建/编辑/删除/分组片段，片段可带 {占位参数}、可绑快捷键，
// 「下发」把片段（填好参数后）发到当前活动会话终端。
//
// 数据经 SnippetsStore 落用户配置目录 snippets.json；本对话框只管呈现与编辑，
// 真正的下发由 MainWindow 经 runSnippetRequested 信号拿回去做（需要当前终端上下文）。

#include <QDialog>

#include "config/snippets_store.h"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;

namespace cubeshell {

class SnippetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SnippetsDialog(QWidget *parent = nullptr);

    // 取当前选中片段；无选中返回 false。
    bool selectedSnippet(Snippet *out) const;

    // 参数填写小窗：body 无占位符时直接返回 true（values 不动）。
    // 有占位符则弹窗逐行填，取消返回 false。供 MainWindow 下发前调用。
    static bool promptParams(QWidget *parent, const Snippet &snippet,
                             QHash<QString, QString> *values);

signals:
    // 用户点「下发」：把选中的片段交给 MainWindow 发到当前会话。
    void runSnippetRequested(const cubeshell::Snippet &snippet);
    // 片段集合变了（新建/编辑/删除）：MainWindow 据此重建快捷键与按钮栏。
    void snippetsChanged();

private:
    void rebuildTree();
    void addSnippet();
    void editSelected();
    void removeSelected();
    // 编辑/新建共用的表单对话框；确认返回 true 并填好 s。
    bool editSnippet(Snippet *s, bool isNew);

    SnippetsStore m_store;
    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace cubeshell
