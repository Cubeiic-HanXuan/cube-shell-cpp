#pragma once

// SshSessionTab.h — one SSH session tab hosting the terminal. The SFTP file
// browser it owns is shown in the main window's LEFT panel after the
// connection succeeds (mirroring cube-shell.py, where the left treeWidget
// switches from the device list to the remote file tree).

#include <QWidget>

#include "config/DeviceConfigStore.h"

namespace cubeshell {

class SshTerminalWidget;
class SftpBrowserWidget;
class RemoteMonitor;

class SshSessionTab : public QWidget {
    Q_OBJECT
public:
    explicit SshSessionTab(const DeviceEntry &device, QWidget *parent = nullptr);
    ~SshSessionTab() override;

    void connectToHost();

    const DeviceEntry &device() const { return m_device; }
    SshTerminalWidget *terminal() const { return m_term; }
    // 连接成功后已挂接 SftpClient；主窗口把它放进左侧面板显示。
    // 对应Python: 连接成功后左侧 treeWidget 切换为远程文件树
    SftpBrowserWidget *sftpBrowser() const { return m_sftp; }
    // 连接成功后创建并启动；连接前为 nullptr。
    RemoteMonitor *monitor() const { return m_monitor; }

signals:
    void connected();
    void connectionFailed(const QString &message);
    void disconnected();
    void mfaRequested(const QString &prompt);
    // 正在终端里等用户输密码（配置没存密码 / 上次认证失败重问）。
    void awaitingPassword();
    // RemoteMonitor 已创建并启动（主窗口监控面板据此重新订阅）。
    void monitorReady();
    // OSC7 报告的远程 cwd（follow_folder 联动用）。
    void cwdChanged(const QString &path);

private:
    DeviceEntry m_device;
    SshTerminalWidget *m_term = nullptr;
    SftpBrowserWidget *m_sftp = nullptr;
    RemoteMonitor *m_monitor = nullptr;
};

} // namespace cubeshell
