#pragma once

// Session.h — C++ port of qtermwidget/session.py
//
// A terminal session: owns a Pty shell process + an Emulation + zero or more
// views (TerminalDisplay). Runs the shell, forwards data pty<->emulation,
// monitors activity/silence, and handles title/notify/done signals.
//
// Original copyright:
//   Copyright (C) 2006-2007 by Robert Knight <robertknight@gmail.com>
//   Copyright (C) 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QString>
#include <QStringDecoder>
#include <QStringList>
#include <QTimer>

#include "Emulation.h"   // NOTIFYNORMAL etc., KeyboardCursorShape
#include "History.h"     // HistoryType

class QKeyEvent;

namespace Konsole {

class Emulation;
class Pty;
class TerminalDisplay;

// 视图阈值常量 - 对应C++: SESSION_VIEW_*_THRESHOLD
inline constexpr int SESSION_VIEW_LINES_THRESHOLD   = 2;
inline constexpr int SESSION_VIEW_COLUMNS_THRESHOLD = 2;

// 对应C++: class Session : public QObject
class Session : public QObject {
    Q_OBJECT

    // 对应C++: Q_PROPERTY(QString name READ nameTitle)
    Q_PROPERTY(QString name READ nameTitle)
    // 对应C++: Q_PROPERTY(int processId READ processId)
    Q_PROPERTY(int processId READ processId)
    // 对应C++: Q_PROPERTY(QString keyBindings READ keyBindings WRITE setKeyBindings)
    Q_PROPERTY(QString keyBindings READ keyBindings WRITE setKeyBindings)
    // 对应C++: Q_PROPERTY(QSize size READ size WRITE setSize)
    Q_PROPERTY(QSize size READ size WRITE setSize)

public:
    // 对应C++: enum TitleRole
    enum TitleRole {
        NameRole           = 0, // 会话名称
        DisplayedTitleRole = 1  // 显示的标题
    };

    // 对应C++: enum TabTitleContext
    enum TabTitleContext {
        LocalTabTitle  = 0, // 本地标签标题
        RemoteTabTitle = 1  // 远程标签标题
    };

    // 对应C++: Session::Session(QObject* parent)
    explicit Session(QObject *parent = nullptr);
    // 对应C++: ~Session()
    ~Session() override;

    // 对应C++: bool isRunning() const
    bool isRunning() const;

    // 对应C++: void setProfileKey(const QString &)
    void setProfileKey(const QString &profileKey);
    // 对应C++: QString profileKey() const
    QString profileKey() const;

    // 对应C++: void addView(TerminalDisplay *)
    void addView(TerminalDisplay *widget);
    // 对应C++: void removeView(TerminalDisplay *)
    void removeView(TerminalDisplay *widget);
    // 对应C++: QList<TerminalDisplay *> views() const
    QList<TerminalDisplay *> views() const;

    // 对应C++: Emulation * emulation() const  (cube-shell SSH bridge hook)
    Emulation *emulation() const;

    // 对应C++: QStringList environment() const
    QStringList environment() const;
    // 对应C++: void setEnvironment(const QStringList &)
    void setEnvironment(const QStringList &environment);

    // 对应C++: int sessionId() const
    int sessionId() const;

    // 对应C++: QString userTitle() const
    QString userTitle() const;

    // 对应C++: void setTabTitleFormat(TabTitleContext, const QString &)
    void setTabTitleFormat(TabTitleContext context, const QString &format);
    // 对应C++: QString tabTitleFormat(TabTitleContext) const
    QString tabTitleFormat(TabTitleContext context) const;

    // 对应C++: QStringList arguments() const
    QStringList arguments() const;
    // 对应C++: QString program() const
    QString program() const;

    // 对应C++: void setArguments(const QStringList &)
    void setArguments(const QStringList &arguments);
    // 对应C++: void setProgram(const QString &)
    void setProgram(const QString &program);

    // 对应C++: QString initialWorkingDirectory()
    QString initialWorkingDirectory() const;
    // 对应C++: void setInitialWorkingDirectory(const QString &)
    void setInitialWorkingDirectory(const QString &directory);

    // 对应C++: void setHistoryType(const HistoryType &)
    void setHistoryType(const HistoryType &historyType);
    // 对应C++: const HistoryType & historyType() const
    const HistoryType &historyType() const;
    // 对应C++: void clearHistory()
    void clearHistory();

    // 对应C++: void setMonitorActivity(bool)
    void setMonitorActivity(bool monitor);
    // 对应C++: bool isMonitorActivity() const
    bool isMonitorActivity() const;

    // 对应C++: void setMonitorSilence(bool)
    void setMonitorSilence(bool monitor);
    // 对应C++: bool isMonitorSilence() const
    bool isMonitorSilence() const;
    // 对应C++: void setMonitorSilenceSeconds(int)
    void setMonitorSilenceSeconds(int seconds);

    // 对应C++: void setKeyBindings(const QString &)
    void setKeyBindings(const QString &bindingsId);
    // 对应C++: QString keyBindings() const
    QString keyBindings() const;

    // 对应C++: void setTitle(TitleRole, const QString &)
    void setTitle(TitleRole role, const QString &newTitle);
    // 对应C++: QString title(TitleRole) const
    QString title(TitleRole role) const;
    // 对应C++: QString nameTitle() const
    QString nameTitle() const;

    // 对应C++: void setIconName(const QString &)
    void setIconName(const QString &iconName);
    // 对应C++: QString iconName() const
    QString iconName() const;

    // 对应C++: void setIconText(const QString &)
    void setIconText(const QString &iconText);
    // 对应C++: QString iconText() const
    QString iconText() const;

    // 对应C++: bool isTitleChanged() const
    bool isTitleChanged() const;

    // 对应C++: void setAddToUtmp(bool)
    void setAddToUtmp(bool add);

    // 对应C++: bool sendSignal(int)
    bool sendSignal(int signal);

    // 对应C++: void setAutoClose(bool)
    void setAutoClose(bool autoClose);

    // 对应C++: void setFlowControlEnabled(bool)
    void setFlowControlEnabled(bool enabled);
    // 对应C++: bool flowControlEnabled() const
    bool flowControlEnabled() const;

    // 对应C++: void sendText(const QString &) const
    void sendText(const QString &text) const;
    // 对应C++: void sendKeyEvent(QKeyEvent*) const
    void sendKeyEvent(QKeyEvent *event) const;

    // 对应C++: int processId() const
    int processId() const;
    // 对应C++: int foregroundProcessId() const
    int foregroundProcessId() const;

    // 对应C++: QSize size()
    QSize size() const;
    // 对应C++: void setSize(const QSize &)
    void setSize(const QSize &size);

    // 对应C++: void setDarkBackground(bool)
    void setDarkBackground(bool darkBackground);
    // 对应C++: bool hasDarkBackground() const
    bool hasDarkBackground() const;

    // 对应C++: void refresh()
    void refresh();

    // 对应C++: int getPtySlaveFd() const
    int getPtySlaveFd() const;

    // 对应C++: WId windowId() const
    int windowId() const;

Q_SIGNALS:
    // 对应C++: void started()
    void started();
    // 对应C++: void finished()
    void finished();
    // 对应C++: void receivedData(const QString &)  (Python: receivedData = Signal(str))
    void receivedData(const QString &text);
    // 对应C++: void titleChanged()
    void titleChanged();
    // 对应C++: void profileChanged(const QString &)
    void profileChanged(const QString &profileKey);
    // 对应C++: void stateChanged(int)
    void stateChanged(int state);
    // 对应C++: void bellRequest(const QString &)
    void bellRequest(const QString &message);
    // 对应C++: void changeTabTextColorRequest(int)
    void changeTabTextColorRequest(int color);
    // 对应C++: void changeBackgroundColorRequest(const QColor &)
    void changeBackgroundColorRequest(const QColor &color);
    // 对应C++: void openUrlRequest(const QString &)
    void openUrlRequest(const QString &url);
    // 对应C++: void resizeRequest(const QSize &)
    void resizeRequest(const QSize &size);
    // 对应C++: void profileChangeCommandReceived(const QString &)
    void profileChangeCommandReceived(const QString &text);
    // 对应C++: void flowControlEnabledChanged(bool)
    void flowControlEnabledChanged(bool enabled);
    // 对应C++: void cursorChanged(KeyboardCursorShape, bool)
    void cursorChanged(Konsole::KeyboardCursorShape cursorShape, bool blinkingCursorEnabled);
    // 对应C++: void silence()
    void silence();
    // 对应C++: void activity()
    void activity();
    // 终端尺寸（行列）已应用到 PTY / 外部代理（columns, rows）
    void terminalSizeApplied(int columns, int rows);

public Q_SLOTS:
    // 对应C++: void run()
    void run();
    // 对应C++: void runEmptyPTY()
    void runEmptyPTY();
    // 对应C++: void close()
    void close();
    // 对应C++: void setUserTitle(int, const QString &)
    void setUserTitle(int what, const QString &caption);

    // cube-shell SSH bridge hook: 由 paramiko_bridge 接收远端数据后调用。
    // 对应C++: void onReceiveBlock(const char *, int)
    void onReceiveBlock(const char *buffer, int length);

private Q_SLOTS:
    // 对应C++: void done(int, QProcess::ExitStatus)
    void done(int exitCode, QProcess::ExitStatus exitStatus);
    // 对应C++: void monitorTimerDone()
    void monitorTimerDone();
    // 对应C++: void onViewSizeChange(int, int)
    void onViewSizeChange(int height, int width);
    // 对应C++: void onEmulationSizeChange(QSize)
    void onEmulationSizeChange(const QSize &size);
    // 对应C++: void activityStateSet(int)
    void activityStateSet(int state);
    // 对应C++: void viewDestroyed(QObject *)
    void viewDestroyed(QObject *view);
    // 对应 Python _applyPendingTerminalSize()
    void applyPendingTerminalSize();

private:
    // 对应C++: void updateTerminalSize()
    void updateTerminalSize();
    // 对应 Python invalidateTerminalSize()
    void invalidateTerminalSize();

    // ---- 基本成员 ----
    // 对应C++: int _uniqueIdentifier
    int _uniqueIdentifier = 0;

    // 对应C++: Pty *_shellProcess
    Pty *_shellProcess;
    // 对应C++: Emulation *_emulation
    Emulation *_emulation;
    // 对应C++: QList<TerminalDisplay *> _views
    QList<TerminalDisplay *> _views;

    // 对应C++: bool _monitorActivity / _monitorSilence / _notifiedActivity
    bool _monitorActivity   = false;
    bool _monitorSilence    = false;
    bool _notifiedActivity  = false;
    bool _masterMode        = false;
    bool _autoClose         = true;
    bool _wantedClose       = false;
    // 对应C++: QTimer *_monitorTimer
    QTimer *_monitorTimer;

    // 对应C++: int _silenceSeconds
    int _silenceSeconds = 10;

    // 对应C++: QString _nameTitle / _displayTitle / _userTitle ...
    QString _nameTitle;
    QString _displayTitle;
    QString _userTitle;
    QString _localTabTitleFormat;
    QString _remoteTabTitleFormat;
    QString _iconName;
    QString _iconText;
    bool _isTitleChanged = false;

    // 对应C++: bool _addToUtmp / _flowControl / _fullScripting
    bool _addToUtmp    = false;
    bool _flowControl  = true;
    bool _fullScripting = false;

    // 对应C++: QString _program / QStringList _arguments / _environment / _initialWorkingDir
    QString _program;
    QStringList _arguments;
    QStringList _environment; // 修正: 是字符串列表,不是字典
    QString _initialWorkingDir;

    // 对应C++: int _sessionId
    int _sessionId;

    // 对应C++: QColor _modifiedBackground / QString _profileKey / bool _hasDarkBackground
    QColor _modifiedBackground;
    QString _profileKey;
    bool _hasDarkBackground = false;

    // 对应C++: int ptySlaveFd
    int _ptySlaveFd = -1;

    // Python 新增: 合并连续窗口变化的防抖定时器
    QTimer *_terminalSizeTimer;
    // 待应用的尺寸 (lines, columns); 无效表示没有挂起的尺寸
    QSize _pendingTerminalSize   = QSize(-1, -1);
    QSize _appliedTerminalSize   = QSize(-1, -1);
    bool _updatingTerminalImageSize = false;

    // Python 新增: 增量 UTF-8 解码器 (对应 codecs incremental decoder)
    QStringDecoder _receivedTextDecoder;

    // 对应C++: static int lastSessionId
    static int lastSessionId;
    // 对应 Python TERMINAL_RESIZE_DEBOUNCE_MS = 80
    static constexpr int TERMINAL_RESIZE_DEBOUNCE_MS = 80;
};

// 对应C++: class SessionGroup : public QObject
class SessionGroup : public QObject {
    Q_OBJECT
public:
    // 对应C++: enum MasterMode
    enum MasterMode {
        CopyInputToAll = 1 // 主会话中的任何输入按键都发送到组中的所有会话
    };

    // 对应C++: SessionGroup::SessionGroup()
    explicit SessionGroup(QObject *parent = nullptr);
    // 对应C++: ~SessionGroup()
    ~SessionGroup() override;

    // 对应C++: void addSession(Session *)
    void addSession(Session *session);
    // 对应C++: void removeSession(Session *)
    void removeSession(Session *session);
    // 对应C++: QList<Session *> sessions() const
    QList<Session *> sessions() const;

    // 对应C++: void setMasterStatus(Session *, bool)
    void setMasterStatus(Session *session, bool master);
    // 对应C++: bool masterStatus(Session *) const
    bool masterStatus(Session *session) const;

    // 对应C++: void setMasterMode(int)
    void setMasterMode(int mode);
    // 对应C++: int masterMode() const
    int masterMode() const;

private:
    // 对应C++: QList<Session*> masters() const
    QList<Session *> masters() const;
    // 对应C++: void connectAll(bool)
    void connectAll(bool connect);
    // 对应C++: void connectPair(Session *, Session *) const
    void connectPair(Session *master, Session *other);
    // 对应C++: void disconnectPair(Session *, Session *) const
    void disconnectPair(Session *master, Session *other);

    // 对应C++: QHash<Session *,bool> _sessions
    QHash<Session *, bool> _sessions;
    // 对应C++: int _masterMode
    int _masterMode = 0;
};

} // namespace Konsole
