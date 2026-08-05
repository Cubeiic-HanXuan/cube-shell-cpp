#pragma once

// AddDeviceDialog.h — add / edit a saved SSH / RDP / Serial device.
//
// C++ counterpart of AddConfigUi (cube-shell.py:5989). Edits name / username /
// password-or-key / host / port (plus RDP auth/domain when built with
// CUBESHELL_WITH_RDP, plus serial port params when built with
// CUBESHELL_WITH_SERIAL) and returns a DeviceEntry via device(). Used for both
// "add" and "edit" (setDevice to pre-fill).

#include <QDialog>

#include "config/DeviceConfigStore.h"

class QCheckBox;
class QLineEdit;
class QComboBox;
class QFormLayout;
class QPushButton;
class QStackedWidget;

namespace cubeshell {

// 协议下拉框在 RDP 或 串口 任一开启时才有意义（只有 SSH 时不显示）。
#if defined(CUBESHELL_WITH_RDP) || defined(CUBESHELL_WITH_SERIAL)
#define CUBESHELL_HAS_PROTOCOL_COMBO 1
#endif

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

#ifdef CUBESHELL_HAS_PROTOCOL_COMBO
    // 对应Python: _on_protocol_changed（cube-shell.py:6073-6084）
    void onProtocolChanged(int index);
#endif
#ifdef CUBESHELL_WITH_RDP
    bool rdpSelected() const;
#endif
#ifdef CUBESHELL_WITH_SERIAL
    bool serialSelected() const;
    void onRefreshPorts();
#endif

    QFormLayout *m_form = nullptr;
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

#ifdef CUBESHELL_HAS_PROTOCOL_COMBO
    QComboBox *m_protocol = nullptr;        // Row 0: "SSH" | "RDP" | "Serial"
#endif
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
};

} // namespace cubeshell
