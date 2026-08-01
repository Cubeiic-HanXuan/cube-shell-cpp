#include "AuthDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace cubeshell {

// 对应Python: cube-shell.py::AddConfigUi 的密码/密钥认证分支布局
AuthDialog::AuthDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("SSH 认证"));
    setMinimumWidth(360);

    m_username = new QLineEdit(this);
    m_username->setReadOnly(true);

    m_method = new QComboBox(this);
    m_method->addItem(tr("密码登录"));
    m_method->addItem(tr("私钥登录"));
    m_method->addItem(tr("键盘交互（MFA）"));

    m_stack = new QStackedWidget(this);

    // --- password page ---
    auto *pwPage = new QWidget(this);
    {
        auto *form = new QFormLayout(pwPage);
        m_password = new QLineEdit(pwPage);
        m_password->setEchoMode(QLineEdit::Password);
        form->addRow(tr("密  码："), m_password);
    }
    m_stack->addWidget(pwPage);

    // --- private key page ---
    auto *keyPage = new QWidget(this);
    {
        auto *form = new QFormLayout(keyPage);
        m_keyType = new QComboBox(keyPage);
        // 对应Python: paramiko key 类名（DeviceEntry.keyType 同名）
        m_keyType->addItems({QStringLiteral("Ed25519Key"), QStringLiteral("RSAKey"),
                             QStringLiteral("ECDSAKey"), QStringLiteral("DSSKey")});
        m_keyFile = new QLineEdit(keyPage);
        m_browse = new QPushButton(tr("浏览…"), keyPage);
        auto *fileRow = new QHBoxLayout;
        fileRow->addWidget(m_keyFile, 1);
        fileRow->addWidget(m_browse);
        m_passphrase = new QLineEdit(keyPage);
        m_passphrase->setEchoMode(QLineEdit::Password);
        form->addRow(tr("私钥类型："), m_keyType);
        form->addRow(tr("私钥文件"), fileRow);
        form->addRow(tr("私钥口令："), m_passphrase);
    }
    m_stack->addWidget(keyPage);

    // --- keyboard-interactive page ---
    auto *kbiPage = new QWidget(this);
    {
        auto *form = new QFormLayout(kbiPage);
        m_answer = new QLineEdit(kbiPage);
        form->addRow(tr("验证码："), m_answer);
    }
    m_stack->addWidget(kbiPage);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *form = new QFormLayout;
    form->addRow(tr("用户名："), m_username);
    form->addRow(tr("认证方式："), m_method);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_stack, 1);
    layout->addWidget(buttons);

    connect(m_method, &QComboBox::currentIndexChanged, this, &AuthDialog::onMethodChanged);
    connect(m_browse, &QPushButton::clicked, this, &AuthDialog::onBrowseKey);
    connect(buttons, &QDialogButtonBox::accepted, this, &AuthDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &AuthDialog::reject);
}

void AuthDialog::setUsername(const QString &username)
{
    m_username->setText(username);
}

void AuthDialog::setMethod(Method method)
{
    m_method->setCurrentIndex(static_cast<int>(method));
}

AuthDialog::Method AuthDialog::method() const
{
    return static_cast<Method>(m_method->currentIndex());
}

QString AuthDialog::password() const { return m_password->text(); }
QString AuthDialog::keyType() const { return m_keyType->currentText(); }
QString AuthDialog::keyFile() const { return m_keyFile->text().trimmed(); }
QString AuthDialog::passphrase() const { return m_passphrase->text(); }
QString AuthDialog::interactiveAnswer() const { return m_answer->text().trimmed(); }

void AuthDialog::onMethodChanged(int index)
{
    m_stack->setCurrentIndex(index);
}

void AuthDialog::onBrowseKey()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("选择私钥文件"), QDir::homePath());
    if (!path.isEmpty())
        m_keyFile->setText(path);
}

void AuthDialog::accept()
{
    if (method() == Method::PrivateKey && keyFile().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("请选择私钥文件。"));
        return;
    }
    QDialog::accept();
}

} // namespace cubeshell
