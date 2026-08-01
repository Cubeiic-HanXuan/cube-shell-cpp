#pragma once

// ConfirmDialog.h — 通用确认对话框（消息 + 确认/取消按钮）。
// 对应Python: ui/confirm.py::Ui_confirm（"改动未保存，是否放弃修改？"等场景）

#include <QDialog>

class QLabel;
class QPushButton;

namespace cubeshell {

class ConfirmDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConfirmDialog(const QString &message = QString(), QWidget *parent = nullptr);

    void setMessage(const QString &message);
    // 自定义按钮文案（默认 "确定"/"取消"，对应 Python 的 "保存并退出"/"放弃保存"）。
    void setButtonTexts(const QString &acceptText, const QString &rejectText);

    // 便捷静态入口：返回 true 表示用户点了确认。
    // 对应Python: cube-shell.py 中 confirm 弹窗的 exec + 结果判定
    static bool confirm(QWidget *parent, const QString &title, const QString &message);

private:
    QLabel *m_label = nullptr;
    QPushButton *m_accept = nullptr;
    QPushButton *m_reject = nullptr;
};

} // namespace cubeshell
