// Session.cpp — C++ port of qtermwidget/session.py
//
// A terminal session: owns a Pty shell process + an Emulation + zero or more
// views (TerminalDisplay). See Session.h for the class documentation.
//
// Original copyright:
//   Copyright (C) 2006-2007 by Robert Knight <robertknight@gmail.com>
//   Copyright (C) 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

#include "Session.h"

#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QThread>

#include "Emulation.h"
#include "Pty.h"
#include "ShellCommand.h"
#include "TerminalDisplay.h" // 并行移植模块,契约名 (见 assumptions)
#include "Vt102Emulation.h"

#if defined(Q_OS_UNIX)
#  include <csignal>
#  include <unistd.h>
#endif

namespace Konsole {

// 对应C++: int Session::lastSessionId = 0
int Session::lastSessionId = 0;

// 对应C++: Session::Session(QObject* parent)
Session::Session(QObject *parent)
    : QObject(parent)
    , _receivedTextDecoder("UTF-8") // 对应 codecs.getincrementaldecoder('utf-8')(errors='replace')
{
    // ---- 核心组件 ----
    // 对应C++: _shellProcess = new Pty();
    _shellProcess = new Pty(this);
    // 对应C++: ptySlaveFd = _shellProcess->pty()->slaveFd();
    if (_shellProcess->pty())
        _ptySlaveFd = _shellProcess->pty()->slaveFd();

    // 对应C++: _emulation = new Vt102Emulation();
    _emulation = new Vt102Emulation();
    _emulation->setKeyBindings(QStringLiteral("default"));

    // 对应C++: _sessionId = ++lastSessionId;
    _sessionId = ++lastSessionId;

    // ---- 连接信号 (对应 _connectSignals) ----
    // connect(_emulation, SIGNAL(titleChanged(int, const QString &)), this, SLOT(setUserTitle(int, const QString &)));
    connect(_emulation, &Emulation::titleChanged, this, &Session::setUserTitle);
    // connect(_emulation, SIGNAL(stateSet(int)), this, SLOT(activityStateSet(int)));
    connect(_emulation, &Emulation::stateSet, this, &Session::activityStateSet);
    // connect(_emulation, SIGNAL(changeTabTextColorRequest(int)), this, SIGNAL(changeTabTextColorRequest(int)));
    connect(_emulation, &Emulation::changeTabTextColorRequest, this, &Session::changeTabTextColorRequest);
    // connect(_emulation, SIGNAL(profileChangeCommandReceived(const QString &)), this, SIGNAL(...));
    connect(_emulation, &Emulation::profileChangeCommandReceived, this, &Session::profileChangeCommandReceived);
    // connect(_emulation, SIGNAL(imageResizeRequest(QSize)), this, SLOT(onEmulationSizeChange(QSize)));
    connect(_emulation, &Emulation::imageResizeRequest, this, &Session::onEmulationSizeChange);
    // connect(_emulation, SIGNAL(imageSizeChanged(int, int)), this, SLOT(onViewSizeChange(int, int)));
    connect(_emulation, &Emulation::imageSizeChanged, this, &Session::onViewSizeChange);
    // connect(_emulation, &Vt102Emulation::cursorChanged, this, &Session::cursorChanged);
    connect(_emulation, &Emulation::cursorChanged, this, &Session::cursorChanged);

    // _shellProcess->setUtf8Mode(true);
    _shellProcess->setUtf8Mode(true);

    // connect(_shellProcess, SIGNAL(receivedData(const char *,int)), this, SLOT(onReceiveBlock(const char *,int)));
    connect(_shellProcess, &Pty::receivedData, this, &Session::onReceiveBlock);
    // connect(_emulation, SIGNAL(sendData(const char *,int)), _shellProcess, SLOT(sendData(const char *,int)));
    connect(_emulation, &Emulation::sendData, _shellProcess, &Pty::sendData);
    // connect(_emulation, SIGNAL(lockPtyRequest(bool)), _shellProcess, SLOT(lockPty(bool)));
    connect(_emulation, &Emulation::lockPtyRequest, _shellProcess, &Pty::lockPty);
    // connect(_emulation, SIGNAL(useUtf8Request(bool)), _shellProcess, SLOT(setUtf8Mode(bool)));
    connect(_emulation, &Emulation::useUtf8Request, _shellProcess, &Pty::setUtf8Mode);
    // connect(_shellProcess, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(done(int,QProcess::ExitStatus)));
#ifdef Q_OS_WIN
    connect(_shellProcess, &Pty::processExited, this, &Session::done);
#else
    connect(_shellProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &Session::done);
#endif

    // ---- 监控定时器 (对应 _setupMonitorTimer) ----
    // 对应C++: _monitorTimer = new QTimer(this);
    _monitorTimer = new QTimer(this);
    _monitorTimer->setSingleShot(true);
    connect(_monitorTimer, &QTimer::timeout, this, &Session::monitorTimerDone);

    // ---- 终端尺寸防抖定时器 (对应 _setupTerminalSizeTimer,Python 新增) ----
    _terminalSizeTimer = new QTimer(this);
    _terminalSizeTimer->setSingleShot(true);
    connect(_terminalSizeTimer, &QTimer::timeout, this, &Session::applyPendingTerminalSize);
}

// 对应C++: Session::~Session()
Session::~Session()
{
    close();
}

// 对应C++: bool Session::isRunning() const
bool Session::isRunning() const
{
    return _shellProcess && _shellProcess->state() == QProcess::Running;
}

// 对应C++: void Session::setProfileKey(const QString &)
void Session::setProfileKey(const QString &profileKey)
{
    _profileKey = profileKey;
    Q_EMIT profileChanged(profileKey);
}

// 对应C++: QString Session::profileKey() const
QString Session::profileKey() const
{
    return _profileKey;
}

// 对应C++: void Session::addView(TerminalDisplay *)
void Session::addView(TerminalDisplay *widget)
{
    // 对应C++: Q_ASSERT(!_views.contains(widget));
    if (_views.contains(widget))
        return;

    // 对应C++: _views.append(widget);
    _views.append(widget);

    if (_emulation) {
        // connect(widget, &TerminalDisplay::keyPressedSignal, _emulation, &Emulation::sendKeyEvent);
        // keyPressedSignal(QKeyEvent*,bool) -> sendKeyEvent(QKeyEvent*,bool)
        connect(widget, &TerminalDisplay::keyPressedSignal, _emulation, &Emulation::sendKeyEvent);
        // connect(widget, SIGNAL(mouseSignal(int,int,int,int)), _emulation, SLOT(sendMouseEvent(int,int,int,int)));
        connect(widget, &TerminalDisplay::mouseSignal, _emulation, &Emulation::sendMouseEvent);
        // connect(widget, SIGNAL(sendStringToEmu(const char *)), _emulation, SLOT(sendString(const char *)));
        // TerminalDisplay::sendStringToEmu(const QByteArray&) -> Emulation::sendString(const char*, int)
        connect(widget, &TerminalDisplay::sendStringToEmu, _emulation,
                [emulation = _emulation](const QByteArray &data) {
                    emulation->sendString(data.constData(), data.size());
                });

        // connect(_emulation, SIGNAL(programUsesMouseChanged(bool)), widget, SLOT(setUsesMouse(bool)));
        connect(_emulation, &Emulation::programUsesMouseChanged, widget, &TerminalDisplay::setUsesMouse);
        // widget->setUsesMouse(_emulation->programUsesMouse());
        widget->setUsesMouse(_emulation->programUsesMouse());

        // 主屏/备用屏切换通知视图 (cube-shell 扩展)
        connect(_emulation, &Emulation::primaryScreenInUse, widget, &TerminalDisplay::setPrimaryScreenInUse);
        // 焦点上报(?1004)通知视图 (cube-shell 扩展)
        connect(_emulation, &Emulation::programReportFocusChanged, widget, &TerminalDisplay::setReportFocusMode);

        // connect(_emulation, SIGNAL(programBracketedPasteModeChanged(bool)), widget, SLOT(setBracketedPasteMode(bool)));
        connect(_emulation, &Emulation::programBracketedPasteModeChanged, widget, &TerminalDisplay::setBracketedPasteMode);
        // widget->setBracketedPasteMode(_emulation->programBracketedPasteMode());
        widget->setBracketedPasteMode(_emulation->programBracketedPasteMode());

        // widget->setScreenWindow(_emulation->createWindow());
        widget->setScreenWindow(_emulation->createWindow());
    }

    // QObject::connect(widget, SIGNAL(changedContentSizeSignal(int,int)), this, SLOT(onViewSizeChange(int,int)));
    connect(widget, &TerminalDisplay::changedContentSizeSignal, this, &Session::onViewSizeChange);
    // QObject::connect(widget, SIGNAL(destroyed(QObject *)), this, SLOT(viewDestroyed(QObject *)));
    connect(widget, &QObject::destroyed, this, &Session::viewDestroyed);
    // QObject::connect(this, SIGNAL(finished()), widget, SLOT(close()));
    connect(this, &Session::finished, widget, &QWidget::close);
}

// 对应C++: void Session::removeView(TerminalDisplay *)
void Session::removeView(TerminalDisplay *widget)
{
    // 对应C++: _views.removeAll(widget);
    _views.removeAll(widget);

    // 对应C++: disconnect(widget, nullptr, this, nullptr);
    disconnect(widget, nullptr, this, nullptr);

    if (_emulation) {
        // 对应C++: disconnect(widget, nullptr, _emulation, nullptr);
        disconnect(widget, nullptr, _emulation, nullptr);
        // 对应C++: disconnect(_emulation, nullptr, widget, nullptr);
        disconnect(_emulation, nullptr, widget, nullptr);
    }

    // 当移除最后一个视图时自动关闭会话
    // 对应C++: if (_views.count() == 0) { close(); }
    if (_views.count() == 0)
        close();
}

// 对应C++: QList<TerminalDisplay *> Session::views() const
QList<TerminalDisplay *> Session::views() const
{
    return _views;
}

// 对应C++: Emulation * Session::emulation() const
Emulation *Session::emulation() const
{
    return _emulation;
}

// 对应C++: QStringList Session::environment() const
QStringList Session::environment() const
{
    return _environment;
}

// 对应C++: void Session::setEnvironment(const QStringList &)
void Session::setEnvironment(const QStringList &environment)
{
    _environment = environment;
}

// 对应C++: int Session::sessionId() const
int Session::sessionId() const
{
    return _sessionId;
}

// 对应C++: QString Session::userTitle() const
QString Session::userTitle() const
{
    return _userTitle;
}

// 对应C++: void Session::setTabTitleFormat(TabTitleContext, const QString &)
void Session::setTabTitleFormat(TabTitleContext context, const QString &format)
{
    if (context == LocalTabTitle)
        _localTabTitleFormat = format;
    else if (context == RemoteTabTitle)
        _remoteTabTitleFormat = format;
}

// 对应C++: QString Session::tabTitleFormat(TabTitleContext) const
QString Session::tabTitleFormat(TabTitleContext context) const
{
    if (context == LocalTabTitle)
        return _localTabTitleFormat;
    else if (context == RemoteTabTitle)
        return _remoteTabTitleFormat;
    return QString();
}

// 对应C++: QStringList Session::arguments() const
QStringList Session::arguments() const
{
    return _arguments;
}

// 对应C++: QString Session::program() const
QString Session::program() const
{
    return _program;
}

// 对应C++: void Session::setArguments(const QStringList &)
void Session::setArguments(const QStringList &arguments)
{
    _arguments = ShellCommand::expand(arguments);
}

// 对应C++: void Session::setProgram(const QString &)
void Session::setProgram(const QString &program)
{
    _program = ShellCommand::expand(program);
}

// 对应C++: QString Session::initialWorkingDirectory()
QString Session::initialWorkingDirectory() const
{
    return _initialWorkingDir;
}

// 对应C++: void Session::setInitialWorkingDirectory(const QString &)
void Session::setInitialWorkingDirectory(const QString &directory)
{
    _initialWorkingDir = ShellCommand::expand(directory);
}

// 对应C++: void Session::setHistoryType(const HistoryType &)
void Session::setHistoryType(const HistoryType &historyType)
{
    if (_emulation)
        _emulation->setHistory(historyType);
}

// 对应C++: const HistoryType & Session::historyType() const
const HistoryType &Session::historyType() const
{
    // Emulation::history() 在无模拟器时无法返回引用;保证 _emulation 恒非空。
    return _emulation->history();
}

// 对应C++: void Session::clearHistory()
void Session::clearHistory()
{
    if (_emulation)
        _emulation->clearHistory();
}

// 对应C++: void Session::setMonitorActivity(bool)
void Session::setMonitorActivity(bool monitor)
{
    _monitorActivity = monitor;
    _notifiedActivity = false;
    activityStateSet(NOTIFYNORMAL);
}

// 对应C++: bool Session::isMonitorActivity() const
bool Session::isMonitorActivity() const
{
    return _monitorActivity;
}

// 对应C++: void Session::setMonitorSilence(bool)
void Session::setMonitorSilence(bool monitor)
{
    if (_monitorSilence == monitor)
        return;

    _monitorSilence = monitor;
    if (_monitorSilence)
        _monitorTimer->start(_silenceSeconds * 1000);
    else
        _monitorTimer->stop();

    activityStateSet(NOTIFYNORMAL);
}

// 对应C++: bool Session::isMonitorSilence() const
bool Session::isMonitorSilence() const
{
    return _monitorSilence;
}

// 对应C++: void Session::setMonitorSilenceSeconds(int)
void Session::setMonitorSilenceSeconds(int seconds)
{
    _silenceSeconds = seconds;
    if (_monitorSilence)
        _monitorTimer->start(_silenceSeconds * 1000);
}

// 对应C++: void Session::setKeyBindings(const QString &)
void Session::setKeyBindings(const QString &bindingsId)
{
    if (_emulation)
        _emulation->setKeyBindings(bindingsId);
}

// 对应C++: QString Session::keyBindings() const
QString Session::keyBindings() const
{
    if (_emulation)
        return _emulation->keyBindings();
    return QString();
}

// 对应C++: void Session::setTitle(TitleRole, const QString &)
void Session::setTitle(TitleRole role, const QString &newTitle)
{
    if (title(role) != newTitle) {
        if (role == NameRole)
            _nameTitle = newTitle;
        else if (role == DisplayedTitleRole)
            _displayTitle = newTitle;

        Q_EMIT titleChanged();
    }
}

// 对应C++: QString Session::title(TitleRole) const
QString Session::title(TitleRole role) const
{
    if (role == NameRole)
        return _nameTitle;
    else if (role == DisplayedTitleRole)
        return _displayTitle;
    return QString();
}

// 对应C++: QString Session::nameTitle() const
QString Session::nameTitle() const
{
    return title(NameRole);
}

// 对应C++: void Session::setIconName(const QString &)
void Session::setIconName(const QString &iconName)
{
    if (iconName != _iconName) {
        _iconName = iconName;
        Q_EMIT titleChanged();
    }
}

// 对应C++: QString Session::iconName() const
QString Session::iconName() const
{
    return _iconName;
}

// 对应C++: void Session::setIconText(const QString &)
void Session::setIconText(const QString &iconText)
{
    _iconText = iconText;
}

// 对应C++: QString Session::iconText() const
QString Session::iconText() const
{
    return _iconText;
}

// 对应C++: bool Session::isTitleChanged() const
bool Session::isTitleChanged() const
{
    return _isTitleChanged;
}

// 对应C++: void Session::setAddToUtmp(bool)
void Session::setAddToUtmp(bool add)
{
    _addToUtmp = add;
}

// 对应C++: bool Session::sendSignal(int)
bool Session::sendSignal(int sig)
{
    if (processId() <= 0)
        return false;

#if defined(Q_OS_UNIX)
    // 对应C++: int result = ::kill(static_cast<pid_t>(_shellProcess->processId()), signal);
    const int result = ::kill(static_cast<pid_t>(_shellProcess->processId()), sig);
    // 对应C++: if (result == 0) { return _shellProcess->waitForFinished(1000); }
    if (result == 0)
        return _shellProcess->waitForFinished(1000);
    return false;
#else
    // TODO(win32): ConPTY 进程信号
    Q_UNUSED(sig);
    return false;
#endif
}

// 对应C++: void Session::setAutoClose(bool)
void Session::setAutoClose(bool autoClose)
{
    _autoClose = autoClose;
}

// 对应C++: void Session::setFlowControlEnabled(bool)
void Session::setFlowControlEnabled(bool enabled)
{
    if (_flowControl == enabled)
        return;

    _flowControl = enabled;

    if (_shellProcess)
        _shellProcess->setFlowControlEnabled(_flowControl);

    Q_EMIT flowControlEnabledChanged(enabled);
}

// 对应C++: bool Session::flowControlEnabled() const
bool Session::flowControlEnabled() const
{
    return _flowControl;
}

// 对应C++: void Session::sendText(const QString &) const
void Session::sendText(const QString &text) const
{
    if (_emulation)
        _emulation->sendText(text);
}

// 对应C++: void Session::sendKeyEvent(QKeyEvent*) const
void Session::sendKeyEvent(QKeyEvent *event) const
{
    if (_emulation)
        _emulation->sendKeyEvent(event, false);
}

// 对应C++: int Session::processId() const
int Session::processId() const
{
    if (_shellProcess)
        return static_cast<int>(_shellProcess->processId());
    return -1;
}

// 对应C++: int Session::foregroundProcessId() const
int Session::foregroundProcessId() const
{
    if (_shellProcess)
        return _shellProcess->foregroundProcessGroup();
    return -1;
}

// 对应C++: QSize Session::size()
QSize Session::size() const
{
    if (_emulation)
        return _emulation->imageSize();
    return QSize();
}

// 对应C++: void Session::setSize(const QSize &)
void Session::setSize(const QSize &size)
{
    if (size.width() <= 1 || size.height() <= 1)
        return;
    Q_EMIT resizeRequest(size);
}

// 对应C++: void Session::setDarkBackground(bool)
void Session::setDarkBackground(bool darkBackground)
{
    _hasDarkBackground = darkBackground;
}

// 对应C++: bool Session::hasDarkBackground() const
bool Session::hasDarkBackground() const
{
    return _hasDarkBackground;
}

// 对应C++: void Session::refresh()
void Session::refresh()
{
    if (!_shellProcess)
        return;

    // 先稍微增大窗口,然后恢复原大小以触发变化
    const QSize existingSize = _shellProcess->windowSize();
    _shellProcess->setWindowSize(existingSize.height(), existingSize.width() + 1);
    // 延迟1ms恢复,给Shell一点反应时间
    QTimer::singleShot(1, this, [this, existingSize]() {
        if (_shellProcess)
            _shellProcess->setWindowSize(existingSize.height(), existingSize.width());
    });
}

// 对应C++: int Session::getPtySlaveFd() const
int Session::getPtySlaveFd() const
{
    return _ptySlaveFd;
}

// 对应C++: WId Session::windowId() const
int Session::windowId() const
{
    // 在Qt5+中返回0以避免问题
    return 0;
}

// 对应C++: void Session::run()
void Session::run()
{
    if (!_shellProcess || !_emulation)
        return;

    // 对应C++注释中的shell检查逻辑
    QString exec = _program;

    // 只有当exec是绝对路径或为空时才检查文件存在性;
    // 对于非绝对路径(如ssh),直接使用,让系统在PATH中查找。
    if (exec.startsWith(QLatin1Char('/')) || exec.isEmpty()) {
        const QString defaultShell = QStringLiteral("/bin/sh");

        if (exec.isEmpty() || (exec.startsWith(QLatin1Char('/')) && !QFileInfo::exists(exec)))
            exec = QString::fromLocal8Bit(qgetenv("SHELL"));

        if (exec.isEmpty() || (exec.startsWith(QLatin1Char('/')) && !QFileInfo::exists(exec)))
            exec = defaultShell;
    }

    // arguments[0]是程序名,arguments[1:]是实际参数
    const QString argsTmp = _arguments.join(QLatin1Char(' ')).trimmed();
    // argv[0]应该是程序的基本名称,不是完整路径
    const QString programName = exec.startsWith(QLatin1Char('/')) ? QFileInfo(exec).fileName() : exec;
    QStringList arguments{programName};
    if (!argsTmp.isEmpty())
        arguments.append(_arguments);

    // 设置工作目录
    const QString cwd = QDir::currentPath();
    if (!_initialWorkingDir.isEmpty())
        _shellProcess->setWorkingDirectory(_initialWorkingDir);
    else
        _shellProcess->setWorkingDirectory(cwd);

    // 设置流控制和其他属性
    _shellProcess->setFlowControlEnabled(_flowControl);
    _shellProcess->setErase(_emulation->eraseChar());

    // 设置颜色背景提示
    const QString backgroundColorHint = _hasDarkBackground
        ? QStringLiteral("COLORFGBG=15;0")
        : QStringLiteral("COLORFGBG=0;15");

    // int result = _shellProcess->start(exec, arguments, _environment << backgroundColorHint, windowId(), _addToUtmp);
    // Pty::start 期望 programArguments[0] 是程序名、[1:] 是实际参数(它内部做 .mid(1))。
    // 因此这里要传完整的 arguments(含程序名),不能再 .mid(1) 剥掉程序名,否则参数会被剥离两次。
    QStringList environment = _environment;
    environment << backgroundColorHint;

    const int result = _shellProcess->start(exec, arguments, environment, windowId(), _addToUtmp);
    if (result < 0)
        return;

    // 对应C++: _shellProcess->setWriteable(false);
    _shellProcess->setWriteable(false);

    // 新进程拥有新的 PTY,强制把当前尺寸同步给它。
    invalidateTerminalSize();
    // 延迟确保尺寸正确同步到PTY
    QTimer::singleShot(100, this, [this]() { updateTerminalSize(); });

    Q_EMIT started();
}

// 对应C++: void Session::runEmptyPTY()
void Session::runEmptyPTY()
{
    if (!_shellProcess || !_emulation)
        return;

    _shellProcess->setFlowControlEnabled(_flowControl);
    _shellProcess->setErase(_emulation->eraseChar());
    _shellProcess->setWriteable(false);

    // 断开从模拟器到内部终端进程的数据发送
    disconnect(_emulation, &Emulation::sendData, _shellProcess, &Pty::sendData);

    _shellProcess->setEmptyPTYProperties();

    Q_EMIT started();
}

// 对应C++: void Session::close()
void Session::close()
{
    // 线程安全: 非对象所属线程调用则排队执行
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, &Session::close, Qt::QueuedConnection);
        return;
    }

    _autoClose = true;
    _wantedClose = true;

    if (_shellProcess)
        _shellProcess->blockSignals(true);

    if (isRunning()) {
        // 直接 kill() 杀死进程,避免复杂信号逻辑导致的竞态条件
        _shellProcess->kill();
        // 等待时间不能太长,否则阻塞UI线程
        _shellProcess->waitForFinished(100);
    }

    // 强制发出 finished 信号
    QTimer::singleShot(1, this, [this]() { Q_EMIT finished(); });
}

// 对应C++: void Session::setUserTitle(int, const QString &)
void Session::setUserTitle(int what, const QString &caption)
{
    // (btw: what=0 changes _userTitle and icon, what=1 only icon, what=2 only _nameTitle
    bool modified = false;

    if (what == 0 || what == 2) {
        _isTitleChanged = true;
        if (_userTitle != caption) {
            _userTitle = caption;
            modified = true;
        }
    }

    if (what == 0 || what == 1) {
        _isTitleChanged = true;
        if (_iconText != caption) {
            _iconText = caption;
            modified = true;
        }
    }

    if (what == 11) {
        const QString colorString = caption.section(QLatin1Char(';'), 0, 0);
        const QColor backColor(colorString);
        if (backColor.isValid() && backColor != _modifiedBackground) {
            _modifiedBackground = backColor;
            Q_EMIT changeBackgroundColorRequest(backColor);
        }
    }

    if (what == 30) {
        _isTitleChanged = true;
        if (_nameTitle != caption) {
            setTitle(NameRole, caption);
            return;
        }
    }

    if (what == 31) {
        // 处理当前工作目录
        QString cwd = caption;
        cwd.replace(QRegularExpression(QStringLiteral("^~")), QDir::homePath());
        Q_EMIT openUrlRequest(cwd);
    }

    if (what == 32) {
        // 通过 \033]32;Icon\007 更改图标
        _isTitleChanged = true;
        if (_iconName != caption) {
            _iconName = caption;
            modified = true;
        }
    }

    if (what == 50) {
        Q_EMIT profileChangeCommandReceived(caption);
        return;
    }

    if (modified)
        Q_EMIT titleChanged();
}

// 对应C++: void Session::done(int, QProcess::ExitStatus)
void Session::done(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!_autoClose) {
        _userTitle = QStringLiteral("This session is done. Finished");
        Q_EMIT titleChanged();
        return;
    }

    // message 构造仅用于上游调试输出,此处省略;保留 finished 的发射逻辑。
    QString message;
    if (!_wantedClose || exitCode != 0) {
        if (_shellProcess && _shellProcess->exitStatus() == QProcess::NormalExit)
            message = QStringLiteral("Session '%1' exited with code %2.").arg(_nameTitle).arg(exitCode);
        else
            message = QStringLiteral("Session '%1' crashed.").arg(_nameTitle);
    }

    if (!_wantedClose && exitStatus != QProcess::NormalExit)
        message = QStringLiteral("Session '%1' exited unexpectedly.").arg(_nameTitle);
    else
        Q_EMIT finished();

    Q_UNUSED(message);
}

// 对应C++: void Session::onReceiveBlock(const char *, int)
void Session::onReceiveBlock(const char *buffer, int length)
{
    if (_emulation)
        _emulation->receiveData(buffer, length);

    // 使用 UTF-8 增量解码,支持 SSH 远端输出
    // 对应 codecs incremental decoder(errors='replace')
    const QByteArrayView view(buffer, length);
    const QString text = _receivedTextDecoder.decode(view);
    Q_EMIT receivedData(text);
}

// 对应C++: void Session::monitorTimerDone()
void Session::monitorTimerDone()
{
    if (_monitorSilence) {
        Q_EMIT silence();
        Q_EMIT stateChanged(NOTIFYSILENCE);
    } else {
        Q_EMIT stateChanged(NOTIFYNORMAL);
    }

    _notifiedActivity = false;
}

// 对应C++: void Session::onViewSizeChange(int, int)
void Session::onViewSizeChange(int height, int width)
{
    Q_UNUSED(height);
    Q_UNUSED(width);
    if (_updatingTerminalImageSize)
        return;
    updateTerminalSize();
}

// 对应C++: void Session::onEmulationSizeChange(QSize)
void Session::onEmulationSizeChange(const QSize &size)
{
    setSize(size);
}

// 对应C++: void Session::activityStateSet(int)
void Session::activityStateSet(int state)
{
    if (state == NOTIFYBELL) {
        Q_EMIT bellRequest(QStringLiteral("Bell in session '%1'").arg(_nameTitle));
    } else if (state == NOTIFYACTIVITY) {
        if (_monitorSilence)
            _monitorTimer->start(_silenceSeconds * 1000);

        if (_monitorActivity && !_notifiedActivity) {
            _notifiedActivity = true;
            Q_EMIT activity();
        }
    }

    if (state == NOTIFYACTIVITY && !_monitorActivity)
        state = NOTIFYNORMAL;
    if (state == NOTIFYSILENCE && !_monitorSilence)
        state = NOTIFYNORMAL;

    Q_EMIT stateChanged(state);
}

// 对应C++: void Session::viewDestroyed(QObject *)
void Session::viewDestroyed(QObject *view)
{
    auto *display = qobject_cast<TerminalDisplay *>(view);
    if (display && _views.contains(display))
        removeView(display);
}

// 对应C++: void Session::updateTerminalSize()
void Session::updateTerminalSize()
{
    if (!_emulation)
        return;

    int minLines = -1;
    int minColumns = -1;

    // 选择适合所有可见视图大小的最大行列数
    for (TerminalDisplay *view : std::as_const(_views)) {
        if (!view->isHidden()
            && view->lines() >= SESSION_VIEW_LINES_THRESHOLD
            && view->columns() >= SESSION_VIEW_COLUMNS_THRESHOLD) {
            minLines = (minLines == -1) ? view->lines() : qMin(minLines, view->lines());
            minColumns = (minColumns == -1) ? view->columns() : qMin(minColumns, view->columns());
        }
    }

    // 后端模拟器必须至少有1列x1行的终端大小
    if (minLines > 0 && minColumns > 0) {
        const QSize size(minColumns, minLines);

        if (_emulation->imageSize() != size) {
            _updatingTerminalImageSize = true;
            _emulation->setImageSize(minLines, minColumns);
            _updatingTerminalImageSize = false;
        }

        _pendingTerminalSize = size;
        if (size == _appliedTerminalSize) {
            _pendingTerminalSize = QSize(-1, -1);
            _terminalSizeTimer->stop();
            return;
        }

        _terminalSizeTimer->start(TERMINAL_RESIZE_DEBOUNCE_MS);
    }
}

// 对应 Python _applyPendingTerminalSize()
void Session::applyPendingTerminalSize()
{
    const QSize size = _pendingTerminalSize;
    _pendingTerminalSize = QSize(-1, -1);
    if (!size.isValid() || size == _appliedTerminalSize)
        return;

    if (_shellProcess) {
        // setWindowSize(lines, cols)
        _shellProcess->setWindowSize(size.height(), size.width());
        _appliedTerminalSize = size;
    }

    // 通知外部代理（如 SshBridge）同步新尺寸到远程 PTY
    // size = (columns, lines)  —  emit (columns, rows)
    emit terminalSizeApplied(size.width(), size.height());
}

// 对应 Python invalidateTerminalSize()
void Session::invalidateTerminalSize()
{
    _appliedTerminalSize = QSize(-1, -1);
}

// ============================================================================
// SessionGroup
// ============================================================================

// 对应C++: SessionGroup::SessionGroup()
SessionGroup::SessionGroup(QObject *parent)
    : QObject(parent)
{
}

// 对应C++: SessionGroup::~SessionGroup()
SessionGroup::~SessionGroup()
{
    // 对应C++: connectAll(false);
    connectAll(false);
}

// 对应C++: void SessionGroup::addSession(Session *)
void SessionGroup::addSession(Session *session)
{
    // 对应C++: _sessions.insert(session, false);
    _sessions.insert(session, false);

    const QList<Session *> masterList = masters();
    for (Session *master : masterList)
        connectPair(master, session);
}

// 对应C++: void SessionGroup::removeSession(Session *)
void SessionGroup::removeSession(Session *session)
{
    setMasterStatus(session, false);

    const QList<Session *> masterList = masters();
    for (Session *master : masterList)
        disconnectPair(master, session);

    _sessions.remove(session);
}

// 对应C++: QList<Session *> SessionGroup::sessions() const
QList<Session *> SessionGroup::sessions() const
{
    return _sessions.keys();
}

// 对应C++: void SessionGroup::setMasterStatus(Session *, bool)
void SessionGroup::setMasterStatus(Session *session, bool master)
{
    if (!_sessions.contains(session))
        return;

    const bool wasMaster = _sessions[session];
    _sessions[session] = master;

    if (wasMaster == master)
        return;

    const QList<Session *> keys = _sessions.keys();
    for (Session *other : keys) {
        if (other != session) {
            if (master)
                connectPair(session, other);
            else
                disconnectPair(session, other);
        }
    }
}

// 对应C++: bool SessionGroup::masterStatus(Session *) const
bool SessionGroup::masterStatus(Session *session) const
{
    return _sessions.value(session, false);
}

// 对应C++: void SessionGroup::setMasterMode(int)
void SessionGroup::setMasterMode(int mode)
{
    _masterMode = mode;
    connectAll(false);
    connectAll(true);
}

// 对应C++: int SessionGroup::masterMode() const
int SessionGroup::masterMode() const
{
    return _masterMode;
}

// 对应C++: QList<Session*> SessionGroup::masters() const
QList<Session *> SessionGroup::masters() const
{
    QList<Session *> result;
    for (auto it = _sessions.constBegin(); it != _sessions.constEnd(); ++it) {
        if (it.value())
            result.append(it.key());
    }
    return result;
}

// 对应C++: void SessionGroup::connectAll(bool)
void SessionGroup::connectAll(bool connect)
{
    const QList<Session *> masterList = masters();
    const QList<Session *> keys = _sessions.keys();
    for (Session *master : masterList) {
        for (Session *other : keys) {
            if (other != master) {
                if (connect)
                    connectPair(master, other);
                else
                    disconnectPair(master, other);
            }
        }
    }
}

// 对应C++: void SessionGroup::connectPair(Session *, Session *) const
void SessionGroup::connectPair(Session *master, Session *other)
{
    if (_masterMode & CopyInputToAll) {
        Emulation *masterEmulation = master->emulation();
        Emulation *otherEmulation = other->emulation();

        if (masterEmulation && otherEmulation) {
            // connect(master->emulation(), SIGNAL(sendData(const char *,int)),
            //         other->emulation(), SLOT(sendString(const char *,int)));
            QObject::connect(masterEmulation, &Emulation::sendData,
                             otherEmulation, &Emulation::sendString);
        }
    }
}

// 对应C++: void SessionGroup::disconnectPair(Session *, Session *) const
void SessionGroup::disconnectPair(Session *master, Session *other)
{
    if (_masterMode & CopyInputToAll) {
        Emulation *masterEmulation = master->emulation();
        Emulation *otherEmulation = other->emulation();

        if (masterEmulation && otherEmulation) {
            QObject::disconnect(masterEmulation, &Emulation::sendData,
                                otherEmulation, &Emulation::sendString);
        }
    }
}

} // namespace Konsole
