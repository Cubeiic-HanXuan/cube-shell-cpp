#pragma once

// LockTabDialog.h — 锁定标签页对话框（设置密码 + 选项）。

#include <QDialog>

class QCheckBox;
class QLineEdit;

namespace cubeshell {

class LockTabDialog : public QDialog {
    Q_OBJECT
public:
    explicit LockTabDialog(QWidget *parent = nullptr);

    QString password() const;
    bool lockAllTabs() const;
    bool hideOutput() const;

    void accept() override;

private:
    QLineEdit *m_password = nullptr;
    QLineEdit *m_confirmPassword = nullptr;
    QCheckBox *m_lockAllTabs = nullptr;
    QCheckBox *m_hideOutput = nullptr;
};

} // namespace cubeshell
