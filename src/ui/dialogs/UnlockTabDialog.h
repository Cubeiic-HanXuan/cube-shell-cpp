#pragma once

// UnlockTabDialog.h — 解锁标签页对话框（输入密码 + 选项）。

#include <QDialog>

class QCheckBox;
class QLineEdit;

namespace cubeshell {

class UnlockTabDialog : public QDialog {
    Q_OBJECT
public:
    explicit UnlockTabDialog(QWidget *parent = nullptr);

    QString password() const;
    bool unlockAllTabs() const;

    void accept() override;

private:
    QLineEdit *m_password = nullptr;
    QCheckBox *m_unlockAllTabs = nullptr;
};

} // namespace cubeshell
