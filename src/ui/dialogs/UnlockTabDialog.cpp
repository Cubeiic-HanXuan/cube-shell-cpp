#include "UnlockTabDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

namespace cubeshell {

UnlockTabDialog::UnlockTabDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("解锁标签页"));
    setMinimumWidth(300);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("输入密码"));

    m_unlockAllTabs = new QCheckBox(tr("解锁所有标签页"), this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *form = new QFormLayout;
    form->addRow(tr("输入密码："), m_password);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_unlockAllTabs);
    layout->addSpacing(10);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &UnlockTabDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_password->setFocus();
}

QString UnlockTabDialog::password() const { return m_password->text(); }
bool UnlockTabDialog::unlockAllTabs() const { return m_unlockAllTabs->isChecked(); }

void UnlockTabDialog::accept()
{
    if (m_password->text().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("密码不能为空。"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
