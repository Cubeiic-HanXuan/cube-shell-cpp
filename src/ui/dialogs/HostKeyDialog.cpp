// HostKeyDialog.cpp — host key acceptance prompt.

#include "HostKeyDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace cubeshell {

HostKeyDialog::HostKeyDialog(const QString &host, quint16 port,
                             const QString &fingerprintDisplay, const QString &keyType,
                             bool keyChanged, QWidget *parent)
    : QDialog(parent)
    , m_host(host)
    , m_port(port)
    , m_fingerprint(fingerprintDisplay)
    , m_keyType(keyType)
    , m_keyChanged(keyChanged)
{
    setWindowTitle(keyChanged ? tr("警告：主机密钥已变更") : tr("主机密钥未记录"));
    setMinimumWidth(480);
    setupUi();
}

void HostKeyDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(16);

    auto *topLayout = new QHBoxLayout();
    topLayout->setSpacing(12);

    m_iconLabel = new QLabel(this);
    const QIcon icon = style()->standardIcon(m_keyChanged ? QStyle::SP_MessageBoxCritical
                                                          : QStyle::SP_MessageBoxWarning);
    m_iconLabel->setPixmap(icon.pixmap(48, 48));
    topLayout->addWidget(m_iconLabel, 0, Qt::AlignTop);

    const QString hostPort = m_port == 22 ? m_host : QStringLiteral("%1:%2").arg(m_host).arg(m_port);

    QString text;
    if (m_keyChanged) {
        text = tr("<p><b>%1</b> 的主机密钥与之前保存的不一致。</p>"
                  "<p>这通常意味着：服务器被重新安装、管理员更换了密钥，或者你正遭受中间人攻击。</p>"
                  "<p>请在继续前向服务器管理员确认密钥变更是合法的。</p>")
                   .arg(hostPort);
    } else {
        text = tr("<p>首次连接 <b>%1</b>。</p>"
                  "<p>请核对此指纹与服务器上 <code>/etc/ssh/ssh_host_%2_key.pub</code> 的指纹一致后再接受。</p>")
                   .arg(hostPort, m_keyType);
    }

    m_textLabel = new QLabel(text, this);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_textLabel->setOpenExternalLinks(false);
    topLayout->addWidget(m_textLabel, 1);
    mainLayout->addLayout(topLayout);

    auto *fpLabel = new QLabel(
        tr("<b>主机密钥指纹：</b><code style='font-size:13px;'>%1</code>").arg(m_fingerprint),
        this);
    fpLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fpLabel->setWordWrap(true);
    mainLayout->addWidget(fpLabel);

    mainLayout->addSpacing(8);

    m_buttonBox = new QDialogButtonBox(this);

    QPushButton *acceptSaveBtn = m_buttonBox->addButton(
        m_keyChanged ? tr("接受新密钥并保存") : tr("接受并保存"),
        QDialogButtonBox::AcceptRole);
    QPushButton *acceptOnceBtn = m_buttonBox->addButton(
        tr("仅本次连接不保存"), QDialogButtonBox::YesRole);
    QPushButton *rejectBtn = m_buttonBox->addButton(
        tr("拒绝并断开"), QDialogButtonBox::RejectRole);

    // Don't make "accept" the default to reduce accidental confirmation.
    rejectBtn->setDefault(true);
    rejectBtn->setFocus();

    mainLayout->addWidget(m_buttonBox);

    connect(acceptSaveBtn, &QPushButton::clicked, this, [this]() {
        m_result = HostKeyPromptResult::AcceptAndSave;
        accept();
    });
    connect(acceptOnceBtn, &QPushButton::clicked, this, [this]() {
        m_result = HostKeyPromptResult::AcceptOnce;
        accept();
    });
    connect(rejectBtn, &QPushButton::clicked, this, [this]() {
        m_result = HostKeyPromptResult::Reject;
        reject();
    });
}

} // namespace cubeshell
