#include "AboutDialog.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "config/GlobalState.h"

namespace cubeshell {

// 对应Python: function/about.py::AboutDialog.init_ui
AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("关于 cubeShell"));
    setFixedSize(400, 360);

    auto *layout = new QVBoxLayout(this);

    // Logo（Python 侧用 :docs-log.png 资源；C++ 侧资源在 Phase 6 接入，
    // 这里先用应用图标/占位文本）。
    auto *logo = new QLabel(QStringLiteral("cubeShell"), this);
    QFont logoFont = logo->font();
    logoFont.setPointSize(28);
    logoFont.setBold(true);
    logo->setFont(logoFont);
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);

    // 版本号取 theme.json 的 "version"（对应 util.THEME['version']）。
    QString version = GlobalState::instance().theme().value(QStringLiteral("version")).toString();
    if (version.isEmpty())
        version = QApplication::applicationVersion();
    auto *versionLabel = new QLabel(
        tr("版本：  %1\n\n作者：     寒暄\n\n公众号：  IT技术小屋").arg(version), this);
    versionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(versionLabel);

    auto *info = new QLabel(
        tr("cubeShell 是 Linux 服务器远程管理工具。\n"
           "可以代替 Xshell、XSftp 等工具，对远程服务器进行管理。\n"
           " 简洁、方便、强大\n\n开源协议：GPL-3.0"), this);
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
