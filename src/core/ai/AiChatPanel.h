#pragma once

// AiChatPanel.h — AI 助手的连接信息、输入区与会话视图容器。

#include "AiConversationModel.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QFrame;
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

// 同一个面板可切换展示不同会话；记录由会话持有，不随面板切换而清空。
// 只发出发送/停止/清空意图，不直接调度网络或执行命令；外部控制器连接这些信号。
class AiChatPanel : public QWidget {
    Q_OBJECT
public:
    // 模式仅由面板选择和通知，具体请求路由由外部控制器决定。
    enum class ChatMode { PlainChat, SshAgent };
    Q_ENUM(ChatMode)
    explicit AiChatPanel(QWidget *parent = nullptr);
    ChatMode chatMode() const;

    // --- 当前会话与连接信息 ---
    // 非拥有式绑定，可传 nullptr；切换时断开旧模型通知并刷新忙碌状态。
    void setConversationModel(AiConversationModel *model);
    AiConversationModel *conversationModel() const { return m_model; }
    // 运行时切换发送/停止按钮并禁用清空，但允许编辑下一条问题。
    void setBusy(bool busy);
    void appendAiMessage(const QString &message); // 显示轮次之外的错误/提示，不写入会话记录
    void setStatus(bool connected, const QString &host = {}, const QString &osInfo = {});
    void refreshModelLabel(); // 重新读取配置，刷新模型名称与对话模式标签
signals:
    void userMessageSent(const QString &text); // 非空输入去掉首尾空白后发送，输入框随即清空
    void stopRequested();
    void clearRequested();
    void chatModeChanged(cubeshell::AiChatPanel::ChatMode mode);
protected:
    // 统一处理 Enter 发送、Shift+Enter 换行、输入框焦点样式及尺寸变化。
    bool eventFilter(QObject *object, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void onSend();
    void applyTheme();
    // 根据字体、实际折行数和面板高度测量输入区，超出上限后由输入框内部滚动。
    void updateComposerMetrics();
    void updateSendButton();
    QTimer m_composerTimer; // 将输入、换行及布局变化合并到下一轮事件循环测量
    QFrame *m_inputFrame = nullptr;
    QLabel *m_statusLabel, *m_modelLabel, *m_notice;
    QPlainTextEdit *m_inputEdit;
    QComboBox *m_modeCombo;
    QPushButton *m_sendBtn, *m_stopBtn;
    QToolButton *m_clearBtn;
    AiConversationView *m_view;
    QPointer<AiConversationModel> m_model; // 不拥有模型；所属会话关闭时自动失效
    bool m_busy = false;
#ifdef CUBESHELL_WITH_VOICE
    QToolButton *m_micBtn;
    VoiceInput *m_voiceInput; // 面板拥有；识别结果只填入输入框，不自动提交命令
#endif
};
} // namespace cubeshell
