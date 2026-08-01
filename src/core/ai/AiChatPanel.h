#pragma once

// AiChatPanel.h — AI SSH ops assistant chat panel (embeddable QWidget).
//
// 对应Python: core/ai/ai_panel.py::AIChatPanel / CommandCard /
//             _ThinkingWidget
//
// 结构：顶部状态栏（连接状态 + 模型名）→ 滚动对话区（QTextBrowser 气泡 /
// 命令卡片 / 执行结果 / 任务总结）→ 输入区（多行输入框 + 上下文模式选择
// + 麦克风 + 发送 + 停止）。
//
// 流式渲染节流：增量 token 只累积文本，60ms 单发定时器合并后一次
// Markdown 渲染 + setHtml，流结束强制 flush（对应 Python 侧注释）。
//
// 上下文模式选择（普通聊天 / SSH 代理）为 C++ 版新增下拉框（任务要求），
// 通过 chatMode()/chatModeChanged 暴露给宿主窗口路由到
// AiChatWorker（普通聊天）或 SshAiAgent（SSH 代理）。

#include "CommandSafetyChecker.h"
#include "MarkdownRenderer.h"

#include <QFrame>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

namespace cubeshell {

class VoiceInput;

// 单条命令卡片，显示命令、风险等级、描述及操作按钮。
// 对应Python: ai_panel.py::CommandCard
class CommandCard : public QFrame {
    Q_OBJECT
public:
    CommandCard(const QString &cmd, const QString &description,
                RiskLevel riskLevel, QWidget *parent = nullptr);

signals:
    void executeClicked(const QString &cmd);    // execute_clicked

private:
    QString m_cmd;
};

// AI 深度思考可折叠面板，显示思考过程和耗时。
// 对应Python: ai_panel.py::_ThinkingWidget
class ThinkingWidget : public QFrame {
    Q_OBJECT
public:
    explicit ThinkingWidget(QWidget *parent = nullptr);

    // 对应Python: append_thinking
    void appendThinking(const QString &text);
    // 对应Python: stop — 停止计时，标记思考结束
    void stopThinking();

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void toggleExpanded();
    void updateTitle();

    QFrame *m_header = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_arrowLabel = nullptr;
    QFrame *m_contentFrame = nullptr;
    QLabel *m_contentLabel = nullptr;
    QTimer m_timer;
    QString m_thinkingText;
    int m_elapsed = 0;
    bool m_isExpanded = false;
    bool m_isActive = true;
};

// AI SSH 运维助手对话面板。
// 对应Python: ai_panel.py::AIChatPanel
class AiChatPanel : public QWidget {
    Q_OBJECT
public:
    // 上下文模式（C++ 版新增，任务要求：普通聊天 / SSH 代理）
    enum class ChatMode {
        PlainChat,
        SshAgent,
    };
    Q_ENUM(ChatMode)

    explicit AiChatPanel(QWidget *parent = nullptr);

    ChatMode chatMode() const;

    // --- 公共接口（对应 Python 同名方法） ---

    void appendUserMessage(const QString &text);        // append_user_message
    void appendAiMessage(const QString &text);          // append_ai_message
    void appendAiDelta(const QString &reasoning,
                       const QString &content);         // append_ai_delta
    void appendCommandCard(const QString &cmd, const QString &description,
                           RiskLevel riskLevel);        // append_command_card
    void appendExecutionResult(const QString &cmd, int exitCode,
                               const QString &output,
                               const QString &description = QString()); // append_execution_result
    void appendTaskSummary(const QString &summary);     // append_task_summary
    void appendDiagnosingHint();                        // append_diagnosing_hint

    void setStatus(bool connected, const QString &host = QString(),
                   const QString &osInfo = QString());  // set_status
    void setThinking(bool isThinking);                  // set_thinking
    void setExecuting(bool isExecuting,
                      const QString &progressText = QString(),
                      int current = 0, int total = 0);  // set_executing
    void updateCommandOutput(const QString &outputLine); // update_command_output
    void refreshModelLabel();                           // refresh_model_label

signals:
    // 对应Python: AIChatPanel 的同名信号集
    void userMessageSent(const QString &text);          // user_message_sent
    void commandExecuteRequested(const QString &cmd);   // command_execute_requested
    void stopRequested();                               // stop_requested
    void clearRequested();                              // clear_requested
    void chatModeChanged(cubeshell::AiChatPanel::ChatMode mode); // C++ 新增

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();                     // 对应Python: _build_ui
    void onSend();                      // _on_send
    void onStop();                      // _on_stop
    void onMicClicked();                // _on_mic_clicked
    void flushAiRender();               // _flush_ai_render
    void finalizeAiRender();            // _finalize_ai_render
    void insertWidget(QWidget *widget); // _insert_widget
    void scrollToBottom();              // _scroll_to_bottom
    void relayoutBubbles();             // _relayout_bubbles
    int bubbleAvailableWidth(bool isUser) const; // _get_bubble_available_width
    QTextBrowser *createBubble(const QString &text, bool isUser); // _create_bubble
    QString buildModelLabelText(const QString &osInfo = QString()) const; // _build_model_label_text

    // 顶部状态栏
    QLabel *m_statusLabel = nullptr;
    QLabel *m_modelLabel = nullptr;

    // 对话区
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_chatContainer = nullptr;
    QVBoxLayout *m_chatLayout = nullptr;

    // 输入区
    QPlainTextEdit *m_inputEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QPushButton *m_micBtn = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;

    // 流式渲染状态
    QTextBrowser *m_currentAiBubble = nullptr;
    QString m_currentAiText;
    ThinkingWidget *m_thinkingWidget = nullptr;
    QTimer m_renderTimer;               // 60ms 单发节流
    bool m_renderDirty = false;
    int m_lastBubbleHeight = 0;

    MarkdownRenderer m_renderer;
    VoiceInput *m_voiceInput = nullptr;
};

} // namespace cubeshell
