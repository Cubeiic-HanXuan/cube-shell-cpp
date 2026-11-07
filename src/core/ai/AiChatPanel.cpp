#include "AiChatPanel.h"
#include "AiConversationView.h"
#include "AiPreferences.h"
#include "widgets/ConversationWidgets.h"
#ifdef CUBESHELL_WITH_VOICE
#include "VoiceInput.h"
#endif

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

namespace cubeshell {

AiChatPanel::AiChatPanel(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(300);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *header = new QWidget(this);
    auto *headerLayout = new QVBoxLayout(header); headerLayout->setContentsMargins(14, 10, 14, 10); headerLayout->setSpacing(3);
    m_statusLabel = new QLabel(tr("未连接 SSH"), header); m_statusLabel->setTextFormat(Qt::PlainText);
    auto headingFont = m_statusLabel->font(); headingFont.setBold(true); m_statusLabel->setFont(headingFont);
    headerLayout->addWidget(m_statusLabel);
    m_modelLabel = new sessionui::ElidedLabel({}, header); headerLayout->addWidget(m_modelLabel);
    layout->addWidget(header);
    m_notice = new QLabel(this); m_notice->setTextFormat(Qt::PlainText); m_notice->setWordWrap(true);
    m_notice->setMargin(10); m_notice->hide(); layout->addWidget(m_notice);
    m_view = new AiConversationView(this); layout->addWidget(m_view, 1);

    auto *inputFrame = new QFrame(this); inputFrame->setObjectName(QStringLiteral("aiInputFrame"));
    auto *inputLayout = new QVBoxLayout(inputFrame); inputLayout->setContentsMargins(12, 10, 12, 10); inputLayout->setSpacing(8);
    m_inputEdit = new QPlainTextEdit(inputFrame);
    m_inputEdit->setObjectName(QStringLiteral("aiInput"));
    m_inputEdit->setPlaceholderText(tr("描述运维需求…\nEnter 发送，Shift+Enter 换行"));
    m_inputEdit->setMinimumHeight(76); m_inputEdit->setMaximumHeight(150);
    m_inputEdit->installEventFilter(this); inputLayout->addWidget(m_inputEdit);
    auto *toolbar = new QHBoxLayout; toolbar->setSpacing(6);
    m_modeCombo = new QComboBox(inputFrame); m_modeCombo->setObjectName(QStringLiteral("aiChatMode"));
    m_modeCombo->addItem(tr("SSH 代理"), int(ChatMode::SshAgent));
    m_modeCombo->addItem(tr("普通聊天"), int(ChatMode::PlainChat));
    m_modeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_modeCombo->setMinimumWidth(0);
    m_modeCombo->setToolTip(tr("对话模式")); toolbar->addWidget(m_modeCombo, 1);
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this] { refreshModelLabel(); emit chatModeChanged(chatMode()); });
    const auto colors = sessionui::colors(palette());
    m_clearBtn = sessionui::iconButton(sessionui::Glyph::Close, tr("清空当前对话"), colors, inputFrame);
    m_clearBtn->setObjectName(QStringLiteral("aiClear")); toolbar->addWidget(m_clearBtn);
    connect(m_clearBtn, &QToolButton::clicked, this, &AiChatPanel::clearRequested);
#ifdef CUBESHELL_WITH_VOICE
    m_micBtn = new QToolButton(inputFrame); m_micBtn->setText(tr("语音")); m_micBtn->setCheckable(true);
    m_micBtn->setToolTip(tr("开始或停止语音输入")); toolbar->addWidget(m_micBtn);
    m_voiceInput = new VoiceInput(this);
    connect(m_micBtn, &QToolButton::clicked, m_voiceInput, &VoiceInput::toggleRecording);
    connect(m_voiceInput, &VoiceInput::recordingStarted, this, [this] {
        m_micBtn->setChecked(true); m_inputEdit->setPlaceholderText(tr("正在听…"));
    });
    connect(m_voiceInput, &VoiceInput::recordingStopped, this, [this] { m_micBtn->setChecked(false); m_inputEdit->setPlaceholderText(tr("正在识别语音…")); });
    const auto insertVoice = [this](const QString &text) {
        m_inputEdit->setPlainText(text); auto cursor = m_inputEdit->textCursor(); cursor.movePosition(QTextCursor::End); m_inputEdit->setTextCursor(cursor);
    };
    connect(m_voiceInput, &VoiceInput::partialTextRecognized, this, insertVoice);
    connect(m_voiceInput, &VoiceInput::textRecognized, this, [this, insertVoice](const QString &text) {
        insertVoice(text); m_inputEdit->setPlaceholderText(tr("描述运维需求…")); m_inputEdit->setFocus();
    });
    connect(m_voiceInput, &VoiceInput::errorOccurred, this, [this](const QString &error) { m_micBtn->setChecked(false); appendAiMessage(error); });
#endif
    m_sendBtn = new QPushButton(tr("发送"), inputFrame); m_sendBtn->setObjectName(QStringLiteral("aiSend"));
    m_sendBtn->setToolTip(tr("发送 (Enter)")); toolbar->addWidget(m_sendBtn);
    connect(m_sendBtn, &QPushButton::clicked, this, &AiChatPanel::onSend);
    m_stopBtn = new QPushButton(tr("停止"), inputFrame); m_stopBtn->setObjectName(QStringLiteral("aiStop"));
    m_stopBtn->hide(); toolbar->addWidget(m_stopBtn);
    connect(m_stopBtn, &QPushButton::clicked, this, &AiChatPanel::stopRequested);
    inputLayout->addLayout(toolbar); layout->addWidget(inputFrame);
    refreshModelLabel(); applyTheme();
}

AiChatPanel::ChatMode AiChatPanel::chatMode() const { return static_cast<ChatMode>(m_modeCombo->currentData().toInt()); }

void AiChatPanel::setConversationModel(AiConversationModel *model)
{
    if (m_model == model) return;
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model; m_view->setModel(model);
    if (model) {
        connect(model, &AiConversationModel::changed, this, [this] { setBusy(m_model && m_model->isBusy()); });
        connect(model, &QObject::destroyed, this, [this] { setBusy(false); });
    }
    setBusy(model && model->isBusy());
    m_notice->hide();
}

void AiChatPanel::setBusy(bool busy)
{
    m_busy = busy;
    m_sendBtn->setVisible(!busy); m_sendBtn->setEnabled(!busy);
    m_stopBtn->setVisible(busy); m_clearBtn->setEnabled(!busy);
    // Input remains editable so the next question can be prepared while running.
}

void AiChatPanel::appendAiMessage(const QString &message)
{
    m_notice->setText(message); m_notice->setVisible(!message.isEmpty());
}

void AiChatPanel::setStatus(bool connected, const QString &host, const QString &osInfo)
{
    m_statusLabel->setText(chatMode() == ChatMode::PlainChat ? tr("普通聊天")
        : connected ? tr("已连接 · %1").arg(host) : tr("未连接 SSH"));
    m_statusLabel->setToolTip(osInfo);
}

void AiChatPanel::refreshModelLabel()
{
    const auto prefs = AiPreferences::load();
    const QString text = tr("%1 · %2").arg(prefs.model, chatMode() == ChatMode::SshAgent ? tr("SSH 代理") : tr("普通聊天"));
    m_modelLabel->setText(text); m_modelLabel->setToolTip(text);
}

void AiChatPanel::onSend()
{
    if (m_busy) return;
    const auto text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    m_notice->hide(); m_inputEdit->clear(); emit userMessageSent(text);
}

bool AiChatPanel::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            onSend(); return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

void AiChatPanel::applyTheme()
{
    const auto c = sessionui::colors(palette());
    m_modelLabel->setStyleSheet(QStringLiteral("color:%1;").arg(sessionui::css(c.muted)));
    m_notice->setStyleSheet(QStringLiteral("color:%1; background:%2;").arg(sessionui::css(c.error), sessionui::css(c.raised)));
    m_inputEdit->setStyleSheet(QStringLiteral("QPlainTextEdit { border:1px solid %1; border-radius:10px; padding:8px; background:%2; }").arg(sessionui::css(c.border), sessionui::css(c.surface)));
    sessionui::styleIconButton(m_clearBtn, c);
}

void AiChatPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) applyTheme();
}
} // namespace cubeshell
