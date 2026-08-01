#pragma once

// SshTerminalWidget.h — vertical-slice SSH terminal tab.
//
// A QTermWidget whose Session is bridged to a remote SSH shell channel
// (SshClient + SshBridge). This is the C++ counterpart of the SSHQTermWidget
// path in cube-shell.py, cut down to the vertical slice: connect -> open pty
// shell -> display + keystrokes.
//
// Connection + auth happen on a worker thread (SshClient is thread-hostile and
// blocking during connect); the SshBridge then runs the read loop on its own
// thread and pushes bytes into the terminal via queued signals.

#include <QWidget>

#include <memory>
#include <utility>

#include "config/DeviceConfigStore.h"

class QTermWidget;

namespace cubeshell {

class SshClient;
class SshBridge;
class TerminalCommandSuggest;

class SshTerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit SshTerminalWidget(const DeviceEntry &device, QWidget *parent = nullptr);
    ~SshTerminalWidget() override;

    // Kick off the connection (async). Safe to call once, right after ctor.
    void connectToHost();

    // The connected SSH client (null until connected()). Owned by this widget;
    // do not delete. Used to attach an SftpClient for the file browser.
    std::shared_ptr<SshClient> sshClient() const { return m_client; }
    // The terminal (for embedding / focusing).
    QTermWidget *terminal() const { return m_term; }

signals:
    // Emitted on the UI thread.
    void connected();
    void connectionFailed(const QString &message);
    void disconnected();            // channel closed (remote exit / drop)
    void mfaRequested(const QString &prompt);
    // OSC7 报告的远程 cwd（从 SshBridge 转发，已切回 UI 线程）。
    // 对应Python: cube-shell.py::_on_cwd_changed 的信号源
    void cwdChanged(const QString &path);

private slots:
    void onMfaPrompt(const QString &prompt);
    void onDisconnected();

private:
    bool promptForMfa(const QString &prompt, bool echo, QString &response);

    DeviceEntry m_device;
    QTermWidget *m_term = nullptr;
    // 终端命令智能提示控制器（子 QObject，随本 widget 析构）。
    // 对应Python: cube-shell.py::SSHQTermWidget 提示相关逻辑
    TerminalCommandSuggest *m_suggest = nullptr;

    // The client is created and connected on a worker thread, then handed to
    // the bridge. shared_ptr so the worker lambda can keep it alive until the
    // UI thread takes its copy.
    std::shared_ptr<SshClient> m_client;
    SshBridge *m_bridge = nullptr;         // child QObject

    bool m_started = false;
};

} // namespace cubeshell
