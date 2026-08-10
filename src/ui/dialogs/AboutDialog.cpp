#include "AboutDialog.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: function/about.py::AboutDialog.init_ui
AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("关于 cubeShell"));
    setFixedSize(400, 360);

    auto *layout = new QVBoxLayout(this);

    // Logo（对应 Python 侧 QIcon(':docs-log.png').pixmap(160, 160)）。
    auto *logo = new QLabel(this);
    QIcon icon(QStringLiteral(":/docs-log.png"));
    QPixmap logoPixmap = icon.pixmap(160, 160);
    logo->setPixmap(logoPixmap);
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);

    // 版本号取编译进二进制的 PROJECT_VERSION（main.cpp 里 setApplicationVersion）。
    // 不再读 theme.json 的 "version"：该配置与 Python 版共用，值可能缺失或过期，
    // 显示出来会和实际运行的版本对不上。
    QString version = QApplication::applicationVersion();
    if (version.isEmpty())
        version = QStringLiteral(CUBESHELL_VERSION);
    auto *versionLabel = new QLabel(
        tr("版本：  %1\n\n作者：     寒暄\n\n公众号：  IT技术小屋").arg(version), this);
    versionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(versionLabel);

    auto *info = new QLabel(
        tr("cubeShell 是 Linux 服务器远程管理工具。\n"
           "可以代替 Xshell、XSftp 等工具，对远程服务器进行管理。\n"
           " 简洁、方便、强大"), this);
    info->setAlignment(Qt::AlignCenter);
    layout->addWidget(info);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto *checkBtn = new QPushButton(tr("检查更新"), this);
    connect(checkBtn, &QPushButton::clicked, this, [this]() {
        emit checkUpdateRequested();
        close();
    });
    btnRow->addWidget(checkBtn);
    btnRow->addStretch(1);
    layout->addLayout(btnRow);
}

} // namespace cubeshell
