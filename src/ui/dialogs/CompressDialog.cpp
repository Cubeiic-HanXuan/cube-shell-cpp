#include "CompressDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: ui/compress_dialog.py::CompressDialog.__init__
CompressDialog::CompressDialog(QWidget *parent, const QString &defaultName)
    : QDialog(parent)
{
    setWindowTitle(tr("新建压缩"));
    setMinimumWidth(240);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_nameEdit = new QLineEdit(defaultName, this);
    form->addRow(tr("文件名:"), m_nameEdit);

    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItems({QStringLiteral(".tar.gz"), QStringLiteral(".zip")});
    m_formatCombo->setMinimumWidth(100);
    form->addRow(tr("格式:"), m_formatCombo);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString CompressDialog::archiveName() const
{
    return m_nameEdit->text().trimmed();
}

QString CompressDialog::format() const
{
    return m_formatCombo->currentText();
}

QString CompressDialog::fileName() const
{
    return archiveName() + format();
}

} // namespace cubeshell
