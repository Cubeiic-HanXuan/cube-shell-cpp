#pragma once

// AddDeviceDialog.h — add / edit a saved SSH / RDP device.
//
// C++ counterpart of AddConfigUi (cube-shell.py:5989). Edits name / username /
// password-or-key / host / port (plus RDP auth/domain when built with
// CUBESHELL_WITH_RDP) and returns a DeviceEntry via device(). Used for both
// "add" and "edit" (setDevice to pre-fill).

#include <QDialog>

#include "config/DeviceConfigStore.h"

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

#ifdef CUBESHELL_WITH_RDP
    // 对应Python: _on_protocol_changed（cube-shell.py:6073-6084）
    void onProtocolChanged(int index);
    bool rdpSelected() const;
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

#ifdef CUBESHELL_WITH_RDP
    // RDP 专用控件。对应Python: _inject_protocol_fields（cube-shell.py:5989-6084）
    QComboBox *m_protocol = nullptr;        // Row 0: "SSH" | "RDP"
    QComboBox *m_rdpAuth = nullptr;         // data: "ntlm" | "plain"
    QLineEdit *m_domain = nullptr;          // Windows 域（可选）
#endif
};

} // namespace cubeshell
