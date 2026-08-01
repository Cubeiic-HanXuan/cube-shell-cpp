#pragma once

// AuthDialog.h — SSH 认证方式选择对话框（密码 / 私钥 / keyboard-interactive）。
//
// 对应Python: cube-shell.py 连接流程中认证方式的选择与凭据补录
// （AddConfigUi 的密码/密钥分支 + paramiko keyboard-interactive 的 MFA 输入）。
// 当保存的凭据缺失或认证失败时弹出，让用户补充认证信息。

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace cubeshell {

class AuthDialog : public QDialog {
    Q_OBJECT
public:
    enum class Method {
        Password,            // 密码认证
        PrivateKey,          // 私钥认证
        KeyboardInteractive, // keyboard-interactive（MFA/OTP）
    };

    explicit AuthDialog(QWidget *parent = nullptr);

    // 预填用户名（只读展示，凭据针对该用户）。
    void setUsername(const QString &username);
    void setMethod(Method method);

    Method method() const;
    QString password() const;      // Password 分支
    QString keyType() const;       // PrivateKey 分支: Ed25519Key/RSAKey/ECDSAKey/DSSKey
    QString keyFile() const;
    QString passphrase() const;
    QString interactiveAnswer() const; // KeyboardInteractive 分支（验证码等）

    void accept() override;

private slots:
    void onMethodChanged(int index);
    void onBrowseKey();

private:
    QLineEdit *m_username = nullptr;
    QComboBox *m_method = nullptr;
    QStackedWidget *m_stack = nullptr;

    // password page
    QLineEdit *m_password = nullptr;
    // key page
    QComboBox *m_keyType = nullptr;
    QLineEdit *m_keyFile = nullptr;
    QLineEdit *m_passphrase = nullptr;
    QPushButton *m_browse = nullptr;
    // keyboard-interactive page
    QLineEdit *m_answer = nullptr;
};

} // namespace cubeshell
