// AiChatPanel.cpp — see AiChatPanel.h.
//
// 对应Python: core/ai/ai_panel.py

#include "AiChatPanel.h"

#include "AiPreferences.h"
#ifdef CUBESHELL_WITH_VOICE
#include "VoiceInput.h"
#endif

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextBrowser>
#include <QTextCursor>
#include <QVBoxLayout>

namespace cubeshell {

// ---------------------------------------------------------------------------
// CommandCard
// ---------------------------------------------------------------------------

// 对应Python: CommandCard._build_ui
CommandCard::CommandCard(const QString &cmd, const QString &description,
                         RiskLevel riskLevel, QWidget *parent)
    : QFrame(parent)
    , m_cmd(cmd)
{
    setObjectName(QStringLiteral("CommandCard"));
    const QString color = riskLevelColor(riskLevel);
    const QString labelText = riskLevelLabel(riskLevel);

    setStyleSheet(QStringLiteral(
        "#CommandCard {"
        "  border: 1px solid palette(mid);"
        "  border-left: 3px solid %1;"
        "  border-radius: 6px;"
        "  padding: 8px;"
        "  margin: 4px 8px;"
        "}").arg(color));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    // 第一行: 命令 + 风险等级标签
    auto *topRow = new QHBoxLayout();
    auto *cmdLabel = new QLabel(cmd, this);
    cmdLabel->setFont(QFont(QStringLiteral("Courier New"), 12));
    cmdLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    cmdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    topRow->addWidget(cmdLabel, 1);

    auto *riskTag = new QLabel(QStringLiteral(" %1 ").arg(labelText), this);
    riskTag->setStyleSheet(QStringLiteral(
        "background: %1; color: white; border-radius: 3px;"
        "padding: 1px 6px; font-size: 11px;").arg(color));
    riskTag->setFixedHeight(20);
    topRow->addWidget(riskTag);
    layout->addLayout(topRow);

    // 第二行: 描述
    if (!description.isEmpty()) {
        auto *descLabel = new QLabel(description, this);
        descLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: palette(placeholderText);"));
        descLabel->setWordWrap(true);
        layout->addWidget(descLabel);
    }

    // 第三行: 操作按钮
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    auto *copyBtn = new QPushButton(QStringLiteral("复制"), this);
    copyBtn->setFixedHeight(24);
    copyBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: palette(button); border: none; border-radius: 4px;"
        "  padding: 2px 10px; font-size: 11px;"
        "}"
        "QPushButton:hover { background: palette(mid); }"));
    copyBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        // 对应Python: CommandCard._copy_cmd
        if (QClipboard *clipboard = QApplication::clipboard())
            clipboard->setText(m_cmd);
    });
    btnRow->addWidget(copyBtn);

    auto *execBtn = new QPushButton(QStringLiteral("在终端执行"), this);
    execBtn->setFixedHeight(24);
    execBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: white; border: none;"
        "  border-radius: 4px; padding: 2px 10px; font-size: 11px;"
        "}").arg(color));
    execBtn->setCursor(Qt::PointingHandCursor);
    connect(execBtn, &QPushButton::clicked, this, [this]() {
        emit executeClicked(m_cmd);
    });
    btnRow->addWidget(execBtn);

    layout->addLayout(btnRow);
}

// ---------------------------------------------------------------------------
// ThinkingWidget
// ---------------------------------------------------------------------------

// 对应Python: _ThinkingWidget.__init__ / _build_ui
ThinkingWidget::ThinkingWidget(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("ThinkingWidget"));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    mainLayout->setSpacing(0);

    // ─ 头部（可点击） ─
    m_header = new QFrame(this);
    m_header->setCursor(Qt::PointingHandCursor);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(4, 4, 4, 4);
    headerLayout->setSpacing(6);

    auto *iconLabel = new QLabel(QStringLiteral("💭"), m_header);
    iconLabel->setStyleSheet(QStringLiteral("font-size: 14px;"));
    headerLayout->addWidget(iconLabel);

    m_titleLabel = new QLabel(QStringLiteral("深度思考 · 0s"), m_header);
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: bold; color: palette(placeholderText);"));
    headerLayout->addWidget(m_titleLabel);

    headerLayout->addStretch();

    m_arrowLabel = new QLabel(QStringLiteral("\u203a"), m_header);
    m_arrowLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; color: palette(placeholderText);"));
    headerLayout->addWidget(m_arrowLabel);

    mainLayout->addWidget(m_header);

    // ─ 思考内容区（可折叠，默认折叠） ─
    m_contentFrame = new QFrame(this);
    m_contentFrame->setVisible(false);
    auto *contentLayout = new QVBoxLayout(m_contentFrame);
    contentLayout->setContentsMargins(24, 4, 8, 4);
    contentLayout->setSpacing(0);

    m_contentLabel = new QLabel(m_contentFrame);
    m_contentLabel->setWordWrap(true);
    m_contentLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: rgba(180, 180, 180, 0.85); line-height: 1.4;"));
    m_contentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(m_contentLabel);

    mainLayout->addWidget(m_contentFrame);

    setStyleSheet(QStringLiteral(
        "#ThinkingWidget { border-radius: 8px; margin: 2px 8px; }"));

    // 计时器。对应Python: _tick
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        ++m_elapsed;
        updateTitle();
    });
    m_timer.start(1000);
}

// 对应Python: _ThinkingWidget.mousePressEvent — 点击头部切换展开/折叠
void ThinkingWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_header->geometry().contains(event->pos()))
        toggleExpanded();
    QFrame::mousePressEvent(event);
}

// 对应Python: _ThinkingWidget._toggle_expanded
void ThinkingWidget::toggleExpanded()
{
    m_isExpanded = !m_isExpanded;
    m_contentFrame->setVisible(m_isExpanded);
    m_arrowLabel->setText(m_isExpanded ? QStringLiteral("\u02c5")
                                       : QStringLiteral("\u203a"));
}

// 对应Python: _ThinkingWidget._update_title
void ThinkingWidget::updateTitle()
{
    const QString suffix =
        m_isActive ? QStringLiteral("...") : QString();
    m_titleLabel->setText(
        QStringLiteral("深度思考 · %1s%2").arg(m_elapsed).arg(suffix));
}

// 对应Python: _ThinkingWidget.append_thinking
void ThinkingWidget::appendThinking(const QString &text)
{
    if (text.isEmpty())
        return;
    m_thinkingText += text;
    QString display = m_thinkingText;
    display.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    m_contentLabel->setText(display);
}

// 对应Python: _ThinkingWidget.stop
void ThinkingWidget::stopThinking()
{
    m_timer.stop();
    m_isActive = false;
    updateTitle();
    // 如果没有思考内容，隐藏展开箭头
    if (m_thinkingText.trimmed().isEmpty()) {
        m_arrowLabel->setVisible(false);
        m_header->setCursor(Qt::ArrowCursor);
    }
}

// ---------------------------------------------------------------------------
// AiChatPanel
// ---------------------------------------------------------------------------

AiChatPanel::AiChatPanel(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(320);
    resize(380, 600);

    // ─ 流式渲染节流 ─（对应 Python 侧的详细注释）
    // 流式增量到达很快，若每个增量都做全文 Markdown 重解析 + setHtml
    // 全量重绘会造成闪烁与卡顿；用 60ms 单发定时器把多次增量合并成
    // 一次渲染，流结束时再强制 flush 一次。
    m_renderTimer.setInterval(60);
    m_renderTimer.setSingleShot(true);
    connect(&m_renderTimer, &QTimer::timeout,
            this, &AiChatPanel::flushAiRender);

    buildUi();

#ifdef CUBESHELL_WITH_VOICE
    // 语音输入管理器。对应Python: VoiceInputManager 的信号连接
    // 鸿蒙：QtMultimedia 支持不完整，CUBESHELL_WITH_VOICE=OFF 时整体摘除。
    m_voiceInput = new VoiceInput(this);
    connect(m_voiceInput, &VoiceInput::recordingStarted, this, [this]() {
        m_micBtn->setChecked(true);
        m_micBtn->setToolTip(QStringLiteral("录音中... 点击停止"));
        m_inputEdit->clear();
        m_inputEdit->setPlaceholderText(QStringLiteral("🔴 正在听..."));
    });
    connect(m_voiceInput, &VoiceInput::recordingStopped, this, [this]() {
        m_micBtn->setChecked(false);
        m_micBtn->setToolTip(
            QStringLiteral("语音输入（点击开始/停止录音）"));
        m_inputEdit->setPlaceholderText(QStringLiteral("正在完成识别..."));
    });
    connect(m_voiceInput, &VoiceInput::recognitionStarted, this, [this]() {
        m_inputEdit->setPlaceholderText(QStringLiteral("正在识别语音..."));
    });
    connect(m_voiceInput, &VoiceInput::partialTextRecognized,
            this, [this](const QString &text) {
        m_inputEdit->setPlainText(text);
        QTextCursor cursor = m_inputEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_inputEdit->setTextCursor(cursor);
    });
    connect(m_voiceInput, &VoiceInput::textRecognized,
            this, [this](const QString &text) {
        m_inputEdit->setPlaceholderText(QStringLiteral("输入运维需求提示词"));
        if (!text.isEmpty()) {
            m_inputEdit->setPlainText(text);
            QTextCursor cursor = m_inputEdit->textCursor();
            cursor.movePosition(QTextCursor::End);
            m_inputEdit->setTextCursor(cursor);
            m_inputEdit->setFocus();
        }
    });
    connect(m_voiceInput, &VoiceInput::errorOccurred,
            this, [this](const QString &msg) {
        m_micBtn->setChecked(false);
        m_inputEdit->setPlaceholderText(QStringLiteral("输入运维需求提示词"));
        appendAiMessage(QStringLiteral("🎤 %1").arg(msg));
    });
#endif // CUBESHELL_WITH_VOICE
}

// 对应Python: AIChatPanel._build_ui
void AiChatPanel::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ─ 顶部状态栏 ─
    auto *statusBar = new QFrame(this);
    statusBar->setObjectName(QStringLiteral("StatusBar"));
    auto *statusLayout = new QVBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 8, 12, 8);
    statusLayout->setSpacing(2);

    m_statusLabel = new QLabel(QStringLiteral("⚪ 未连接"), statusBar);
    m_statusLabel->setStyleSheet(
        QStringLiteral("font-size: 13px; font-weight: bold;"));
    statusLayout->addWidget(m_statusLabel);

    m_modelLabel = new QLabel(buildModelLabelText(), statusBar);
    m_modelLabel->setStyleSheet(QStringLiteral("font-size: 11px;"));
    statusLayout->addWidget(m_modelLabel);

    mainLayout->addWidget(statusBar);

    // ─ 对话区域 ─
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setObjectName(QStringLiteral("ChatScrollArea"));

    m_chatContainer = new QWidget();
    m_chatLayout = new QVBoxLayout(m_chatContainer);
    m_chatLayout->setContentsMargins(4, 8, 4, 8);
    m_chatLayout->setSpacing(8);
    m_chatLayout->addStretch();     // 使消息靠底

    m_scrollArea->setWidget(m_chatContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    // ─ 输入区域 ─
    auto *inputFrame = new QFrame(this);
    inputFrame->setObjectName(QStringLiteral("inputFrame"));
    auto *inputVbox = new QVBoxLayout(inputFrame);
    inputVbox->setContentsMargins(8, 8, 8, 8);
    inputVbox->setSpacing(6);

    // 上层：多行输入框
    m_inputEdit = new QPlainTextEdit(inputFrame);
    m_inputEdit->setPlaceholderText(
        QStringLiteral("输入运维需求，输入 '/' 获取更多能力"));
    m_inputEdit->setFixedHeight(80);
    m_inputEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_inputEdit->installEventFilter(this);
    inputVbox->addWidget(m_inputEdit);

    // 下层：工具栏
    auto *toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    // 上下文模式选择（C++ 新增：普通聊天 / SSH 代理）
    m_modeCombo = new QComboBox(inputFrame);
    m_modeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modeCombo->setMinimumWidth(140);
    m_modeCombo->addItem(QStringLiteral("SSH 代理"),
                         QVariant::fromValue(int(ChatMode::SshAgent)));
    m_modeCombo->addItem(QStringLiteral("普通聊天"),
                         QVariant::fromValue(int(ChatMode::PlainChat)));
    m_modeCombo->setToolTip(QStringLiteral("上下文模式"));
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this]() {
        emit chatModeChanged(chatMode());
    });
    toolbarLayout->addWidget(m_modeCombo);

    toolbarLayout->addStretch();

#ifdef CUBESHELL_WITH_VOICE
    // 麦克风按钮
    m_micBtn = new QPushButton(QStringLiteral("🎤"), inputFrame);
    m_micBtn->setFixedSize(32, 32);
    m_micBtn->setCheckable(true);
    m_micBtn->setToolTip(QStringLiteral("语音输入（点击开始/停止录音）"));
    m_micBtn->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; border-radius: 16px; }"
        "QPushButton:hover { background: palette(midlight); }"
        "QPushButton:checked { background: #F44336; border-color: #F44336; }"));
    m_micBtn->setCursor(Qt::PointingHandCursor);
    connect(m_micBtn, &QPushButton::clicked,
            this, &AiChatPanel::onMicClicked);
    toolbarLayout->addWidget(m_micBtn);
#endif // CUBESHELL_WITH_VOICE

    // 发送按钮（绿色 ↑ 样式）
    m_sendBtn = new QPushButton(QStringLiteral("↑"), inputFrame);
    m_sendBtn->setFixedSize(32, 32);
    m_sendBtn->setToolTip(QStringLiteral("发送 (Enter)"));
    m_sendBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #4CAF50; color: white; border-radius: 8px;"
        "  font-size: 18px; font-weight: bold;"
        "}"
        "QPushButton:hover { background: #43A047; }"
        "QPushButton:pressed { background: #388E3C; }"
        "QPushButton:disabled { background: palette(mid); }"));
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    connect(m_sendBtn, &QPushButton::clicked, this, &AiChatPanel::onSend);
    toolbarLayout->addWidget(m_sendBtn);

    // 停止按钮
    m_stopBtn = new QPushButton(QStringLiteral("停止"), inputFrame);
    m_stopBtn->setFixedSize(50, 32);
    m_stopBtn->setCursor(Qt::PointingHandCursor);
    m_stopBtn->setVisible(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &AiChatPanel::onStop);
    toolbarLayout->addWidget(m_stopBtn);

    inputVbox->addLayout(toolbarLayout);
    mainLayout->addWidget(inputFrame);
}

AiChatPanel::ChatMode AiChatPanel::chatMode() const
{
    return static_cast<ChatMode>(m_modeCombo->currentData().toInt());
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

// 对应Python: AIChatPanel.append_user_message
void AiChatPanel::appendUserMessage(const QString &text)
{
    QTextBrowser *bubble = createBubble(text, true);
    insertWidget(bubble);
    scrollToBottom();
}

// 对应Python: AIChatPanel.append_ai_message
void AiChatPanel::appendAiMessage(const QString &text)
{
    // 先结束上一轮可能仍在等待渲染的流式气泡，避免 pending flush 落到新气泡上
    finalizeAiRender();
    QTextBrowser *bubble = createBubble(text, false);
    m_currentAiBubble = bubble;
    m_currentAiText = text;
    m_lastBubbleHeight = 0;
    insertWidget(bubble);
    scrollToBottom();
}

// 对应Python: AIChatPanel.append_ai_delta
void AiChatPanel::appendAiDelta(const QString &reasoning, const QString &content)
{
    // reasoning 内容路由到思考面板
    if (!reasoning.isEmpty() && m_thinkingWidget)
        m_thinkingWidget->appendThinking(reasoning);

    if (content.isEmpty())
        return;

    if (!m_currentAiBubble) {
        m_currentAiText.clear();
        m_lastBubbleHeight = 0;
        QTextBrowser *bubble = createBubble(QString(), false);
        m_currentAiBubble = bubble;
        insertWidget(bubble);
    }

    // 只累积文本，真正的渲染交给节流定时器合并处理
    m_currentAiText += content;
    m_renderDirty = true;
    if (!m_renderTimer.isActive())
        m_renderTimer.start();
}

// 对应Python: AIChatPanel._flush_ai_render
void AiChatPanel::flushAiRender()
{
    if (!m_renderDirty || !m_currentAiBubble)
        return;
    m_renderDirty = false;

    const QString html = m_renderer.render(m_currentAiText);
    // 内联主题文字颜色
    const QString textColor = palette().color(QPalette::Text).name();
    m_currentAiBubble->setHtml(
        QStringLiteral("<div style=\"color:%1;\">%2</div>")
            .arg(textColor, html));
    // 更新文档宽度以适配当前面板尺寸
    m_currentAiBubble->document()->setTextWidth(bubbleAvailableWidth(false));
    // 仅在高度真正变化时才调整固定高度，避免每次都触发父布局 reflow 抖动
    const int newHeight = qMax(
        36, int(m_currentAiBubble->document()->size().height()) + 16);
    if (newHeight != m_lastBubbleHeight) {
        m_lastBubbleHeight = newHeight;
        m_currentAiBubble->setFixedHeight(newHeight);
    }
    scrollToBottom();
}

// 对应Python: AIChatPanel._finalize_ai_render
void AiChatPanel::finalizeAiRender()
{
    m_renderTimer.stop();
    if (m_renderDirty)
        flushAiRender();
}

// 对应Python: AIChatPanel.append_command_card
void AiChatPanel::appendCommandCard(const QString &cmd,
                                    const QString &description,
                                    RiskLevel riskLevel)
{
    auto *card = new CommandCard(cmd, description, riskLevel, m_chatContainer);
    connect(card, &CommandCard::executeClicked,
            this, &AiChatPanel::commandExecuteRequested);
    finalizeAiRender();
    insertWidget(card);
    m_currentAiBubble = nullptr;
    scrollToBottom();
}

// 对应Python: AIChatPanel.append_execution_result
void AiChatPanel::appendExecutionResult(const QString &cmd, int exitCode,
                                        const QString &output,
                                        const QString &description)
{
    auto *frame = new QFrame(m_chatContainer);
    frame->setObjectName(QStringLiteral("ExecResult"));
    const bool success = (exitCode == 0);
    const QString borderColor = success ? QStringLiteral("#4caf50")
                                        : QStringLiteral("#f44336");
    frame->setStyleSheet(QStringLiteral(
        "#ExecResult {"
        "  border: 1px solid %1; border-radius: 6px;"
        "  margin: 4px 8px; padding: 8px;"
        "}").arg(borderColor));

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    // 状态行
    const QString icon = success ? QStringLiteral("✅") : QStringLiteral("❌");
    auto *statusLabel = new QLabel(
        QStringLiteral("%1 执行%2 (exit_code: %3)")
            .arg(icon, success ? QStringLiteral("成功")
                               : QStringLiteral("失败"))
            .arg(exitCode),
        frame);
    statusLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1; font-weight: bold;").arg(borderColor));
    layout->addWidget(statusLabel);

    // 命令说明（如果有）
    if (!description.isEmpty()) {
        auto *descLabel = new QLabel(
            QStringLiteral("# %1").arg(description), frame);
        descLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: #888; font-style: italic;"));
        descLabel->setWordWrap(true);
        layout->addWidget(descLabel);
    }

    // 命令（支持换行、可选中复制）
    auto *cmdLabel = new QLabel(QStringLiteral("$ %1").arg(cmd), frame);
    cmdLabel->setFont(QFont(QStringLiteral("Courier New"), 11));
    cmdLabel->setWordWrap(true);
    cmdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cmdLabel->setCursor(Qt::IBeamCursor);
    layout->addWidget(cmdLabel);

    // 输出（最多显示 20 行）
    if (!output.isEmpty()) {
        auto *outputBrowser = new QTextBrowser(frame);
        outputBrowser->setFont(QFont(QStringLiteral("Courier New"), 11));
        outputBrowser->setStyleSheet(QStringLiteral(
            "QTextBrowser {"
            "  background: #263238; color: #e0e0e0;"
            "  border-radius: 4px; padding: 6px; border: none;"
            "}"));
        const QStringList lines = output.split(QLatin1Char('\n'));
        QString displayText =
            QStringList(lines.mid(0, 20)).join(QLatin1Char('\n'));
        if (lines.size() > 20)
            displayText += QStringLiteral("\n... (共 %1 行)").arg(lines.size());
        outputBrowser->setPlainText(displayText);
        // 动态高度
        const int shownLines = qMin(20, int(lines.size()));
        const int docHeight = qMin(200, qMax(40, shownLines * 16 + 20));
        outputBrowser->setFixedHeight(docHeight);
        layout->addWidget(outputBrowser);
    }

    finalizeAiRender();
    insertWidget(frame);
    m_currentAiBubble = nullptr;
    scrollToBottom();
}

// 对应Python: AIChatPanel.append_task_summary
void AiChatPanel::appendTaskSummary(const QString &summary)
{
    auto *frame = new QFrame(m_chatContainer);
    frame->setObjectName(QStringLiteral("TaskSummary"));

    // 根据内容判断颜色：包含"失败"用橙色，否则绿色
    QString bgColor, borderColor, icon;
    if (summary.contains(QStringLiteral("失败"))) {
        bgColor = QStringLiteral("#fff3e0");
        borderColor = QStringLiteral("#ff9800");
        icon = QStringLiteral("⚠️");
    } else {
        bgColor = QStringLiteral("#e8f5e9");
        borderColor = QStringLiteral("#4caf50");
        icon = QStringLiteral("✅");
    }

    frame->setStyleSheet(QStringLiteral(
        "#TaskSummary {"
        "  background: %1; border: 1px solid %2; border-radius: 6px;"
        "  margin: 6px 8px; padding: 10px 12px;"
        "}").arg(bgColor, borderColor));

    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *label = new QLabel(
        QStringLiteral("%1 %2").arg(icon, summary), frame);
    label->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: #333; font-weight: bold;"
        "background: transparent;"));
    label->setWordWrap(true);
    layout->addWidget(label);

    finalizeAiRender();
    insertWidget(frame);
    m_currentAiBubble = nullptr;
    scrollToBottom();
}

// 对应Python: AIChatPanel.append_diagnosing_hint
void AiChatPanel::appendDiagnosingHint()
{
    appendAiMessage(QStringLiteral(
        "检测到命令执行失败，正在自动分析原因并生成修复方案..."));
}

// 对应Python: AIChatPanel.set_status
void AiChatPanel::setStatus(bool connected, const QString &host,
                            const QString &osInfo)
{
    if (connected)
        m_statusLabel->setText(QStringLiteral("🟢 已连接 %1").arg(host));
    else
        m_statusLabel->setText(QStringLiteral("⚪ 未连接"));
    if (!osInfo.isEmpty())
        m_modelLabel->setText(buildModelLabelText(osInfo));
}

// 对应Python: AIChatPanel.set_thinking
void AiChatPanel::setThinking(bool isThinking)
{
    if (isThinking) {
        if (!m_thinkingWidget) {
            m_thinkingWidget = new ThinkingWidget(m_chatContainer);
            insertWidget(m_thinkingWidget);
            scrollToBottom();
        }
        m_stopBtn->setVisible(true);
    } else {
        if (m_thinkingWidget)
            m_thinkingWidget->stopThinking();   // 保留为可折叠的已完成状态
        m_stopBtn->setVisible(false);
        finalizeAiRender();
        m_currentAiBubble = nullptr;
        m_thinkingWidget = nullptr;             // 组件保留在布局中
    }
}

// 对应Python: AIChatPanel.set_executing
void AiChatPanel::setExecuting(bool isExecuting, const QString &progressText,
                               int current, int total)
{
    m_stopBtn->setVisible(isExecuting);
    if (isExecuting) {
        if (current > 0 && total > 0) {
            m_modelLabel->setText(QStringLiteral("执行中 [%1/%2] $ %3")
                                      .arg(current)
                                      .arg(total)
                                      .arg(progressText));
        } else if (!progressText.isEmpty()) {
            m_modelLabel->setText(
                QStringLiteral("执行中 $ %1").arg(progressText));
        } else {
            m_modelLabel->setText(QStringLiteral("命令执行中..."));
        }
    } else {
        m_modelLabel->setText(buildModelLabelText());
    }
}

// 对应Python: AIChatPanel.update_command_output
void AiChatPanel::updateCommandOutput(const QString &outputLine)
{
    if (outputLine.isEmpty())
        return;
    // 截取显示（状态栏空间有限）
    const QString display =
        outputLine.length() > 80 ? outputLine.right(80) : outputLine;
    m_modelLabel->setText(QStringLiteral("⬇ %1").arg(display));
}

// 对应Python: AIChatPanel.refresh_model_label
void AiChatPanel::refreshModelLabel()
{
    if (m_modelLabel)
        m_modelLabel->setText(buildModelLabelText());
}

// 对应Python: AIChatPanel._build_model_label_text
QString AiChatPanel::buildModelLabelText(const QString &osInfo) const
{
    QString modelName = QStringLiteral("GLM-4");
    const AiPreferences prefs = AiPreferences::load();
    if (!prefs.model.isEmpty())
        modelName = prefs.model;
    const QString base = QStringLiteral("%1 | SSH 模式").arg(modelName);
    if (!osInfo.isEmpty())
        return QStringLiteral("%1 | %2").arg(base, osInfo);
    return base;
}

// ---------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------

// 对应Python: AIChatPanel._on_send
void AiChatPanel::onSend()
{
    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    m_inputEdit->clear();
    appendUserMessage(text);
    emit userMessageSent(text);
}

// 对应Python: AIChatPanel.eventFilter — Enter 发送、Shift+Enter 换行
bool AiChatPanel::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Return
             || keyEvent->key() == Qt::Key_Enter)
            && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            onSend();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// 对应Python: AIChatPanel._on_mic_clicked
void AiChatPanel::onMicClicked()
{
#ifdef CUBESHELL_WITH_VOICE
    if (m_voiceInput)
        m_voiceInput->toggleRecording();
#endif
    // CUBESHELL_WITH_VOICE=OFF：麦克风按钮未创建，本函数不会被触发。
}

// 对应Python: AIChatPanel._on_stop
void AiChatPanel::onStop()
{
    emit stopRequested();
}

// 对应Python: AIChatPanel._insert_widget — 插入到 stretch 前面
void AiChatPanel::insertWidget(QWidget *widget)
{
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, widget);
}

// 对应Python: AIChatPanel._scroll_to_bottom（延迟滚动确保布局更新后）
void AiChatPanel::scrollToBottom()
{
    QTimer::singleShot(50, this, [this]() {
        if (QScrollBar *vbar = m_scrollArea->verticalScrollBar())
            vbar->setValue(vbar->maximum());
    });
}

// 对应Python: AIChatPanel.resizeEvent
void AiChatPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, &AiChatPanel::relayoutBubbles);
}

// 对应Python: AIChatPanel._relayout_bubbles
void AiChatPanel::relayoutBubbles()
{
    const int widthAi = bubbleAvailableWidth(false);
    const int widthUser = bubbleAvailableWidth(true);

    for (int i = 0; i < m_chatLayout->count(); ++i) {
        QLayoutItem *item = m_chatLayout->itemAt(i);
        if (!item)
            continue;
        auto *browser = qobject_cast<QTextBrowser *>(item->widget());
        if (!browser)
            continue;
        // 判断是用户消息还是 AI 消息（通过 margin 样式）
        const bool isAi =
            browser->styleSheet().contains(QStringLiteral("40px 2px 8px"));
        browser->document()->setTextWidth(isAi ? widthAi : widthUser);
        const int docHeight = int(browser->document()->size().height()) + 16;
        browser->setFixedHeight(qMax(36, docHeight));
    }
}

// 对应Python: AIChatPanel._get_bubble_available_width
int AiChatPanel::bubbleAvailableWidth(bool isUser) const
{
    int viewportW = m_scrollArea->viewport()->width();
    if (viewportW <= 0)
        viewportW = width() - 20;   // fallback
    if (viewportW <= 0)
        viewportW = 300;            // 最终 fallback

    // 减去 chat_layout 的左右 margins (4+4)
    viewportW -= 8;
    // 减去气泡样式中的 margin（用户: 8+40, AI: 40+8）和 padding (12+12)
    viewportW -= (8 + 40 + 24);

    return qMax(200, viewportW);
}

// 对应Python: AIChatPanel._create_bubble
QTextBrowser *AiChatPanel::createBubble(const QString &text, bool isUser)
{
    auto *bubble = new QTextBrowser(m_chatContainer);
    bubble->setOpenExternalLinks(true);
    bubble->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bubble->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bubble->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    bubble->setFont(QFont(QStringLiteral("sans-serif"), 13));

    // 使用 palette 获取主题自适应颜色
    const QPalette pal = palette();
    QString bgHex, margin;
    if (isUser) {
        // 用户消息：使用 highlight 色的低透明度版本
        const QColor highlight = pal.color(QPalette::Highlight);
        bgHex = QStringLiteral("rgba(%1, %2, %3, 0.15)")
                    .arg(highlight.red())
                    .arg(highlight.green())
                    .arg(highlight.blue());
        margin = QStringLiteral("2px 8px 2px 40px");
    } else {
        // AI 消息：使用 Window 色的半透明版本
        const QColor windowColor = pal.color(QPalette::Window);
        bgHex = QStringLiteral("rgba(%1, %2, %3, 0.5)")
                    .arg(windowColor.red())
                    .arg(windowColor.green())
                    .arg(windowColor.blue());
        margin = QStringLiteral("2px 40px 2px 8px");
    }

    bubble->setStyleSheet(QStringLiteral(
        "QTextBrowser {"
        "  background: %1; border-radius: 8px;"
        "  padding: 8px 12px; border: none; margin: %2;"
        "}").arg(bgHex, margin));

    // 通过 palette 获取当前主题文字颜色，直接内联到 HTML
    const QString textColor = pal.color(QPalette::Text).name();
    QString html;
    if (isUser) {
        // 用户消息只做 HTML 转义 + 换行（对应 Python _escape_html 全流程）
        html = MarkdownRenderer::escapeHtml(text);
        html.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    } else {
        html = m_renderer.render(text);
    }
    bubble->setHtml(QStringLiteral("<div style=\"color:%1;\">%2</div>")
                        .arg(textColor, html));

    // 根据滚动区域实际可用宽度计算文档宽度和高度
    bubble->document()->setTextWidth(bubbleAvailableWidth(isUser));
    const int docHeight = int(bubble->document()->size().height()) + 16;
    bubble->setFixedHeight(qMax(36, docHeight));
    return bubble;
}

} // namespace cubeshell
