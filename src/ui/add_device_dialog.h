#pragma once

// AddDeviceDialog.h — add / edit a saved SSH / RDP / Serial / Telnet / TCP device.
//
// C++ counterpart of AddConfigUi (cube-shell.py:5989). Edits name / username /
// password-or-key / host / port (plus RDP auth/domain when built with
// CUBESHELL_WITH_RDP, plus serial port params when built with
// CUBESHELL_WITH_SERIAL, plus TCP/Telnet params unconditionally) and returns a
// DeviceEntry via device(). Used for both "add" and "edit" (setDevice to pre-fill).
//
// 协议下拉框无条件存在：TCP/Telnet 不依赖任何可选组件，所以至少有
// SSH / Telnet / TCP 三项可选。（早先这里有个 CUBESHELL_HAS_PROTOCOL_COMBO 宏，
// 定义为"RDP 或 Serial 任一开启"，在两者都关掉的鸿蒙构建上会让整个下拉框消失。）

#include <QDialog>

#include "config/DeviceConfigStore.h"

class QCheckBox;
class QLineEdit;
class QComboBox;
class QFormLayout;
class QPushButton;
class QStackedWidget;

namespace cubeshell {

class AddDeviceDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddDeviceDialog(QWidget *parent = nullptr);

    // Pre-fill for editing an existing entry.
    void setDevice(const DeviceEntry &entry);
    // The edited entry (valid after accept()).
    DeviceEntry device() const;

    void accept() override;

private slots:
    void onAuthMethodChanged(int index);
    void onBrowseKey();

private:
    bool validate(QString *err) const;

    // 当前选中的协议值（"ssh" | "rdp" | "serial" | "telnet" | "tcp"）。
    // 一律取 currentData() 而非 currentText()：显示文本是给人看的，一旦有人
    // 给协议名加上 tr() 或改个大小写，比对文本的判定就会静默失效。
    QString selectedProtocol() const;
    bool rdpSelected() const;
    bool serialSelected() const;
    bool telnetSelected() const;
    bool tcpSelected() const;

    // 对应Python: _on_protocol_changed（cube-shell.py:6073-6084）
    void onProtocolChanged(int index);
#ifdef CUBESHELL_WITH_SERIAL
    void onRefreshPorts();
#endif

    QFormLayout *m_form = nullptr;
    QComboBox *m_protocol = nullptr;        // Row 0: SSH | RDP | Serial | Telnet | TCP
    QLineEdit *m_name = nullptr;
    QLineEdit *m_username = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_port = nullptr;

    QComboBox *m_authMethod = nullptr;      // 0 = password, 1 = private key
    QStackedWidget *m_authStack = nullptr;

    // password page
    QLineEdit *m_password = nullptr;
    // key page
    QComboBox *m_keyType = nullptr;         // Ed25519Key / RSAKey / ECDSAKey / DSSKey
    QLineEdit *m_keyFile = nullptr;
    QPushButton *m_browseKey = nullptr;

    // 端口框里当前放的是哪个协议的默认值。切协议时据此判断"用户没改过端口"，
    // 从而可以安全地换成新协议的默认端口（用户手填过的端口不动）。
    QString m_portDefaultFor;

#ifdef CUBESHELL_WITH_RDP
    // RDP 专用控件。对应Python: _inject_protocol_fields（cube-shell.py:5989-6084）
    QComboBox *m_rdpAuth = nullptr;         // data: "ntlm" | "plain"
    QLineEdit *m_domain = nullptr;          // Windows 域（可选）
#endif
#ifdef CUBESHELL_WITH_SERIAL
    // 串口专用控件（Python 版无对应实现）。
    QWidget   *m_serialPortRow = nullptr;   // 端口下拉 + 刷新按钮的容器
    QComboBox *m_serialPort = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;
    QComboBox *m_newline = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QCheckBox *m_rxImplicitCr = nullptr;
#endif
    // TCP/Telnet 专用控件（无条件编译）。换行/回显/接收补 CR 三项与串口是
    // 同一套语义，但刻意用独立控件而非复用串口那三个——串口控件在
    // CUBESHELL_WITH_SERIAL=OFF 时不存在，而 TCP/Telnet 在任何构建里都要能用。
    QComboBox *m_netNewline = nullptr;
    QCheckBox *m_netLocalEcho = nullptr;
    QCheckBox *m_netRxImplicitCr = nullptr;
    QComboBox *m_termType = nullptr;        // Telnet TERMINAL-TYPE 上报值
    QCheckBox *m_negotiate = nullptr;       // Telnet IAC 选项协商
    QCheckBox *m_autoLogin = nullptr;       // Telnet 自动登录
};

} // namespace cubeshell
