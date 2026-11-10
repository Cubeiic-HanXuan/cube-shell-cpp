#pragma once

// SshSessionTab.h — one SSH session tab hosting the terminal. The SFTP file
// browser it owns is shown in the main window's LEFT panel after the
// connection succeeds (mirroring cube-shell.py, where the left treeWidget
// switches from the device list to the remote file tree).

#include <QWidget>
#include <memory>

#include "config/DeviceConfigStore.h"

namespace cubeshell {

class SshTerminalWidget;
class SftpBrowserWidget;
class RemoteMonitor;
class SshKeepaliveTimer;
class SshClient;

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
    // 每次连接成功后创建；是否采集由复选框控制，连接前为 nullptr。
    RemoteMonitor *monitor() const { return m_monitor; }
    // 保存用户选择，使重连创建的新监控也遵循当前开关，而不是无条件启动。
    void setMonitoringEnabled(bool enabled);

signals:
    void connected();
    void connectionFailed(const QString &message);
    void disconnected();
    void mfaRequested(const QString &prompt);
    // 正在终端里等用户输密码（配置没存密码 / 上次认证失败重问）。
    void awaitingPassword();
    // 每一代连接的 RemoteMonitor 已就绪，先通知 UI 订阅，再按开关启动。
    void monitorReady();
    // OSC7 报告的远程 cwd（follow_folder 联动用）。
    void cwdChanged(const QString &path);

private:
    friend struct RemoteMonitorReconnectTestAccess;
    void bindConnectionServices(const std::shared_ptr<SshClient> &client);
    void releaseConnectionServices();

    DeviceEntry m_device;
    SshTerminalWidget *m_term = nullptr;
    SftpBrowserWidget *m_sftp = nullptr;
    RemoteMonitor *m_monitor = nullptr;
    SshKeepaliveTimer *m_keepalive = nullptr;
    std::weak_ptr<SshClient> m_servicesClient; // 监控和心跳所属的实际连接身份
    bool m_monitoringEnabled = true;
};

} // namespace cubeshell
