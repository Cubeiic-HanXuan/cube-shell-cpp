#include "terminal_command_suggest.h"

// terminal_command_suggest.cpp — 终端命令智能提示控制器实现。
// 对应Python: cube-shell.py::SSHQTermWidget 提示相关逻辑（见头文件映射表）。

#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include "qtermwidget.h"
#include "TerminalDisplay.h"

#include "terminal/CommandHistory.h"
#include "terminal/CommandIndex.h"

namespace cubeshell {

namespace {

// 进程级共享的静态命令索引（首次使用时加载一次，所有终端共用）。
// 对应Python: load_linux_commands() 结果在每个 SSHQTermWidget 里各存一份
// (L7346-7357)；C++ 侧索引不可变，共享以省内存与启动开销。
const CommandIndex &sharedCommandIndex()
{
    static const CommandIndex index = [] {
        CommandIndex ci;
        ci.load();
        return ci;
    }();
    return index;
}

// 对应Python: str.isprintable()（空串为 False，全部字符可打印为 True）
bool isPrintableText(const QString &text)
{
    if (text.isEmpty())
        return false;
    for (const QChar &c : text) {
        if (!c.isPrint())
            return false;
    }
    return true;
}

// 对应Python: str.lstrip()
QString lstripped(const QString &s)
{
    int i = 0;
    while (i < s.size() && s.at(i).isSpace())
        ++i;
    return s.mid(i);
}

} // namespace

// 对应Python: SSHQTermWidget.__init__ 提示相关初始化 (L7322-7359)
TerminalCommandSuggest::TerminalCommandSuggest(QTermWidget *term,
                                               const QString &profileKey,
                                               QObject *parent)
    : QObject(parent)
    , m_term(term)
    , m_profileKey(profileKey)
    , m_history(std::make_unique<CommandHistory>())
{
    // 历史底层数据同进程按文件路径共享（多 Tab 不互相覆盖），
    // 磁盘文件与 Python 侧共用同一 command_history.json。
    // 对应Python: self._history_data = self._load_history_data() (L7336-7340)
    m_history->load();

    // 80ms 单发防抖定时器 (L7330-7332)
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &TerminalCommandSuggest::autoShowSuggestions);

    // 候选弹窗（owner=term，用于懒 reparent 到主窗口）(L7333)
    m_popup = new SuggestionPopup(term);
    connect(m_popup, &SuggestionPopup::suggestionApplied,
            this, &TerminalCommandSuggest::onSuggestionApplied);

    // 拦截 TerminalDisplay 按键（仅弹窗可见时消费导航键，不影响其他 filter）。
    // 对应Python: self.m_impl.m_terminalDisplay.installEventFilter(self) (L7303-7304)
    if (auto *display = m_term->terminalDisplay())
        display->installEventFilter(this);

    // 问题2修复：弹窗被 reparent 到主窗口，不随 Tab 页面自动隐藏；
    // 监听终端控件自身的 Hide 事件（切换 Tab/关闭页面时触发）以收回弹窗。
    m_term->installEventFilter(this);

    // 对应Python: self.termKeyPressed.connect(self._on_term_key_pressed) (L7342)
    connect(m_term, &QTermWidget::termKeyPressed,
            this, &TerminalCommandSuggest::onTermKeyPressed);
}

TerminalCommandSuggest::~TerminalCommandSuggest()
{
    // 弹窗可能已被 reparent 到主窗口；QPointer 保证不双重删除。
    delete m_popup;
}

// 弹窗可见时拦截"导航/选择"按键；隐藏时所有按键交给终端。
// 对应Python: SSHQTermWidget.eventFilter KeyPress 分支 (L7418-7445)
bool TerminalCommandSuggest::eventFilter(QObject *obj, QEvent *event)
{
    // 问题2修复：终端所在 Tab 被切走/隐藏时收回弹窗（弹窗挂在主窗口上，
    // 不会随 Tab 页面一起隐藏）；同时停掉待触发的防抖定时器避免再次弹出。
    // HideToParent 处理终端被嵌套在容器内、由祖先隐藏连带触发的情况。
    if (obj == m_term && (event->type() == QEvent::Hide
                          || event->type() == QEvent::HideToParent)) {
        m_timer->stop();
        hideSuggestions();
        return false;   // 不消费事件，让终端正常处理隐藏
    }

    if (obj == m_term->terminalDisplay() && event->type() == QEvent::KeyPress
        && m_popup && m_popup->isVisible()) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Up:
            m_popup->selectPrev();
            m_popup->scrollToCurrentItem();
            return true;
        case Qt::Key_Down:
            m_popup->selectNext();
            m_popup->scrollToCurrentItem();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            // 有用户选择 → 应用并消费回车；无选择 → 只收弹窗，回车放行给终端。
            const bool applied = m_popup->applyCurrentIfSelected();
            hideSuggestions();
            return applied;
        }
        case Qt::Key_Escape:
            hideSuggestions();
            return true;
        default:
            break;
        }
    }
    return QObject::eventFilter(obj, event);
}

// 终端按键事件：只做与智能提示相关的"轻量输入跟踪"——
// 维护 m_inputBuffer（尽力而为）、控制弹窗显隐、记录历史命令。
// 对应Python: _on_term_key_pressed (L7463-7528)
void TerminalCommandSuggest::onTermKeyPressed(QKeyEvent *event)
{
    // alternate screen（vim/less/top 等）时禁用提示 (L7473-7475)
    if (m_term->isAppScreenMode()) {
        hideSuggestions();
        return;
    }

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();

    // Ctrl+Space 手动唤起提示 (L7480-7482)
    if ((mods & Qt::ControlModifier) && key == Qt::Key_Space) {
        showSuggestionsMenu();
        return;
    }

    // 回车：记录历史（屏幕提取优先）、清缓冲、收弹窗 (L7484-7490)
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        const QString cmdline = commandlineForHistory();
        if (!cmdline.isEmpty())
            m_history->addEntry(cmdline, m_profileKey);
        m_inputBuffer.clear();
        hideSuggestions();
        return;
    }

    // 长按删除键会产生高频重复事件；此时持续计算/刷新提示会明显卡顿。
    // 直接隐藏弹窗并暂停提示计算，保证终端输入删除顺滑 (L7492-7498)
    if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        m_inputBuffer.chop(1);
        m_lastDeleteMs = QDateTime::currentMSecsSinceEpoch();
        hideSuggestions();
        return;
    }

    if (key == Qt::Key_Escape) {
        hideSuggestions();
        return;
    }

    // 光标移动/翻页类按键：本地缓冲无法跟踪，直接收弹窗 (L7504-7508)
    if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up
        || key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End
        || key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
        hideSuggestions();
        return;
    }

    // 带 Ctrl/Alt/Meta 修饰的组合键：不是普通输入 (L7510-7512)
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        hideSuggestions();
        return;
    }

    const QString text = event->text();
    if (isPrintableText(text)) {
        // 本地维护一个"尽力而为"的输入缓冲用于轻量提示。
        // 当远端 shell 自己做 Tab 补全时，本地缓冲可能偏离，稍后会从屏幕同步一次。
        m_inputBuffer += text;
        if (text == QLatin1String(" ")) {
            hideSuggestions();
            return;
        }
        // 距上次删除 >250ms 才触发提示计算 (L7522-7523)
        if (QDateTime::currentMSecsSinceEpoch() - m_lastDeleteMs > 250)
            scheduleSuggestions();
    } else if (key == Qt::Key_Tab && mods == Qt::NoModifier) {
        // Tab 补全由远端 shell 完成；等待屏幕更新后，从渲染行同步本地缓冲 (L7524-7526)
        QTimer::singleShot(60, this, &TerminalCommandSuggest::syncBufferFromScreen);
    }
}

// 获取光标所在行在光标前的文本。
// 用于在远端 shell 通过 Tab 等方式修改输入后，从屏幕同步出"真实输入"。
// 对应Python: _current_line_before_cursor (L7548-7566)
QString TerminalCommandSuggest::currentLineBeforeCursor() const
{
    auto *display = m_term->terminalDisplay();
    if (!display)
        return QString();
    // inputMethodQuery 在 TerminalDisplay 里重声明为 protected，但在 QWidget
    // 基类是 public virtual；通过基类指针调用以免改动 qtermwidget。
    const QWidget *w = display;
    const QString line = w->inputMethodQuery(Qt::ImSurroundingText).toString();
    bool ok = false;
    int cursorX = w->inputMethodQuery(Qt::ImCursorPosition).toInt(&ok);
    if (!ok)
        cursorX = line.size();
    if (cursorX < 0)
        cursorX = 0;
    return line.left(cursorX);
}

// 基于提示符的启发式剥离：从当前光标行提取"真实命令行"。
// 当输入被远端 shell 功能（例如 Tab 补全）修改时，这能显著提升历史记录准确性。
// 对应Python: _extract_command_from_prompt (L7568-7584)
QString TerminalCommandSuggest::extractCommandFromPrompt(const QString &lineBeforeCursor)
{
    QString s = lineBeforeCursor;
    while (s.endsWith(QLatin1Char('\n')) || s.endsWith(QLatin1Char('\r')))
        s.chop(1);
    if (s.isEmpty())
        return QString();
    static const QStringList markers = {
        QStringLiteral("$ "), QStringLiteral("# "), QStringLiteral("> "),
        QStringLiteral("❯ "), QStringLiteral("➜ "),
    };
    int best = -1;
    int bestLen = 0;
    for (const QString &m : markers) {
        const int i = s.lastIndexOf(m);
        if (i > best) {
            best = i;
            bestLen = m.size();
        }
    }
    if (best >= 0)
        return s.mid(best + bestLen).trimmed();
    return s.trimmed();
}

// 用于写入历史命令的命令行提取：优先从屏幕提取，失败再回退到本地缓冲。
// 对应Python: _get_commandline_for_history (L7586-7595)
QString TerminalCommandSuggest::commandlineForHistory() const
{
    const QString cmd = extractCommandFromPrompt(currentLineBeforeCursor());
    if (!cmd.isEmpty())
        return cmd;
    return m_inputBuffer.trimmed();
}

// 从屏幕当前行同步本地输入缓冲，用于修正 Tab 补全等导致的偏差。
// 对应Python: _sync_input_buffer_from_screen (L7597-7605)
void TerminalCommandSuggest::syncBufferFromScreen()
{
    const QString cmd = extractCommandFromPrompt(currentLineBeforeCursor());
    if (!cmd.isEmpty())
        m_inputBuffer = cmd;
}

// 提取当前输入最后一个 token（用于 token 级候选替换）。
// 对应Python: _current_last_token (L7701-7707)
QString TerminalCommandSuggest::currentLastToken() const
{
    const QString &s = m_inputBuffer;
    if (s.isEmpty() || s.endsWith(QLatin1Char(' ')) || s.endsWith(QLatin1Char('\t')))
        return QString();
    static const QRegularExpression re(QStringLiteral("(\\S+)$"));
    const QRegularExpressionMatch m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 生成候选列表：1) 历史命令（整行）优先；2) 静态索引候选（token 级）。
// 跨来源去重，总数 ≤20。
// 对应Python: _get_suggestion_items (L7917-7957)
QList<SuggestionItem> TerminalCommandSuggest::suggestionItems(const QString &text) const
{
    const QString s = lstripped(text);
    QList<SuggestionItem> items;
    QSet<QString> seen;

    const QStringList hist = m_history->suggestions(s, m_profileKey);
    for (const QString &h : hist) {
        if (seen.contains(h))
            continue;
        seen.insert(h);
        items.append({QStringLiteral("history"), h});
        if (items.size() >= 20)
            return items;
    }

    const QStringList sugg = sharedCommandIndex().computeSuggestions(s);
    QString lastToken;
    if (!s.isEmpty() && !s.endsWith(QLatin1Char(' ')) && !s.endsWith(QLatin1Char('\t'))) {
        static const QRegularExpression re(QStringLiteral("(\\S+)$"));
        const QRegularExpressionMatch m = re.match(s);
        if (m.hasMatch())
            lastToken = m.captured(1);
    }

    // 末 token 过滤；过滤后为空则回退全量 (L7943-7947)
    QStringList candidates;
    if (!lastToken.isEmpty()) {
        for (const QString &x : sugg) {
            if (x.startsWith(lastToken))
                candidates << x;
        }
    }
    if (candidates.isEmpty())
        candidates = sugg;

    for (const QString &x : candidates) {
        if (seen.contains(x))
            continue;
        seen.insert(x);
        items.append({QStringLiteral("token"), x});
        if (items.size() >= 20)
            break;
    }
    return items;
}

// 启动防抖定时器，延迟触发候选计算与弹窗显示。
// 对应Python: _schedule_suggestions (L7907-7915)
void TerminalCommandSuggest::scheduleSuggestions()
{
    if (m_term->isAppScreenMode())
        return;
    m_timer->start(80);
}

// 定时器回调：根据当前输入决定是否显示/更新提示弹窗。
// 对应Python: _auto_show_suggestions (L7959-7994)
void TerminalCommandSuggest::autoShowSuggestions()
{
    // 用户正在弹窗上交互（鼠标悬停）时不刷新，避免候选跳动 (L7962-7964)
    if (m_popup && m_popup->isVisible() && m_popup->isInteracting())
        return;
    if (m_term->isAppScreenMode()) {
        hideSuggestions();
        return;
    }

    // 终端失焦时不弹提示 (L7969-7977)
    auto *display = m_term->terminalDisplay();
    const bool displayHasFocus = display && display->hasFocus();
    if (!(m_term->hasFocus() || displayHasFocus)) {
        hideSuggestions();
        return;
    }

    const QString text = lstripped(m_inputBuffer);
    if (text.isEmpty()) {
        hideSuggestions();
        return;
    }

    const QList<SuggestionItem> items = suggestionItems(text);
    if (items.isEmpty()) {
        hideSuggestions();
        return;
    }

    // 文本与上次相同且弹窗已可见 → 无需重复弹出 (L7989-7990)
    if (text == m_lastInput && m_popup && m_popup->isVisible())
        return;
    m_lastInput = text;
    showSuggestionsMenu();
}

// 计算候选并在光标附近弹出提示窗口。
// 对应Python: _show_suggestions_menu (L7996-8031)
void TerminalCommandSuggest::showSuggestionsMenu()
{
    const QString text = lstripped(m_inputBuffer);
    const QList<SuggestionItem> items = suggestionItems(text);
    if (items.isEmpty()) {
        hideSuggestions();
        return;
    }
    if (!m_popup)
        return;
    m_popup->updateSuggestions(items);

    // 使用 mapTo 计算相对于主窗口的坐标，绕开 Wayland 下 mapToGlobal 失效问题。
    auto *display = m_term->terminalDisplay();
    m_popup->ensureParent();
    QWidget *parent = m_popup->parentWidget();
    // inputMethodQuery 通过 QWidget 基类指针调用（见 currentLineBeforeCursor）。
    const QRect rect = display
        ? static_cast<const QWidget *>(display)
              ->inputMethodQuery(Qt::ImCursorRectangle).toRect()
        : QRect();
    if (display && rect.isValid()) {
        QPoint p = parent ? display->mapTo(parent, rect.bottomLeft())
                          : display->mapToGlobal(rect.bottomLeft());
        // 增加 6px 垂直偏移，避免弹窗紧贴光标 (L8022-8023)
        p.setY(p.y() + 6);

        // 问题1修复：光标在终端底部时，下方空间不足以容纳弹窗，
        // 改为翻到光标上方弹出；同时做水平方向防溢出，保证弹窗完整可见。
        if (parent) {
            const int popupH = m_popup->height();
            const int popupW = m_popup->width();
            if (p.y() + popupH > parent->height()) {
                const QPoint above = display->mapTo(parent, rect.topLeft());
                p.setY(qMax(0, above.y() - popupH - 2));
            }
            if (p.x() + popupW > parent->width())
                p.setX(qMax(0, parent->width() - popupW));
            if (p.x() < 0)
                p.setX(0);
        }
        m_popup->popupAt(p);
    } else {
        // 光标矩形不可用时回退到鼠标位置 (L8025-8031)
        if (parent)
            m_popup->popupAt(parent->mapFromGlobal(QCursor::pos()));
        else
            m_popup->popupAt(QCursor::pos());
    }
}

// 应用一条候选到终端输入。
// 规则：kind=history → 替换整行输入（先退格清空，再写入完整历史命令）；
//       kind=token   → 替换最后一个 token（退格删除 token，再写入候选）。
// 对应Python: _apply_suggestion (L7709-7746)
void TerminalCommandSuggest::onSuggestionApplied(const QString &kind, const QString &text)
{
    if (text.isEmpty())
        return;

    QString buf = m_inputBuffer;

    if (kind == QLatin1String("history")) {
        // "\x7f"×len 逐字符退格清空当前行 (L7726-7728)
        if (!buf.isEmpty())
            m_term->sendText(QString(buf.size(), QChar(0x7f)));
        m_term->sendText(text);
        m_inputBuffer = text;
        return;
    }

    const QString lastToken = currentLastToken();
    const int eraseLen = lastToken.size();
    if (eraseLen > 0) {
        m_term->sendText(QString(eraseLen, QChar(0x7f)));
        buf.chop(eraseLen);
    }
    m_term->sendText(text);
    m_inputBuffer = buf + text;

    // 命令补全后自动追加空格（整行只有一个 token 且候选是已知命令时）(L7741-7744)
    const QString stripped = m_inputBuffer.trimmed();
    if (!stripped.contains(QLatin1Char(' '))
        && sharedCommandIndex().commands().contains(text)) {
        m_term->sendText(QStringLiteral(" "));
        m_inputBuffer += QLatin1Char(' ');
    }
}

// 隐藏提示弹窗并重置本次输入的提示状态。
// 对应Python: _hide_suggestions_menu (L7897-7905)
void TerminalCommandSuggest::hideSuggestions()
{
    if (m_popup)
        m_popup->hide();
    m_lastInput.clear();
}

} // namespace cubeshell
