/*
    Copyright (C) 2008 e_k (e_k@users.sourceforge.net)

    This file is part of Konsole / QTermWidget.

    Ported from the Python PySide6 version (qtermwidget/qtermwidget.py), which
    was itself converted from upstream QTermWidget 2.2.0 (qtermwidget.cpp/h).
*/

#pragma once

// qtermwidget.h — C++ port of qtermwidget/qtermwidget.py
//
// Public terminal widget facade. Wraps a private implementation
// (TermWidgetImpl) that owns a Session plus a TerminalDisplay view, and
// re-exposes the full public API that cube-shell calls. Note the lowercase
// file name, matching upstream QTermWidget.

#include <QFont>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <QSize>
#include <QPoint>
#include <QList>

#include <memory>

#include "ColorScheme.h"      // ColorEntry, ColorSchemeManager, TABLE_COLORS
#include "Emulation.h"        // Konsole::KeyboardCursorShape
#include "Filter.h"           // Konsole::Filter::HotSpot
#include "TerminalDisplay.h"  // Konsole::ScrollBarPosition 等枚举 + 视图类型

class QAction;
class QKeyEvent;
class QResizeEvent;
class QVBoxLayout;
class QIODevice;
class QTranslator;

namespace Konsole {
class Session;
class TerminalDisplay;
class SearchBar;
class ScreenWindow;
}

// 滚动条位置枚举 — 直接复用 TerminalDisplay.h 里 Konsole 命名空间下的定义,
// 避免重复定义造成二义性。顶层 QTermWidget 是无命名空间的外壳类,这里用
// using 声明把 Konsole::ScrollBarPosition 引入全局命名空间。
// 对应C++: enum ScrollBarPosition { NoScrollBar = 0, ScrollBarLeft = 1, ScrollBarRight = 2 };
using Konsole::ScrollBarPosition;
using Konsole::NoScrollBar;
using Konsole::ScrollBarLeft;
using Konsole::ScrollBarRight;

// 平台相关的默认字体家族 — 对应 Python 的 DEFAULT_FONT_FAMILY。
#if defined(Q_OS_MAC)
inline const QString QTERMWIDGET_DEFAULT_FONT_FAMILY = QStringLiteral("Menlo");
#else
inline const QString QTERMWIDGET_DEFAULT_FONT_FAMILY = QStringLiteral("Monospace");
#endif

// 缩放步长 — 对应 Python 的 STEP_ZOOM。
inline constexpr int QTERMWIDGET_STEP_ZOOM = 1;

// 前向声明内部实现类。定义在 .cpp 中(Pimpl 惯用法),避免把 Session /
// TerminalDisplay 的完整定义拉进公共头文件。
// 对应C++: struct TermWidgetImpl
struct TermWidgetImpl;

/**
 * QTermWidget - 主终端部件类
 *
 * 对外公开的外壳 widget,内部持有 Session + TerminalDisplay。
 *
 * 对应C++: class QTermWidget : public QWidget, public QTermWidgetInterface
 */
class QTermWidget : public QWidget
{
    Q_OBJECT

public:
    // 键盘光标形状 — 对应 Python `from .emulation import KeyboardCursorShape`。
    using KeyboardCursorShape = Konsole::KeyboardCursorShape;

    // 对应C++: QTermWidget(int startnow = 1, QWidget *parent = nullptr);
    explicit QTermWidget(int startnow = 1, QWidget *parent = nullptr);
    // 对应C++: ~QTermWidget() override;
    ~QTermWidget() override;

    // cube-shell 钩子:访问内部 TerminalDisplay(Python: m_impl.m_terminalDisplay)。
    // 对应C++: TerminalDisplay* terminalDisplay()  (cube-shell 新增访问器)
    Konsole::TerminalDisplay *terminalDisplay() const;
    // 访问内部 Session(cube-shell 桥接里会用到 emulation()/onReceiveBlock)。
    Konsole::Session *session() const;

    // 终端是否处于 alternate screen(vim/less/top 等全屏 TUI)，
    // 用于禁用智能命令提示；任一级为空时返回 false。
    // 对应Python: cube-shell.py::_should_disable_command_suggestions (L7530-7546)
    bool isAppScreenMode() const;

    // 尺寸提示 — 对应 Python sizeHint()。
    QSize sizeHint() const override;

    // 对应C++: void setTerminalSizeHint(bool enabled)
    void setTerminalSizeHint(bool enabled);
    // 对应C++: bool terminalSizeHint() const
    bool terminalSizeHint() const;

    // 对应C++: void startShellProgram()
    void startShellProgram();
    // 对应C++: void startTerminalTeletype()
    void startTerminalTeletype();

    // 对应C++: int getShellPID()
    int getShellPID();
    // 对应C++: int getForegroundProcessId()
    int getForegroundProcessId();
    // 对应 Python getIsRunning()。
    bool getIsRunning();

    // 对应C++: void changeDir(const QString& dir)
    void changeDir(const QString &dir);

    // 对应C++: void setTerminalFont(const QFont& font)
    void setTerminalFont(const QFont &font);
    // 对应C++: QFont getTerminalFont() const
    QFont getTerminalFont() const;

    // 对应C++: void setTerminalOpacity(qreal level)
    void setTerminalOpacity(qreal level);
    // 对应C++: void setTerminalBackgroundImage(const QString& backgroundImage)
    void setTerminalBackgroundImage(const QString &backgroundImage);
    // 对应C++: void setTerminalBackgroundMode(int mode)
    void setTerminalBackgroundMode(int mode);
    // 对应C++: void setSuppressProgramBackgroundColors(bool suppress)
    void setSuppressProgramBackgroundColors(bool suppress);

    // 对应C++: void setShellProgram(const QString& program)
    void setShellProgram(const QString &program);
    // 对应C++: void setWorkingDirectory(const QString& dir)
    void setWorkingDirectory(const QString &dir);
    // 对应C++: QString workingDirectory()
    QString workingDirectory();
    // 对应C++: void setArgs(const QStringList& args)
    void setArgs(const QStringList &args);

    // 对应C++: void setColorScheme(const QString& origName)
    void setColorScheme(const QString &origName);
    // 对应 Python getAvailableColorSchemes()。
    QStringList getAvailableColorSchemes() const;
    // 对应C++: static QStringList availableColorSchemes()
    static QStringList availableColorSchemes();
    // 对应C++: static void addCustomColorSchemeDir(const QString& custom_dir)
    static void addCustomColorSchemeDir(const QString &customDir);

    // 对应C++: void setHistorySize(int lines)
    void setHistorySize(int lines);
    // 对应C++: int historySize()
    int historySize();

    // 对应C++: void setScrollBarPosition(ScrollBarPosition pos)
    void setScrollBarPosition(ScrollBarPosition pos);
    // 对应C++: void scrollToEnd()
    void scrollToEnd();

    // 对应C++: void sendText(const QString& text)
    void sendText(const QString &text);
    // 对应C++: void sendKeyEvent(QKeyEvent* e)
    void sendKeyEvent(QKeyEvent *e);

    // 对应C++: void setFlowControlEnabled(bool enabled)
    void setFlowControlEnabled(bool enabled);
    // 对应C++: bool flowControlEnabled()
    bool flowControlEnabled();
    // 对应C++: void setFlowControlWarningEnabled(bool enabled)
    void setFlowControlWarningEnabled(bool enabled);

    // 对应C++: QString keyBindings()
    QString keyBindings();

    // 对应C++: void setMotionAfterPasting(int motion)
    void setMotionAfterPasting(int motion);

    // 对应C++: int historyLinesCount()
    int historyLinesCount();
    // 对应C++: int screenColumnsCount()
    int screenColumnsCount();
    // 对应C++: int screenLinesCount()
    int screenLinesCount();

    // 对应C++: void setSelectionStart(int row, int column)
    void setSelectionStart(int row, int column);
    // 对应C++: void setSelectionEnd(int row, int column)
    void setSelectionEnd(int row, int column);
    // 对应C++: void getSelectionStart(int& row, int& column)
    void getSelectionStart(int &row, int &column);
    // 对应C++: void getSelectionEnd(int& row, int& column)
    void getSelectionEnd(int &row, int &column);
    // 对应C++: QString selectedText(bool preserveLineBreaks = true)
    QString selectedText(bool preserveLineBreaks = true);
    // 清除选择 — cube-shell 公共 API。
    void clearSelection();

    // 查找文本(cube-shell 公共 API)。封装搜索栏 + HistorySearch 逻辑。
    // 返回是否找到匹配。
    bool findText(const QString &text, bool forwards = true, bool caseSensitive = false);

    // 对应C++: void setMonitorActivity(bool enabled)
    void setMonitorActivity(bool enabled);
    // 对应C++: void setMonitorSilence(bool enabled)
    void setMonitorSilence(bool enabled);
    // 对应C++: void setSilenceTimeout(int seconds)
    void setSilenceTimeout(int seconds);

    // 对应C++: QList<QAction*> filterActions(const QPoint& position)
    QList<QAction *> filterActions(const QPoint &position);

    // 对应C++: int getPtySlaveFd()
    int getPtySlaveFd();

    // 对应C++: void setBlinkingCursor(bool blink)
    void setBlinkingCursor(bool blink);

    // 对应C++: void setBidiEnabled(bool enabled)
    void setBidiEnabled(bool enabled);
    // 对应C++: bool isBidiEnabled()
    bool isBidiEnabled();

    // 对应C++: void setAutoClose(bool enabled)
    void setAutoClose(bool enabled);

    // 对应C++: QString title() const
    QString title() const;
    // 对应C++: QString icon() const
    QString icon() const;
    // 对应C++: bool isTitleChanged() const
    bool isTitleChanged();

    // 对应C++: void bracketText(QString& text)
    void bracketText(QString &text);
    // 对应C++: void disableBracketedPasteMode(bool disable)
    void disableBracketedPasteMode(bool disable);
    // 对应C++: bool bracketedPasteModeIsDisabled()
    bool bracketedPasteModeIsDisabled();

    // 对应C++: void setMargin(int margin)
    void setMargin(int margin);
    // 对应C++: int getMargin()
    int getMargin();

    // 对应C++: void setDrawLineChars(bool drawLineChars)
    void setDrawLineChars(bool drawLineChars);
    // 对应C++: void setBoldIntense(bool boldIntense)
    void setBoldIntense(bool boldIntense);
    // 对应C++: void setConfirmMultilinePaste(bool confirmMultilinePaste)
    void setConfirmMultilinePaste(bool confirmMultilinePaste);
    // 对应C++: void setTrimPastedTrailingNewlines(bool trimPastedTrailingNewlines)
    void setTrimPastedTrailingNewlines(bool trimPastedTrailingNewlines);

    // 对应C++: QString wordCharacters()
    QString wordCharacters();
    // 对应C++: void setWordCharacters(const QString& chars)
    void setWordCharacters(const QString &chars);

    // 对应C++: QTermWidget* createWidget(int startnow)
    QTermWidget *createWidget(int startnow);
    // 对应C++: void autoHideMouseAfter(int delay)
    void autoHideMouseAfter(int delay);

    // 对应C++: void setEnvironment(const QStringList& environment)
    void setEnvironment(const QStringList &environment);

    // 对应C++: static QStringList availableKeyBindings()
    static QStringList availableKeyBindings();

    // 对应 Python getHotSpotAt()。
    Konsole::Filter::HotSpot *getHotSpotAt(const QPoint &pos);
    Konsole::Filter::HotSpot *getHotSpotAt(int row, int column);

    // 对应C++: void setKeyboardCursorShape(KeyboardCursorShape shape)
    void setKeyboardCursorShape(KeyboardCursorShape shape);

    // 显式关闭终端组件 — 对应 Python close()。安全终止会话与进程。
    // 建议父窗口 closeEvent 中调用。
    void closeTerminal();

public Q_SLOTS:
    // 对应C++: void copyClipboard()
    void copyClipboard();
    // 对应C++: void pasteClipboard()
    void pasteClipboard();
    // 对应C++: void pasteSelection()
    void pasteSelection();
    // 对应C++: void zoomIn()
    void zoomIn();
    // 对应C++: void zoomOut()
    void zoomOut();
    // 对应C++: void setSize(const QSize& size)
    void setSize(const QSize &size);
    // 对应C++: void setKeyBindings(const QString& kb)
    void setKeyBindings(const QString &kb);
    // 对应C++: void clear()
    void clear();
    // 对应C++: void toggleShowSearchBar()
    void toggleShowSearchBar();
    // 对应C++: void saveHistory(QIODevice* device)
    void saveHistory(QIODevice *device);

Q_SIGNALS:
    // 对应C++: void finished()
    void finished();
    // 对应C++: void copyAvailable(bool)
    void copyAvailable(bool);
    // 对应C++: void termGetFocus()
    void termGetFocus();
    // 对应C++: void termLostFocus()
    void termLostFocus();
    // 对应C++: void termKeyPressed(QKeyEvent*)
    void termKeyPressed(QKeyEvent *);
    // 对应C++: void urlActivated(const QUrl&, bool fromContextMenu)
    void urlActivated(const QUrl &url, bool fromContextMenu);
    // 对应C++: void bell(const QString&)
    void bell(const QString &message);
    // 对应C++: void activity()
    void activity();
    // 对应C++: void silence()
    void silence();
    // 对应C++: void sendData(const char*, int)  — startTerminalTeletype 重定向用
    void sendData(const char *data, int length);
    // 对应C++: void profileChanged(const QString&)
    void profileChanged(const QString &profileName);
    // 对应C++: void titleChanged()
    void titleChanged();
    // 对应C++: void receivedData(const QString&)
    void receivedData(const QString &text);
    // 对应 Python destroyed()。Qt 的 QObject::destroyed(QObject*) 已存在,
    // 这里保留无参版本以贴合 Python API。
    void terminalDestroyed();
    // 右键菜单点击"AI" — 请求主窗口打开/聚焦 AI 助手面板。
    // 对应 Python: self.window()._toggle_ai_panel()
    void aiRequested();
    // Ctrl/Cmd+滚轮缩放后的新字号 — 供上层同步到主题配置。
    // 对应Python: cube-shell.py::zoom_in/zoom_out 中 util.THEME['font_size'] 的更新
    void fontSizeChanged(int pointSize);

protected:
    // 对应 Python resizeEvent()。
    void resizeEvent(QResizeEvent *event) override;
    // 对应 Python closeEvent()。
    void closeEvent(QCloseEvent *event) override;

private Q_SLOTS:
    // 对应C++: void sessionFinished()
    void sessionFinished();
    // 对应C++: void selectionChanged(bool)
    void selectionChanged(bool textSelected);
    // 对应C++: void find()
    void find();
    // 对应C++: void findNext()
    void findNext();
    // 对应C++: void findPrevious()
    void findPrevious();
    // 对应C++: void matchFound(int, int, int, int)
    void matchFound(int startColumn, int startLine, int endColumn, int endLine);
    // 对应C++: void noMatchFound()
    void noMatchFound();
    // 对应 Python cursorChanged()。
    void cursorChanged(Konsole::KeyboardCursorShape cursorShape, bool blinkingCursorEnabled);
    // 终端右键菜单 — 响应 TerminalDisplay::configureRequest。
    // 对应 Python contextMenuEvent()/_add_custom_actions()。
    void showContextMenu(const QPoint &pos);

private:
    void init(int startnow);
    void search(bool forwards, bool next);
    void setZoom(int step);
    void setupTranslations();
    void connectSessionSignals();
    void setupUrlFilter();
    void setupCustomFilters();
    void setupSearchBar();
    void connectTerminalDisplaySignals();
    void setupDefaultFont();
    void connectSessionEvents();

    std::unique_ptr<TermWidgetImpl> m_impl;
    QVBoxLayout *m_layout = nullptr;
    Konsole::SearchBar *m_searchBar = nullptr;
    QTranslator *m_translator = nullptr;
};

// 对应C++: void* createTermWidget(int startnow = 1, void* parent = nullptr)
// 工厂函数,供需要 C 兼容入口的调用方使用。
QTermWidget *createTermWidget(int startnow = 1, QWidget *parent = nullptr);
