#pragma once

// HostKeyDialog.h — prompt the user to accept or reject an SSH server's host
// key on first connection or when the key has changed.

#include <QDialog>

#include "ssh/SshClient.h"

class QLabel;
class QDialogButtonBox;

namespace cubeshell {

class HostKeyDialog : public QDialog {
    Q_OBJECT
public:
    HostKeyDialog(const QString &host, quint16 port,
                  const QString &fingerprintDisplay, const QString &keyType,
                  bool keyChanged, QWidget *parent = nullptr);

    HostKeyPromptResult hostKeyResult() const { return m_result; }

private:
    void setupUi();

    QString m_host;
    quint16 m_port = 22;
    QString m_fingerprint;
    QString m_keyType;
    bool m_keyChanged = false;

    HostKeyPromptResult m_result = HostKeyPromptResult::Reject;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};

} // namespace cubeshell
