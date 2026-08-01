#pragma once

// suggestion_popup.h — 终端智能提示候选弹窗（非激活式）。
//
// 对应Python: cube-shell.py::_SuggestionPopup (L7083-7290)
//           + _SuggestionDelegate (L7042-7080)
//
// 设计目标（与 Python 侧一致）：
// - 展示补全候选但不抢占终端焦点，避免 QMenu 抢焦点导致的闪烁与输入卡顿；
// - 支持鼠标选择与键盘上下选择；
// - 默认不选中任何候选，避免用户直接回车执行命令时误触发补全；
// - 懒 reparent 到主窗口作为子控件定位，绕开 Linux/Wayland 下
//   mapToGlobal() 坐标失效问题。
//
// 与 Python 差异：Python 里点击/回车直接回调 owner._apply_suggestion，
// C++ 侧改为发 suggestionApplied(kind, text) 信号由控制器
// （TerminalCommandSuggest）处理，避免 UI 与逻辑互相依赖。

#include <QFrame>
#include <QList>
#include <QString>
#include <QStringList>

class QListWidget;

namespace cubeshell {

// 一条候选项。kind: "history"（历史整行）| "token"（命令/选项 token）。
// 对应Python: items 里的 {kind: ..., text: ...} dict (L7163)
struct SuggestionItem {
    QString kind;
    QString text;
};

class SuggestionPopup : public QFrame {
    Q_OBJECT
public:
    // owner 为终端控件（Python 里的 SSHQTermWidget self），仅用于
    // ensureParent() 时定位主窗口，不作为 Qt parent。
    // 对应Python: _SuggestionPopup.__init__ (L7084-7143)
    explicit SuggestionPopup(QWidget *owner);

    // 更新候选列表内容（截 ≤20 条；签名未变且可见时跳过重建）。
    // 对应Python: updateSuggestions (L7159-7202)
    void updateSuggestions(const QList<SuggestionItem> &items);

    // 是否存在用户显式选择的候选（鼠标点击或上下键导航）。
    // 对应Python: hasUserSelection (L7204-7211)
    bool hasUserSelection() const;

    // 键盘向下/向上选择候选（首次导航分别从 0 / count-1 起）。
    // 对应Python: selectNext/selectPrev (L7213-7235)
    void selectNext();
    void selectPrev();

    // 将当前选中项滚入可视区（配合 selectNext/selectPrev 使用）。
    // 对应Python: eventFilter 中 popup.list.scrollToItem(it) (L7426-7434)
    void scrollToCurrentItem();

    // 仅当用户显式选中过候选时应用当前候选（发信号并 hide）。
    // 返回 true 表示已应用（调用方需消费回车事件）。
    // 对应Python: applyCurrentIfSelected (L7237-7254)
    bool applyCurrentIfSelected();

    // 是否处于用户交互状态（鼠标悬停在候选弹窗内）。
    // 对应Python: isInteracting (L7155-7157)
    bool isInteracting() const { return m_interacting; }

    // 确保弹窗是主窗口的子控件（懒初始化，只执行一次）。
    // 对应Python: _ensure_parent (L7256-7274)
    void ensureParent();

    // 在指定坐标弹出（坐标相对于父控件/主窗口）。
    // 对应Python: popupAt (L7276-7281)
    void popupAt(const QPoint &pos);

signals:
    // 用户确认了一条候选（点击或回车）；由控制器执行实际补全。
    // 对应Python: self._owner._apply_suggestion(payload) (L7252/L7288)
    void suggestionApplied(const QString &kind, const QString &text);

protected:
    // 鼠标移入/移出时维护交互状态，用于暂停候选自动刷新。
    // 对应Python: enterEvent/leaveEvent (L7145-7153)
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QWidget *m_owner = nullptr;
    QListWidget *m_list = nullptr;
    bool m_interacting = false;
    bool m_hasUserSelection = false;
    bool m_reparented = false;
    QStringList m_sig;   // 候选签名缓存（kind+text 序列），未变则跳过重建
};

} // namespace cubeshell
