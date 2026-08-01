#include "ssh_session_tab.h"

#include <QVBoxLayout>

#include "sftp_browser_widget.h"
#include "ssh_terminal_widget.h"
#include "ssh/RemoteMonitor.h"
#include "ssh/SshClient.h"

namespace cubeshell {

SshSessionTab::SshSessionTab(const DeviceEntry &device, QWidget *parent)
    : QWidget(parent)
    , m_device(device)
{
    m_term = new SshTerminalWidget(device, this);

    // SFTP 浏览器由本 tab 创建，但连接成功后被主窗口 reparent 到左侧面板
    // 显示（对应Python: 左侧 treeWidget 连接后切换为远程文件树）。
    m_sftp = new SftpBrowserWidget(this);
    m_sftp->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_term, 1);

    // Relay signals.
    connect(m_term, &SshTerminalWidget::connected, this, [this]() {
        // 挂接 SFTP（初始目录：OSC7 首个 cwd 或 "/"，见 MainWindow 侧接线）。
        if (m_term->sshClient())
            m_sftp->setClient(m_term->sshClient().get());
        // 连接成功后启动监控采集线程。
        // 对应Python: ssh_func.py::get_datas 的后台线程启动
        if (!m_monitor && m_term->sshClient()) {
            m_monitor = new RemoteMonitor(m_term->sshClient().get(), this);
            m_monitor->start();
            emit monitorReady();
        }
        emit connected();
    });
    connect(m_term, &SshTerminalWidget::connectionFailed, this, &SshSessionTab::connectionFailed);
    connect(m_term, &SshTerminalWidget::disconnected, this, [this]() {
        if (m_monitor)
            m_monitor->stop();
        emit disconnected();
    });
    connect(m_term, &SshTerminalWidget::mfaRequested, this, &SshSessionTab::mfaRequested);
    connect(m_term, &SshTerminalWidget::cwdChanged, this, &SshSessionTab::cwdChanged);
}

SshSessionTab::~SshSessionTab()
{
    // 先停监控线程（join），再让 QObject 析构子控件，避免监控线程访问已释放的 SshClient。
    if (m_monitor)
        m_monitor->stop();
}

void SshSessionTab::connectToHost()
{
    m_term->connectToHost();
}

} // namespace cubeshell
