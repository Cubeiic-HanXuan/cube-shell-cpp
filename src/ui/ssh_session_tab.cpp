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
    // 必须在这里（而不是等 connected 信号）标记：下面的 connected 处理器一进来
    // 就 setClient() → 触发首次 loadPath，而首次加载正是要靠这个标记判断
    // "代理端不提供文件浏览"的那一次。晚一步设置就赶不上。
    m_sftp->setBastionProxied(device.viaBastion);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_term, 1);

    // Relay signals.
    connect(m_term, &SshTerminalWidget::connected, this, [this]() {
        // 挂接 SFTP（初始目录：OSC7 首个 cwd 或 "/"，见 MainWindow 侧接线）。
        // 传 shared_ptr：让 SftpBrowserWidget 共享 SshClient 所有权，保证应用关闭
        // 时 client 比 SftpClient 后析构（m_monitor 的注释同此约定）。
        if (m_term->sshClient())
            m_sftp->setClient(m_term->sshClient());
        // 连接成功后启动监控采集线程。
        // 对应Python: ssh_func.py::get_datas 的后台线程启动
        if (!m_monitor && m_term->sshClient()) {
            // 直接传 shared_ptr：监控对象共享 SshClient 所有权，其线程运行期间
            // client 必然存活，杜绝极端路径下的 use-after-free。
            m_monitor = new RemoteMonitor(m_term->sshClient(), this);
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
    connect(m_term, &SshTerminalWidget::awaitingPassword, this, &SshSessionTab::awaitingPassword);
    connect(m_term, &SshTerminalWidget::cwdChanged, this, &SshSessionTab::cwdChanged);
}

SshSessionTab::~SshSessionTab()
{
    // 关闭标签页时 UI 线程绝不能死等工作线程退出（否则 GNOME 弹 "not responding"）。
    // 顺序：
    //  1. 先 shutdown 底层 socket —— 强制唤醒阻塞在 select/read 上的监控线程、
    //     bridge 读线程，让它们立刻拿到 EOF/错误返回并重查取消标志退出。
    //  2. 再停监控线程（stop 已是有界等待 + 后台自删，不会卡住）。
    //  3. bridge/terminal 由 QObject 子对象析构链处理（~SshTerminalWidget →
    //     SshBridge::stop，其内部同样因 socket 已断而快速退出）。
    if (m_term && m_term->sshClient())
        m_term->sshClient()->shutdownSocket();
    if (m_monitor)
        m_monitor->stop();
}

void SshSessionTab::connectToHost()
{
    m_term->connectToHost();
}

} // namespace cubeshell
