#include "LockTabDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace cubeshell {

LockTabDialog::LockTabDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("锁定标签页"));
    setMinimumWidth(320);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("输入密码"));

    m_confirmPassword = new QLineEdit(this);
    m_confirmPassword->setEchoMode(QLineEdit::Password);
    m_confirmPassword->setPlaceholderText(tr("再次输入密码"));

    m_lockAllTabs = new QCheckBox(tr("锁定所有标签页"), this);
    m_hideOutput = new QCheckBox(tr("隐藏输出"), this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *form = new QFormLayout;
    form->addRow(tr("输入密码："), m_password);
    form->addRow(tr("再次输入密码："), m_confirmPassword);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_lockAllTabs);
    layout->addWidget(m_hideOutput);
    layout->addSpacing(10);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &LockTabDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_password->setFocus();
}

QString LockTabDialog::password() const { return m_password->text(); }
bool LockTabDialog::lockAllTabs() const { return m_lockAllTabs->isChecked(); }
bool LockTabDialog::hideOutput() const { return m_hideOutput->isChecked(); }

void LockTabDialog::accept()
{
    if (m_password->text().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("密码不能为空。"));
        return;
    }
    if (m_password->text() != m_confirmPassword->text()) {
        QMessageBox::warning(this, windowTitle(), tr("两次输入的密码不一致。"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
