// DockerSoftDialog.cpp — see DockerSoftDialog.h.
// 对应Python: cube-shell.py:1047-1077 + 4807-4908 + 6396-6458

#include "DockerSoftDialog.h"

#include <QCursor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "docker/ComposeYaml.h"
#include "editors/DockerComposeEditor.h"

namespace cubeshell {

namespace {

// 对应Python: style/style.py:49-65 InstallButtonStyle（字符串照抄）
const char *const kInstallButtonStyle = R"(
    QPushButton {
                    color: white;         /* 设置字体颜色 */
                    background-color: #007BFF; /* 可选：设置背景色提高对比度 */
                    border-radius: 5px;   /* 可选：圆角效果 */
                    padding: 8px 16px;    /* 可选：内边距 */
                }
                
                /* 设置不同状态下的颜色 */
                QPushButton:hover {
                    background-color: #0056b3;
                }
                
                QPushButton:pressed {
                    background-color: #003d80;
                }
)";

// 对应Python: style/style.py:67-83 InstalledButtonStyle（字符串照抄）
const char *const kInstalledButtonStyle = R"(
    QPushButton {
                    color: white;         /* 设置字体颜色 */
                    background-color: #00f260; /* 可选：设置背景色提高对比度 */
                    border-radius: 5px;   /* 可选：圆角效果 */
                    padding: 8px 16px;    /* 可选：内边距 */
                }
                
                /* 设置不同状态下的颜色 */
                QPushButton:hover {
                    background-color: #0056b3;
                }
                
                QPushButton:pressed {
                    background-color: #003d80;
                }
)";

// 对应Python: cube-shell.py:6435-6454 CustomWidget.setStyleSheet（字符串照抄；
// 原文即是无法解析的 qss，Python 侧同样不生效，保持行为一致）
const char *const kCardStyle = R"(
            QWidget
            {
                border - radius: 5px;
            padding: 5
            px;
            }
            QPushButton
            {
                background - color: rgb(50, 115, 245);
            border - radius: 5
            px;
            padding: 5
            px;
            }
            QPushButton: pressed
            {
                background - color: darkgray;
            }
            )";

// 对应Python: function/util.py:288-295 clear_grid_layout（递归清空子布局）
void clearLayoutRecursive(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->deleteLater();
        } else if (QLayout *child = item->layout()) {
            clearLayoutRecursive(child); // 递归清空子布局
        }
        delete item;
    }
}

} // namespace

DockerSoftDialog::DockerSoftDialog(DockerManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    // 对应Python: cube-shell.py:1052-1055
    setWindowTitle(tr("常用容器安装"));
    setMinimumSize(800, 550);
    setModal(false);

    // 对应Python: cube-shell.py:1057-1064（外层 VBox + 内容 QGridLayout）
    auto *layout = new QVBoxLayout(this);
    auto *contentWidget = new QWidget(this);
    m_grid = new QGridLayout(contentWidget);
    m_grid->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(contentWidget);

    // 信号来自工作线程，显式 QueuedConnection（见 DockerManager.h 线程模型）
    connect(m_manager, &DockerManager::commonContainersReady,
            this, &DockerSoftDialog::onCommonContainersReady, Qt::QueuedConnection);
}

// 对应Python: cube-shell.py:4807-4838 refresh_docker_common_containers
void DockerSoftDialog::refreshInfo()
{
    clearGrid();

    // 显示加载状态（qss 照抄 4824 行）
    auto *loadingLabel = new QLabel(tr("正在加载常用容器信息..."));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setStyleSheet(QStringLiteral("font-size: 16px; color: #666;"));
    m_grid->addWidget(loadingLabel);

    // 对应Python: CommonContainersThread(ssh_conn, abspath('docker-compose-full.yml'))
    m_manager->checkCommonContainers(ComposeYaml::defaultComposeFullPath());
}

// 对应Python: cube-shell.py:4840-4908 update_common_containers_ui
void DockerSoftDialog::onCommonContainersReady(
    const QList<cubeshell::CommonServiceInfo> &services, bool hasDocker)
{
    clearGrid();

    if (hasDocker) {
        // 每行最多四个小块 (原文是8，注释写每行最多四个但变量是8，保留原逻辑)
        const int maxColumns = 8;

        // 创建滚动区域
        auto *scrollArea = new QScrollArea();
        scrollArea->setWidgetResizable(true);                          // 允许内容自适应大小
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); // 始终显示垂直滚动条

        // 创建滚动内容容器
        auto *scrollContent = new QWidget();
        scrollArea->setWidget(scrollContent);

        // 使用网格布局管理滚动内容
        auto *gridLayout = new QGridLayout(scrollContent);
        gridLayout->setContentsMargins(0, 0, 0, 0); // 设置布局边距
        gridLayout->setHorizontalSpacing(2);        // 设置水平间距
        gridLayout->setVerticalSpacing(2);          // 设置垂直间距

        // 将滚动区域添加到原布局位置（替换原来的gridLayout_7）
        m_grid->addWidget(scrollArea);

        // 遍历列表创建小块
        for (int index = 0; index < services.size(); ++index) {
            const int row = index / maxColumns;
            const int col = index % maxColumns;

            // 创建外层容器
            auto *containerWidget = new QWidget();
            containerWidget->setFixedSize(95, 143); // 固定每个小块的尺寸
            auto *containerLayout = new QVBoxLayout(containerWidget);
            containerLayout->setContentsMargins(0, 0, 0, 0); // 移除内边距

            // 创建自定义组件
            containerLayout->addWidget(createCard(services.at(index)));

            // 添加到网格布局
            gridLayout->addWidget(containerWidget, row, col);
        }
    } else {
        // 对应Python: cube-shell.py:4891-4908 else 分支（文案照抄）
        auto *containerWidget = new QWidget();
        auto *containerLayout = new QVBoxLayout();
        containerWidget->setLayout(containerLayout);
        containerLayout->setContentsMargins(0, 0, 0, 0); // 去掉布局的内边距

        auto *textBrowser = new QTextBrowser(containerWidget);
        textBrowser->append(QStringLiteral("\n"));
        textBrowser->append(QStringLiteral("\n"));
        textBrowser->append(QStringLiteral("\n"));
        textBrowser->append(tr("服务器还没有安装docker容器"));
        // 设置内容居中对齐
        textBrowser->setAlignment(Qt::AlignCenter);

        containerLayout->addWidget(textBrowser);
        m_grid->addWidget(containerWidget);
    }
}

// 对应Python: cube-shell.py:6396-6454 CustomWidget.__init__
QWidget *DockerSoftDialog::createCard(const CommonServiceInfo &service)
{
    auto *card = new QWidget();
    auto *cardLayout = new QVBoxLayout(card);

    // 创建图标标签（资源缺失时 pixmap 为空，与 Python 一致地留白回退）
    auto *iconLabel = new QLabel(card);
    const QIcon icon(QStringLiteral(":/%1_128.png").arg(service.key));
    iconLabel->setPixmap(icon.pixmap(60, 60));
    iconLabel->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(iconLabel);

    // 创建按钮布局
    auto *buttonLayout = new QHBoxLayout();

    if (!service.has) {
        // 安装按钮
        auto *installButton = new QPushButton(tr("安装"), card);
        installButton->setCursor(QCursor(Qt::PointingHandCursor));
        connect(installButton, &QPushButton::clicked,
                this, &DockerSoftDialog::openComposeEditor);
        installButton->setStyleSheet(QString::fromUtf8(kInstallButtonStyle));
        buttonLayout->addWidget(installButton);
    } else {
        // 已安装按钮
        auto *installButton = new QPushButton(tr("已安装"), card);
        installButton->setCursor(QCursor(Qt::PointingHandCursor));
        installButton->setStyleSheet(QString::fromUtf8(kInstalledButtonStyle));
        installButton->setDisabled(true);
        buttonLayout->addWidget(installButton);
    }

    cardLayout->addLayout(buttonLayout);

    // 设置样式表为小块添加边框
    card->setStyleSheet(QString::fromUtf8(kCardStyle));
    return card;
}

// 对应Python: cube-shell.py:6456-6458 container_orchestration —— 每次点击
// 新建一个编辑器窗口；C++ 侧无 GC，用 WA_DeleteOnClose 在关闭时回收。
void DockerSoftDialog::openComposeEditor()
{
    auto *compose = new DockerComposeEditor(m_manager);
    compose->setAttribute(Qt::WA_DeleteOnClose);
    compose->show();
}

// 对应Python: util.clear_grid_layout(self.ui.gridLayout_7)
void DockerSoftDialog::clearGrid()
{
    clearLayoutRecursive(m_grid);
}

} // namespace cubeshell
