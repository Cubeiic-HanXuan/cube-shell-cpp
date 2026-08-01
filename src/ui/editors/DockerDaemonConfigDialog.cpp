// DockerDaemonConfigDialog.cpp — see DockerDaemonConfigDialog.h.
// 对应Python: core/docker/docker_compose_editor.py:530-589 (DockerDaemonConfigDialog)

#include "DockerDaemonConfigDialog.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "docker/DockerManager.h"

namespace cubeshell {

DockerDaemonConfigDialog::DockerDaemonConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Docker守护进程配置"));
    setMinimumWidth(600);
    setMinimumHeight(400);

    // 创建主布局
    auto *layout = new QVBoxLayout(this);

    // 创建配置编辑器
    m_editor = new QTextEdit(this);
    m_editor->setFont(QFont(QStringLiteral("Courier New"), 10));
    m_editor->setPlaceholderText(tr("请输入daemon.json的配置内容..."));

    // 添加默认配置（registry-mirrors）。对应Python: 548-553 行
    m_editor->setText(DockerManager::defaultDaemonJson());

    // 创建按钮
    auto *buttonLayout = new QHBoxLayout();

    auto *validateButton = new QPushButton(tr("验证配置"), this);
    connect(validateButton, &QPushButton::clicked,
            this, &DockerDaemonConfigDialog::validateConfig);

    auto *applyButton = new QPushButton(tr("应用配置"), this);
    connect(applyButton, &QPushButton::clicked, this, &DockerDaemonConfigDialog::accept);

    auto *cancelButton = new QPushButton(tr("取消"), this);
    connect(cancelButton, &QPushButton::clicked, this, &DockerDaemonConfigDialog::reject);

    buttonLayout->addWidget(validateButton);
    buttonLayout->addWidget(applyButton);
    buttonLayout->addWidget(cancelButton);

    // 添加组件到主布局
    layout->addWidget(new QLabel(tr("Docker守护进程配置 (daemon.json):"), this));
    layout->addWidget(m_editor);
    layout->addLayout(buttonLayout);
}

// 对应Python: validate_config（576-582 行）
void DockerDaemonConfigDialog::validateConfig()
{
    QString error;
    if (DockerManager::validateDaemonJson(m_editor->toPlainText(), &error))
        QMessageBox::information(this, tr("验证成功"), tr("配置格式正确！"));
    else
        QMessageBox::warning(this, tr("验证失败"), tr("配置格式错误：%1").arg(error));
}

QString DockerDaemonConfigDialog::configText() const
{
    return m_editor->toPlainText();
}

} // namespace cubeshell
