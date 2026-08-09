#pragma once

// net_terminal_widget.h — TCP / Telnet 终端面板。
//
// 布局同 SerialTerminalWidget：顶部细工具栏（主机/端口/连接/断开/清屏），
// 中间 QTermWidget，底部状态栏。工具栏连接后不隐藏——排查设备时经常要改
// 端口重试，藏起来反而难用。
//
// 终端渲染复用 SshTerminalWidget 的做法（QTermWidget(0, this) + 主题字体），
// 数据链路走 TcpBridge。
//
// 无条件编译（Qt6::Network 是顶层必需组件，鸿蒙上同样可用）。

#include <QWidget>

#include "net/TcpClient.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTermWidget;

namespace cubeshell {

class TcpBridge;

class NetTerminalWidget : public QWidget {
    Q_OBJECT
public:
    // mode: "telnet" | "tcp"。决定工具栏第二行露不露 Telnet 专属开关。
    explicit NetTerminalWidget(const QString &mode = QStringLiteral("telnet"),
                               QWidget *parent = nullptr);
    ~NetTerminalWidget() override;

    TcpClient *client() const { return m_client; }
    QTermWidget *terminal() const { return m_term; }

    // 工具栏当前内容 → 连接参数（凭据不在工具栏上，由 setSettings 记下来）。
    TcpSettings currentSettings() const;
    // 外部（设备列表 / 菜单对话框 / URL 唤起）预填工具栏。
    void setSettings(const TcpSettings &settings);

    // 立即按当前工具栏参数建连（设备列表双击打开时用）。
    void connectToHost();

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString &message);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onStateChanged(cubeshell::TcpClient::State state);
    void onError(const QString &message);
    void onNewlineModeChanged();
    void onLocalEchoToggled(bool enabled);
    void onRxImplicitCrToggled(bool enabled);
    void onBridgeLocalEchoChanged(bool enabled);
    void onLogToggled(bool enabled);
    void onClearClicked();

private:
    bool isTelnet() const { return m_mode == QLatin1String("telnet"); }
    void setFormEnabled(bool enabled);
    void updateStatus();

    QString m_mode;

    TcpClient *m_client = nullptr;
    TcpBridge *m_bridge = nullptr;
    QTermWidget *m_term = nullptr;

    QLineEdit *m_host = nullptr;
    QSpinBox  *m_port = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QCheckBox *m_rxImplicitCr = nullptr;
    QCheckBox *m_logging = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    // 工具栏上放不下、也不该让用户在会话中途改的参数（凭据/协商/终端类型），
    // 由 setSettings 存在这里，连接时与工具栏内容合并成完整的 TcpSettings。
    TcpSettings m_pending;

    // 最近一次错误，连接失败时显示在状态栏。
    QString m_lastError;
};

} // namespace cubeshell
