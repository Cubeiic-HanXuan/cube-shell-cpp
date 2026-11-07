#include "AiChatPanel.h"
#include "AiConversationView.h"
#include "AiPreferences.h"
#include "widgets/ConversationWidgets.h"
#ifdef CUBESHELL_WITH_VOICE
#include "VoiceInput.h"
#endif

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace cubeshell {
namespace {
class ChatModeDelegate final : public QStyledItemDelegate {
public:
    explicit ChatModeDelegate(QComboBox *combo) : QStyledItemDelegate(combo->view()), m_combo(combo) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto opt = option; initStyleOption(&opt, index);
        return QSize(opt.fontMetrics.horizontalAdvance(opt.text) + 44, qMax(36, opt.fontMetrics.height() + 16));
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto opt = option; initStyleOption(&opt, index);
        const bool current = index.row() == m_combo->currentIndex();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillRect(opt.rect, opt.palette.brush(QPalette::Base));
        if (current || (opt.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
            painter->setPen(Qt::NoPen); painter->setBrush(opt.palette.brush(QPalette::Highlight));
            painter->drawRoundedRect(opt.rect, 5, 5);
        }
        painter->setFont(opt.font); painter->setPen(opt.palette.color(QPalette::Text));
        painter->drawText(opt.rect.adjusted(10, 0, -30, 0), Qt::AlignLeft | Qt::AlignVCenter, opt.text);
        if (current)
            sessionui::icon(sessionui::Glyph::Check, opt.palette.color(QPalette::Link)).paint(painter,
                QRect(opt.rect.right() - 23, opt.rect.center().y() - 7, 14, 14));
        painter->restore();
    }
private:
    QComboBox *m_combo;
};
} // namespace

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

    auto *composer = new QWidget(this);
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(10, 4, 10, 10);
    auto *inputFrame = m_inputFrame = new QFrame(composer);
    inputFrame->setObjectName(QStringLiteral("aiInputFrame"));
    inputFrame->setProperty("inputFocused", false);
    inputFrame->installEventFilter(this);
    auto *inputLayout = new QVBoxLayout(inputFrame);
    inputLayout->setContentsMargins(14, 12, 12, 6); inputLayout->setSpacing(4);
    m_inputEdit = new QPlainTextEdit(inputFrame);
    m_inputEdit->setObjectName(QStringLiteral("aiInput"));
    m_inputEdit->setPlaceholderText(tr("描述你的运维需求…\nEnter 发送，Shift+Enter 换行"));
    m_inputEdit->setToolTip(tr("Enter 发送 · Shift+Enter 换行"));
    m_inputEdit->setFrameShape(QFrame::NoFrame);
    m_inputEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_inputEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inputEdit->document()->setDocumentMargin(2);
    m_inputEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_inputEdit->setFixedHeight(70);
    m_inputEdit->installEventFilter(this); inputLayout->addWidget(m_inputEdit);
    auto *toolbar = new QHBoxLayout; toolbar->setSpacing(8);
    m_modeCombo = new QComboBox(inputFrame); m_modeCombo->setObjectName(QStringLiteral("aiChatMode"));
    m_modeCombo->addItem(tr("SSH 代理"), int(ChatMode::SshAgent));
    m_modeCombo->addItem(tr("普通聊天"), int(ChatMode::PlainChat));
    m_modeCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modeCombo->setToolTip(tr("对话模式"));
    m_modeCombo->setAccessibleName(tr("对话模式"));
    m_modeCombo->setCursor(Qt::PointingHandCursor);
    auto *modeView = new QListView;
    modeView->setObjectName(QStringLiteral("aiChatModePopup"));
    modeView->setSpacing(2);
    modeView->setTextElideMode(Qt::ElideNone);
    modeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_modeCombo->setView(modeView);
    m_modeCombo->setItemDelegate(new ChatModeDelegate(m_modeCombo));
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this] { refreshModelLabel(); emit chatModeChanged(chatMode()); });
    const auto colors = sessionui::colors(palette());
    m_clearBtn = sessionui::iconButton(sessionui::Glyph::Refresh, tr("清空当前对话"), colors, inputFrame);
    m_clearBtn->setObjectName(QStringLiteral("aiClear")); toolbar->addWidget(m_clearBtn);
    connect(m_clearBtn, &QToolButton::clicked, this, &AiChatPanel::clearRequested);
    toolbar->addStretch(1);
    toolbar->addWidget(m_modeCombo);
#ifdef CUBESHELL_WITH_VOICE
    m_micBtn = sessionui::iconButton(sessionui::Glyph::Microphone, tr("开始或停止语音输入"), colors, inputFrame);
    m_micBtn->setObjectName(QStringLiteral("aiMic")); m_micBtn->setCheckable(true);
    toolbar->addWidget(m_micBtn);
    m_voiceInput = new VoiceInput(this);
    connect(m_micBtn, &QToolButton::clicked, m_voiceInput, &VoiceInput::toggleRecording);
    connect(m_voiceInput, &VoiceInput::stateChanged, this, [this](VoiceInput::State state) {
        m_micBtn->setChecked(state == VoiceInput::State::Recording);
        m_micBtn->setEnabled(state == VoiceInput::State::Idle || state == VoiceInput::State::Recording);
        switch (state) {
        case VoiceInput::State::RequestingPermission: m_inputEdit->setPlaceholderText(tr("正在请求麦克风权限…")); break;
        case VoiceInput::State::Recording: m_inputEdit->setPlaceholderText(tr("正在听…")); break;
        case VoiceInput::State::Processing: m_inputEdit->setPlaceholderText(tr("正在识别语音…")); break;
        case VoiceInput::State::Idle: m_inputEdit->setPlaceholderText(tr("描述你的运维需求…\nEnter 发送，Shift+Enter 换行")); break;
        }
    });
    const auto insertVoice = [this](const QString &text) {
        if (text.trimmed().isEmpty()) return;
        m_inputEdit->setPlainText(text); auto cursor = m_inputEdit->textCursor(); cursor.movePosition(QTextCursor::End); m_inputEdit->setTextCursor(cursor);
    };
    connect(m_voiceInput, &VoiceInput::partialTextRecognized, this, insertVoice);
    connect(m_voiceInput, &VoiceInput::textRecognized, this, [this, insertVoice](const QString &text) {
        insertVoice(text); m_inputEdit->setFocus();
    });
    connect(m_voiceInput, &VoiceInput::errorOccurred, this, &AiChatPanel::appendAiMessage);
#endif
    m_sendBtn = new QPushButton(inputFrame); m_sendBtn->setObjectName(QStringLiteral("aiSend"));
    m_sendBtn->setToolTip(tr("发送 (Enter)")); m_sendBtn->setAccessibleName(tr("发送"));
    m_sendBtn->setCursor(Qt::PointingHandCursor); m_sendBtn->setAutoDefault(false);
    toolbar->addWidget(m_sendBtn);
    connect(m_sendBtn, &QPushButton::clicked, this, &AiChatPanel::onSend);
    m_stopBtn = new QPushButton(inputFrame); m_stopBtn->setObjectName(QStringLiteral("aiStop"));
    m_stopBtn->setToolTip(tr("停止当前任务")); m_stopBtn->setAccessibleName(tr("停止当前任务"));
    m_stopBtn->setCursor(Qt::PointingHandCursor); m_stopBtn->setAutoDefault(false);
    m_stopBtn->hide(); toolbar->addWidget(m_stopBtn);
    connect(m_stopBtn, &QPushButton::clicked, this, &AiChatPanel::stopRequested);
    inputLayout->addLayout(toolbar); composerLayout->addWidget(inputFrame); layout->addWidget(composer);

    m_composerTimer.setSingleShot(true);
    connect(&m_composerTimer, &QTimer::timeout, this, &AiChatPanel::updateComposerMetrics);
    connect(m_inputEdit, &QPlainTextEdit::textChanged, this, [this] {
        updateSendButton(); m_composerTimer.start(0);
    });
    connect(m_inputEdit->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this] { m_composerTimer.start(0); });
#ifdef CUBESHELL_WITH_VOICE
    connect(m_micBtn, &QToolButton::toggled, this, &AiChatPanel::applyTheme);
#endif
    refreshModelLabel(); applyTheme(); updateSendButton();
    m_composerTimer.start(0);
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
    m_sendBtn->setVisible(!busy);
    m_stopBtn->setVisible(busy); m_clearBtn->setEnabled(!busy);
    updateSendButton();
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
    if (object == m_inputEdit && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        m_inputFrame->setProperty("inputFocused", event->type() == QEvent::FocusIn);
        m_inputFrame->style()->unpolish(m_inputFrame);
        m_inputFrame->style()->polish(m_inputFrame);
        m_inputFrame->update();
    }
    if (object == m_inputFrame && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        m_composerTimer.start(0);
    if (object == m_inputFrame && event->type() == QEvent::MouseButtonPress)
        m_inputEdit->setFocus(Qt::MouseFocusReason);
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
    using sessionui::css;
    m_modelLabel->setStyleSheet(QStringLiteral("color:%1;").arg(css(c.muted)));
    m_notice->setStyleSheet(QStringLiteral("color:%1; background:%2;").arg(css(c.error), css(c.raised)));
    m_inputFrame->setStyleSheet(QStringLiteral(
        "QFrame#aiInputFrame { background:%1; border:1px solid %2; border-radius:12px; }"
        "QFrame#aiInputFrame[inputFocused=\"true\"] { border-color:%3; }")
        .arg(css(c.raised), css(c.border), css(c.accent)));
    m_inputEdit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#aiInput { border:0; padding:0; background:transparent; color:%1; selection-background-color:%2; }")
        .arg(css(c.text), css(c.accent)));
    auto inputPalette = m_inputEdit->palette();
    inputPalette.setColor(QPalette::PlaceholderText, c.muted);
    m_inputEdit->setPalette(inputPalette);
    m_modeCombo->setStyleSheet(QStringLiteral(
        "QComboBox#aiChatMode { background:%1; color:%2; border:1px solid %3; border-radius:8px;"
        " min-width:0; min-height:0; padding:2px 10px; combobox-popup:0; }"
        "QComboBox#aiChatMode:hover { background:%4; border-color:%5; }"
        "QComboBox#aiChatMode:focus, QComboBox#aiChatMode:on { border-color:%6; }"
        "QComboBox#aiChatMode:on { background:%4; }"
        "QComboBox#aiChatMode::drop-down { subcontrol-origin:padding; subcontrol-position:top right;"
        " border:0; margin:0; width:24px; }"
        "QComboBox#aiChatMode::down-arrow { width:14px; height:14px; }")
        .arg(css(c.surface), css(c.text), css(c.border), css(c.accentSoft), css(c.muted), css(c.accent)));
    m_modeCombo->view()->setStyleSheet(QStringLiteral(
        "QListView { background:%1; color:%2; border:1px solid %3; border-radius:8px;"
        " padding:4px; outline:0; }")
        .arg(css(c.surface), css(c.text), css(c.border)));
    auto popupPalette = m_modeCombo->view()->palette();
    popupPalette.setColor(QPalette::Base, c.surface);
    popupPalette.setColor(QPalette::Text, c.text);
    popupPalette.setColor(QPalette::Highlight, c.accentSoft);
    popupPalette.setColor(QPalette::HighlightedText, c.text);
    m_modeCombo->view()->setPalette(popupPalette);
    sessionui::styleIconButton(m_clearBtn, c);
#ifdef CUBESHELL_WITH_VOICE
    sessionui::styleIconButton(m_micBtn, c);
    m_micBtn->setIcon(sessionui::icon(sessionui::Glyph::Microphone, m_micBtn->isChecked() ? c.error : c.muted));
#endif
    const QString actionStyle = QStringLiteral(
        "QPushButton { background:%1; border:1px solid transparent; border-radius:9px; padding:0; min-width:0; min-height:0; }"
        "QPushButton:hover { border-color:%2; }"
        "QPushButton:pressed { background:%3; }"
        "QPushButton:focus { border-color:%2; }"
        "QPushButton:disabled { background:%1; }")
        .arg(css(c.accentSoft), css(c.accent), css(c.border));
    m_sendBtn->setStyleSheet(actionStyle); m_stopBtn->setStyleSheet(actionStyle);
    m_sendBtn->setIcon(sessionui::icon(sessionui::Glyph::Send, c.text));
    m_stopBtn->setIcon(sessionui::icon(sessionui::Glyph::Stop, c.error));
    m_composerTimer.start(0);
}

void AiChatPanel::updateSendButton()
{
    m_sendBtn->setEnabled(!m_busy && !m_inputEdit->toPlainText().trimmed().isEmpty());
}

void AiChatPanel::updateComposerMetrics()
{
    const int lineHeight = m_inputEdit->fontMetrics().lineSpacing();
    const int actionSide = qMax(32, fontMetrics().height() + 8);
    const int toolSide = qMax(26, fontMetrics().height() + 6);
    const int iconSide = qBound(16, fontMetrics().height(), 22);
    m_sendBtn->setFixedSize(actionSide, actionSide);
    m_stopBtn->setFixedSize(actionSide, actionSide);
    m_sendBtn->setIconSize(QSize(iconSide, iconSide)); m_stopBtn->setIconSize(QSize(iconSide, iconSide));
    m_clearBtn->setFixedSize(toolSide, toolSide); m_clearBtn->setIconSize(QSize(iconSide, iconSide));
#ifdef CUBESHELL_WITH_VOICE
    m_micBtn->setFixedSize(toolSide, toolSide); m_micBtn->setIconSize(QSize(iconSide, iconSide));
#endif
    // Popup widgets can retain the previous application font after a theme change.
    m_modeCombo->setFont(font());
    m_modeCombo->view()->setFont(font());
    // Let the style account for padding and the arrow; never squeeze the mode label.
    m_modeCombo->setFixedSize(m_modeCombo->sizeHint().expandedTo(QSize(0, toolSide)));
    m_modeCombo->view()->setMinimumWidth(qMax(160, m_modeCombo->sizeHint().width()));
    const auto composerMargins = m_inputFrame->parentWidget()->layout()->contentsMargins();
    setMinimumWidth(qMax(300, m_inputFrame->minimumSizeHint().width() + composerMargins.left() + composerMargins.right()));

    // Keep a compact resting height; grow for wrapped/pasted text, not because
    // the parent layout has spare space. Long input scrolls inside this cap.
    const int minimum = 2 * lineHeight + 4;
    const int maximum = qMax(minimum, qMin(7 * lineHeight + 4, int(height() * .36) - actionSide - 48));
    int preferred = qBound(minimum, m_inputFrame->width() / 5, 4 * lineHeight + 4);
    const int placeholderHeight = m_inputEdit->fontMetrics().boundingRect(
        QRect(0, 0, qMax(40, m_inputEdit->viewport()->width() - 4), 10000),
        Qt::TextWordWrap, m_inputEdit->placeholderText()).height() + 4;
    preferred = qMin(maximum, qMax(preferred, placeholderHeight));
    int lines = 0;
    for (auto block = m_inputEdit->document()->begin(); block.isValid() && lines < 8; block = block.next())
        lines += qMax(1, block.layout() ? block.layout()->lineCount() : 1);
    const int desired = qBound(preferred, lines * lineHeight + 4, maximum);
    if (m_inputEdit->height() != desired) m_inputEdit->setFixedHeight(desired);
}

void AiChatPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) applyTheme();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange)
        m_composerTimer.start(0);
}

void AiChatPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_composerTimer.start(0);
}
} // namespace cubeshell
