#pragma once

#include "AiConversationModel.h"
#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace cubeshell {
class AiConversationView;
#ifdef CUBESHELL_WITH_VOICE
class VoiceInput;
#endif

// Input/connection shell. Conversation records belong to the session, not this widget.
class AiChatPanel : public QWidget {
    Q_OBJECT
public:
    enum class ChatMode { PlainChat, SshAgent };
    Q_ENUM(ChatMode)
    explicit AiChatPanel(QWidget *parent = nullptr);
    ChatMode chatMode() const;
    void setConversationModel(AiConversationModel *model);
    AiConversationModel *conversationModel() const { return m_model; }
    void setBusy(bool busy);
    void appendAiMessage(const QString &message); // out-of-band errors/notices
    void setStatus(bool connected, const QString &host = {}, const QString &osInfo = {});
    void refreshModelLabel();
signals:
    void userMessageSent(const QString &text);
    void stopRequested();
    void clearRequested();
    void chatModeChanged(cubeshell::AiChatPanel::ChatMode mode);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void onSend();
    void applyTheme();
    QLabel *m_statusLabel, *m_modelLabel, *m_notice;
    QPlainTextEdit *m_inputEdit;
    QComboBox *m_modeCombo;
    QPushButton *m_sendBtn, *m_stopBtn;
    QToolButton *m_clearBtn;
    AiConversationView *m_view;
    QPointer<AiConversationModel> m_model;
    bool m_busy = false;
#ifdef CUBESHELL_WITH_VOICE
    QToolButton *m_micBtn;
    VoiceInput *m_voiceInput;
#endif
};
} // namespace cubeshell
