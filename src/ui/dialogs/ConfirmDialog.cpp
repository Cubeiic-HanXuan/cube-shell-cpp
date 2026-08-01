#include "ConfirmDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: ui/confirm.py::Ui_confirm.setupUi
ConfirmDialog::ConfirmDialog(const QString &message, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("确认"));
    setMinimumWidth(300);

    m_label = new QLabel(message, this);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::PlainText);

    m_accept = new QPushButton(tr("确定"), this);
    m_reject = new QPushButton(tr("取消"), this);
    m_accept->setCursor(Qt::PointingHandCursor);
    m_reject->setCursor(Qt::PointingHandCursor);
    m_accept->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_accept);
    buttons->addWidget(m_reject);
    buttons->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_label, 1);
    layout->addLayout(buttons);

    connect(m_accept, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_reject, &QPushButton::clicked, this, &QDialog::reject);
}

void ConfirmDialog::setMessage(const QString &message)
{
    m_label->setText(message);
}

void ConfirmDialog::setButtonTexts(const QString &acceptText, const QString &rejectText)
{
    m_accept->setText(acceptText);
    m_reject->setText(rejectText);
}

bool ConfirmDialog::confirm(QWidget *parent, const QString &title, const QString &message)
{
    ConfirmDialog dlg(message, parent);
    dlg.setWindowTitle(title);
    return dlg.exec() == QDialog::Accepted;
}

} // namespace cubeshell
