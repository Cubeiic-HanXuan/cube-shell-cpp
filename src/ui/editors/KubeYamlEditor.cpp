// KubeYamlEditor.cpp — see KubeYamlEditor.h.

#include "KubeYamlEditor.h"

#include "editors/CodeEditor.h"

#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

KubeYamlEditor::KubeYamlEditor(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Kubernetes YAML"));
    setMinimumSize(760, 560);
    setModal(false);

    auto *layout = new QVBoxLayout(this);
    m_editor = new CodeEditor(this);
    layout->addWidget(m_editor);

    auto *bottom = new QHBoxLayout();
    m_applyBtn = new QPushButton(tr("应用 (kubectl apply)"), this);
    auto *closeBtn = new QPushButton(tr("关闭"), this);
    bottom->addStretch();
    bottom->addWidget(m_applyBtn);
    bottom->addWidget(closeBtn);
    layout->addLayout(bottom);

    connect(m_applyBtn, &QPushButton::clicked, this, [this]() {
        if (m_editor->toPlainText().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("应用 YAML"), tr("内容为空，无法应用。"));
            return;
        }
        emit applyRequested(m_editor->toPlainText());
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

void KubeYamlEditor::showYaml(const QString &title, const QString &yamlText, bool editable)
{
    setWindowTitle(title);
    m_editable = editable;
    m_editor->setPlainText(yamlText);
    m_editor->setReadOnly(!editable);
    m_applyBtn->setVisible(editable);
    show();
    raise();
    activateWindow();
}

} // namespace cubeshell
