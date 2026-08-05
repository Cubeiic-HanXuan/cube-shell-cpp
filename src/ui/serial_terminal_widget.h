#pragma once

// serial_terminal_widget.h — 串口终端面板。
//
// 布局仿 RdpPanel 的"表单 + 状态 + 画面合一"：顶部是串口参数工具栏，
// 中间是 QTermWidget，底部是状态栏。与 RdpPanel 不同的是工具栏连接后不隐藏
// ——串口经常要在同一个会话里改波特率试探设备，藏起来反而难用（改参数会
// 重开端口）。
//
// 终端渲染复用 SshTerminalWidget 的做法（QTermWidget(0, this) + 主题字体），
// 但数据链路走 SerialBridge 而非 SshBridge。
//
// 仅在 CUBESHELL_WITH_SERIAL=ON 时编译。

#include <QWidget>

#include "serial/SerialClient.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTermWidget;

namespace cubeshell {

class SerialBridge;

class SerialTerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit SerialTerminalWidget(QWidget *parent = nullptr);
    ~SerialTerminalWidget() override;

    SerialClient *client() const { return m_client; }
    QTermWidget *terminal() const { return m_term; }

    // 工具栏当前内容 → 连接参数。
    SerialSettings currentSettings() const;
    // 外部（设备列表 / 菜单对话框）预填工具栏。
    void setSettings(const SerialSettings &settings);

    // 立即按当前工具栏参数建连（设备列表双击打开时用）。
    void connectToPort();

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString &message);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onStateChanged(cubeshell::SerialClient::State state);
    void onError(const QString &message);
    void onPortsChanged();
    void onNewlineModeChanged();
    void onLocalEchoToggled(bool enabled);
    void onRxImplicitCrToggled(bool enabled);
    void onLogToggled(bool enabled);
    void onClearClicked();

private:
    void setFormEnabled(bool enabled);
    void updateStatus();

    SerialClient *m_client = nullptr;
    SerialBridge *m_bridge = nullptr;
    QTermWidget *m_term = nullptr;

    QComboBox *m_port = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QCheckBox *m_rxImplicitCr = nullptr;
    QCheckBox *m_logging = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QLabel *m_statusLabel = nullptr;

    // 最近一次错误，连接失败时显示在状态栏（errorOccurred 早于 open() 返回）。
    QString m_lastError;
};

} // namespace cubeshell
