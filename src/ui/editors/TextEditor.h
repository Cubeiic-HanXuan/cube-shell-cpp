#pragma once

// TextEditor.h — 纯文本编辑器窗口：CodeEditor（行号/高亮）+ 保存 + 查找/替换工具条。
// 对应Python: ui/text_editor.py::Ui_MainWindow + cube-shell.py 中远程文件编辑逻辑

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolBar;

namespace cubeshell {

class CodeEditor;

// 对应Python: ui/text_editor.py::Ui_MainWindow
class TextEditor : public QWidget {
    Q_OBJECT
public:
    explicit TextEditor(QWidget *parent = nullptr);

    void setPlainText(const QString &text);
    QString toPlainText() const;

    // 编辑目标显示名（窗口标题，如远程文件路径）
    void setFileLabel(const QString &label);

    CodeEditor *editor() const { return m_editor; }
    bool isModified() const;

signals:
    // 用户按 Ctrl+S / 点保存时发射，由持有者负责实际写回（本地或 SFTP）。
    void saveRequested(const QString &content);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSave();
    void onFindNext();
    void onFindPrev();
    void onReplace();
    void onReplaceAll();
    void toggleSearchBar();

private:
    CodeEditor *m_editor = nullptr;
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QCheckBox *m_regexCheck = nullptr;
    QCheckBox *m_caseCheck = nullptr;
    QLabel *m_status = nullptr;
    QString m_fileLabel;
};

} // namespace cubeshell
