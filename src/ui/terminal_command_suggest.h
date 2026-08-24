#pragma once

// terminal_command_suggest.h — 终端命令智能提示控制器。
//
// 对应Python: cube-shell.py::SSHQTermWidget 中提示相关逻辑：
//   - 构造初始化 (L7322-7359)：输入缓冲、80ms 防抖定时器、历史加载、命令索引
//   - eventFilter (L7418-7445)：弹窗可见时拦截 Up/Down/Enter/Esc
//   - _on_term_key_pressed (L7463-7528)：按键分支维护缓冲/显隐弹窗/记录历史
//   - 屏幕提取与提示符剥离 (L7548-7605)
//   - 候选合成 (L7917-7957) / 自动弹出 (L7959-7994) / 定位弹出 (L7996-8031)
//   - 应用候选 (L7701-7746)
//
// Python 侧这些逻辑内嵌在 SSHQTermWidget 子类里；C++ 侧 QTermWidget 不再
// 派生子类，改为独立 QObject 控制器挂到既有 QTermWidget 上（installEventFilter
// + termKeyPressed 信号），不触碰 TerminalDisplay::keyPressEvent 等事件链。

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

#include "suggestion_popup.h"

class QTermWidget;
class QTimer;

namespace cubeshell {

class CommandHistory;

class TerminalCommandSuggest : public QObject {
    Q_OBJECT
public:
    // term: 挂载的终端控件；profileKey: 历史分组键（ssh 配置名，空则 global）。
    // 对应Python: SSHQTermWidget.__init__ 提示相关部分 (L7322-7359)
    //           + _get_history_key (L7607-7612)
    TerminalCommandSuggest(QTermWidget *term, const QString &profileKey,
                           QObject *parent = nullptr);
    ~TerminalCommandSuggest() override;

    // 基于提示符的启发式剥离：从当前光标行提取"真实命令行"。
    // 公开为静态方法便于单测。
    // 对应Python: _extract_command_from_prompt (L7568-7584)
    static QString extractCommandFromPrompt(const QString &lineBeforeCursor);

    // 弹窗可见时拦截 TerminalDisplay 的 Up/Down/Enter/Esc 按键。
    // 对应Python: SSHQTermWidget.eventFilter KeyPress 分支 (L7418-7445)
    bool eventFilter(QObject *obj, QEvent *event) override;

    // 暂停/恢复提示。暂停期间不跟踪输入、不弹候选、不写历史——SSH 连接前
    // 在终端里问密码（TerminalPrompt）时必须暂停：那一次 Enter 会把提示符
    // 那行当成命令写进历史，候选弹窗也不该压在密码提示上。
    void setPaused(bool paused);
    bool paused() const { return m_paused; }

private slots:
    // 终端按键事件（来自 QTermWidget::termKeyPressed）。
    // 对应Python: _on_term_key_pressed (L7463-7528)
    void onTermKeyPressed(QKeyEvent *event);

    // 定时器回调：根据当前输入决定是否显示/更新提示弹窗。
    // 对应Python: _auto_show_suggestions (L7959-7994)
    void autoShowSuggestions();

    // 应用一条候选到终端输入（来自弹窗信号）。
    // 对应Python: _apply_suggestion (L7709-7746)
    void onSuggestionApplied(const QString &kind, const QString &text);

private:
    // 获取光标所在行在光标前的文本（inputMethodQuery 提取）。
    // 对应Python: _current_line_before_cursor (L7548-7566)
    QString currentLineBeforeCursor() const;

    // 用于写入历史命令的命令行提取：优先从屏幕提取，失败再回退到本地缓冲。
    // 对应Python: _get_commandline_for_history (L7586-7595)
    QString commandlineForHistory() const;

    // 从屏幕当前行同步本地输入缓冲，用于修正 Tab 补全等导致的偏差。
    // 对应Python: _sync_input_buffer_from_screen (L7597-7605)
    void syncBufferFromScreen();

    // 提取当前输入最后一个 token（用于 token 级候选替换）。
    // 对应Python: _current_last_token (L7701-7707)
    QString currentLastToken() const;

    // 生成候选列表：历史整行优先 + 静态索引 token 级，跨来源去重 ≤20。
    // 对应Python: _get_suggestion_items (L7917-7957)
    QList<SuggestionItem> suggestionItems(const QString &text) const;

    // 启动防抖定时器，延迟触发候选计算与弹窗显示。
    // 对应Python: _schedule_suggestions (L7907-7915)
    void scheduleSuggestions();

    // 计算候选并在光标附近弹出提示窗口。
    // 对应Python: _show_suggestions_menu (L7996-8031)
    void showSuggestionsMenu();

    // 隐藏提示弹窗并重置本次输入的提示状态。
    // 对应Python: _hide_suggestions_menu (L7897-7905)
    void hideSuggestions();

    QTermWidget *m_term = nullptr;
    QString m_profileKey;                        // 历史分组键（配置名）
    std::unique_ptr<CommandHistory> m_history;   // 每实例一份历史
    // 弹窗会被 ensureParent() 懒 reparent 到主窗口；用 QPointer 防止
    // 主窗口先析构子控件时这里悬空/双重删除。
    QPointer<SuggestionPopup> m_popup;
    QTimer *m_timer = nullptr;                   // 80ms 单发防抖定时器
    QString m_inputBuffer;                       // 尽力而为的本地输入缓冲
    QString m_lastInput;                         // 上次触发弹窗的输入（去重）
    qint64 m_lastDeleteMs = 0;                   // 上次删除键时间戳（毫秒）
    bool m_paused = false;                       // 暂停中（见 setPaused）
};

} // namespace cubeshell
