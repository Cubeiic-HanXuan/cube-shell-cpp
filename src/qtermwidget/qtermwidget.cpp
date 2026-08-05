/*
    Copyright (C) 2008 e_k (e_k@users.sourceforge.net)

    This file is part of Konsole / QTermWidget.

    Ported from the Python PySide6 version (qtermwidget/qtermwidget.py), which
    was itself converted from upstream QTermWidget 2.2.0 (qtermwidget.cpp/h).
*/

// qtermwidget.cpp — C++ port of qtermwidget/qtermwidget.py

#include "qtermwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTextStream>
#include <QTranslator>
#include <QVBoxLayout>

#include "ColorScheme.h"               // ColorSchemeManager, ColorEntry, TABLE_COLORS
#include "Emulation.h"                 // Konsole::Emulation, KeyboardCursorShape
#include "Filter.h"                    // Konsole::UrlFilter, FilterChain, Filter::HotSpot
#include "History.h"                   // HistoryTypeFile / HistoryTypeNone / HistoryTypeBuffer
#include "HistorySearch.h"             // Konsole::HistorySearch (并行移植)
#include "KeyboardTranslator.h"        // KeyboardTranslatorManager
#include "Screen.h"                    // Konsole::Screen (经 ScreenWindow 访问)
#include "ScreenWindow.h"              // Konsole::ScreenWindow
#include "SearchBar.h"                 // Konsole::SearchBar
#include "Session.h"                   // Konsole::Session (并行移植)
#include "TerminalCharacterDecoder.h"  // Konsole::PlainTextDecoder
#include "TerminalDisplay.h"           // Konsole::TerminalDisplay (并行移植)
#include "Vt102Emulation.h"            // Konsole::Vt102Emulation (isAppScreenMode)

using namespace Konsole;

// ---------------------------------------------------------------------------
// 内部实现类
// ---------------------------------------------------------------------------

/**
 * 终端部件内部实现类。持有一个 Session 和一个 TerminalDisplay 视图。
 *
 * 对应C++: struct TermWidgetImpl
 */
struct TermWidgetImpl {
    explicit TermWidgetImpl(QWidget *parent)
    {
        m_session = createSession(parent);
        m_terminalDisplay = createTerminalDisplay(m_session, parent);
    }

    // 对应 Python _create_session()。
    Session *createSession(QWidget *parent)
    {
        auto *session = new Session(parent);

        session->setTitle(Session::NameRole, QStringLiteral("QTermWidget"));

        // 设置 shell 程序 — 各平台的默认 shell 完全不同,必须分开处理。
        QString shellProgram;
#ifdef Q_OS_WIN
        // Windows: 优先 PowerShell 7+ (pwsh),其次系统自带 powershell.exe。
        shellProgram = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
        if (shellProgram.isEmpty())
            shellProgram = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        if (shellProgram.isEmpty()) {
            // 最后兜底:System32 下的 cmd.exe。
            const QString sysRoot = qEnvironmentVariable("SYSTEMROOT", QStringLiteral("C:\\Windows"));
            shellProgram = sysRoot + QStringLiteral("\\System32\\cmd.exe");
        }
#else
        // Unix: 优先使用环境变量 SHELL。
        const QByteArray shellEnv = qgetenv("SHELL");
        shellProgram = shellEnv.isEmpty()
            ? QStringLiteral("/bin/bash")
            : QString::fromLocal8Bit(shellEnv);
#endif
        session->setProgram(shellProgram);

        // 修复(Python 移植 bug):不应传递空字符串参数,而是空列表。
        session->setArguments(QStringList());
        session->setAutoClose(true);

        session->setFlowControlEnabled(true);
        session->setHistoryType(HistoryTypeBuffer(1000));

        session->setDarkBackground(true);
        session->setKeyBindings(QString());

        return session;
    }

    // 对应 Python _create_terminal_display()。
    TerminalDisplay *createTerminalDisplay(Session *session, QWidget *parent)
    {
        auto *display = new TerminalDisplay(parent);

        // 响铃模式
        display->setBellMode(Konsole::NotifyBell);
        // 终端尺寸提示
        display->setTerminalSizeHint(true);
        // 三击模式
        display->setTripleClickMode(Konsole::SelectWholeLine);
        // 终端启动尺寸
        display->setTerminalSizeStartup(true);
        // 随机种子
        display->setRandomSeed(session->sessionId() * 31);

        return display;
    }

    Session *m_session = nullptr;           // 对应C++: Session* m_session
    TerminalDisplay *m_terminalDisplay = nullptr;  // 对应C++: TerminalDisplay* m_terminalDisplay
};

// ---------------------------------------------------------------------------
// QTermWidget
// ---------------------------------------------------------------------------

// 对应C++: QTermWidget::QTermWidget(int startnow, QWidget* parent)
QTermWidget::QTermWidget(int startnow, QWidget *parent)
    : QWidget(parent)
{
    init(startnow);
}

// 对应 Python __del__()。C++ 依赖 Qt 的对象树自动回收子对象,
// 只需发出销毁信号;无需手动 disconnect / deleteLater。
QTermWidget::~QTermWidget()
{
    Q_EMIT terminalDestroyed();
}

void QTermWidget::init(int startnow)
{
    // 布局
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    setLayout(m_layout);

    // 翻译
    setupTranslations();

    // 内部实现
    m_impl = std::make_unique<TermWidgetImpl>(this);
    m_layout->addWidget(m_impl->m_terminalDisplay);

    // 会话信号
    connectSessionSignals();

    // URL 过滤器
    setupUrlFilter();

    // 自定义高亮过滤器 (WindTerm 风格)
    // 对应 Python setup_syntax_highlighting() -> setup_custom_filters()
    setupCustomFilters();

    // 搜索栏
    setupSearchBar();

    // 焦点
    setFocus(Qt::OtherFocusReason);
    setFocusPolicy(Qt::WheelFocus);
    m_impl->m_terminalDisplay->resize(size());

    // 焦点代理
    setFocusProxy(m_impl->m_terminalDisplay);

    // 终端显示信号
    connectTerminalDisplaySignals();

    // 默认字体
    setupDefaultFont();

    // 默认配置
    setScrollBarPosition(NoScrollBar);
    setKeyboardCursorShape(KeyboardCursorShape::BlockCursor);

    // 关键:必须在 run() 之前 addView,这样 shell 启动时尺寸变化信号才能传递。
    m_impl->m_session->addView(m_impl->m_terminalDisplay);

    // 会话事件信号
    connectSessionEvents();

    // 如果需要立即启动 — 放在所有连接建立之后。
    if (startnow && m_impl->m_session) {
        m_impl->m_session->run();
    }
}

void QTermWidget::setupTranslations()
{
    // 对应 Python _setup_translations()。
    // 探测顺序与 tools.cpp 一致：bundle Resources → 可执行文件旁 resources/ →
    // 编译期宏指向的源码树 cpp/resources/，最后才回退 XDG 系统目录。
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList dirs;
#ifdef Q_OS_MAC
    dirs << appDir + QLatin1String("/../Resources/translations/qtermwidget");
#endif
    dirs << appDir + QLatin1String("/resources/translations/qtermwidget");
#ifdef TRANSLATIONS_DIR
    dirs << QStringLiteral(TRANSLATIONS_DIR);
#endif
    const QByteArray xdgDataDirs = qgetenv("XDG_DATA_DIRS");
    if (!xdgDataDirs.isEmpty()) {
        dirs += QString::fromLocal8Bit(xdgDataDirs).split(QLatin1Char(':'));
    } else {
        dirs << QStringLiteral("/usr/local/share") << QStringLiteral("/usr/share");
    }

    m_translator = new QTranslator(this);
    for (const QString &dirPath : dirs) {
        if (m_translator->load(QLocale::system(), QStringLiteral("qtermwidget"),
                               QStringLiteral("_"), dirPath)) {
            QApplication::installTranslator(m_translator);
            break;
        }
    }
}

void QTermWidget::connectSessionSignals()
{
    // 对应 Python _connect_session_signals()。C++ 的 connect 在编译期校验,
    // 不再需要 Python 的 hasattr 防御。

    // 响铃:session.bellRequest -> terminalDisplay.bell
    connect(m_impl->m_session, &Session::bellRequest,
            m_impl->m_terminalDisplay, &TerminalDisplay::bell);
    // terminalDisplay.notifyBell -> this.bell
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::notifyBell,
            this, &QTermWidget::bell);

    // 活动 / 静默
    connect(m_impl->m_session, &Session::activity, this, &QTermWidget::activity);
    connect(m_impl->m_session, &Session::silence, this, &QTermWidget::silence);

    // 配置改变
    connect(m_impl->m_session, &Session::profileChanged,
            this, &QTermWidget::profileChanged);

    // 接收数据
    connect(m_impl->m_session, &Session::receivedData,
            this, &QTermWidget::receivedData);
}

void QTermWidget::setupUrlFilter()
{
    // 对应 Python _setup_url_filter()。
    auto *urlFilter = new UrlFilter();
    connect(urlFilter, &UrlFilter::activated, this, &QTermWidget::urlActivated);
    m_impl->m_terminalDisplay->filterChain()->addFilter(urlFilter);
}

void QTermWidget::setupCustomFilters()
{
    // 对应 Python setup_custom_filters():设置自定义高亮过滤器 (WindTerm 风格)。
    // 过滤器链拥有过滤器所有权,随 TerminalDisplay 销毁。
    FilterChain *filterChain = m_impl->m_terminalDisplay->filterChain();

    // 1. 权限字符串高亮 (drwxr-xr-x)
    filterChain->addFilter(new PermissionHighlightFilter());

    // 2. 数字高亮 (紫色)
    // 匹配独立的数字或者文件大小等,但不匹配包含数字的文件名(如 file1.txt, 123.log)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"((?<!\S)\d+(?!\S))"), QColor("#bd93f9")));

    // 3. 日期时间高亮 (绿色)
    // 匹配像 "Nov 29" 或 "11:30" 或 "2025-11-29"
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"(\b(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+\d+\b|\b\d{2}:\d{2}\b|\b\d{4}-\d{2}-\d{2}\b)"),
        QColor("#50fa7b")));

    // 4. 压缩包文件名高亮 (天蓝色) — Python 版已注释停用,保持一致
    // filterChain->addFilter(new HighlightFilter(
    //     QStringLiteral(R"(\b[\w\-\.]+\.(?:zip|tar\.gz|tgz|rar|7z|gz|bz2|xz)\b)"),
    //     QColor("#8be9fd")));

    // 命令行关键字高亮
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"((?<![\w\-])(?:sudo\s+)?(?:ls|cd|vi|vim|cat|grep|tail|head|tar|zip|unzip|ssh|scp|find|chmod|chown|ps|kill|ss|systemctl|docker|service|journalctl|top|htop|netstat|ip|ifconfig)\b)"),
        QColor("#00A1FF")));

    // 命令行选项高亮 (-x / --xxx, 黄色)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"((?<!\w)(--?[a-zA-Z0-9][\w\-]*))"), QColor("#f1c40f")));

    // 绝对路径 / ~ 路径高亮 (天蓝色)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"((?:^|[\s;])((?:/[^ \t\n]+|~[^ \t\n]+)))"), QColor("#8be9fd")));

    // IP 地址高亮 (橙色)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)"), QColor("#e67e22")));

    // URL 高亮 (蓝色)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral(R"(\bhttps?://[^\s]+\b)"), QColor("#3498db")));

    // 常见错误信息高亮 (红色)
    filterChain->addFilter(new HighlightFilter(
        QStringLiteral("(command not found|No such file or directory|Permission denied|not recognized)"),
        QColor("#e74c3c")));
}

void QTermWidget::setupSearchBar()
{
    // 对应 Python _setup_search_bar()。
    m_searchBar = new SearchBar(this);
    m_searchBar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Maximum);

    connect(m_searchBar, &SearchBar::searchCriteriaChanged, this, &QTermWidget::find);
    connect(m_searchBar, &SearchBar::findNext, this, &QTermWidget::findNext);
    connect(m_searchBar, &SearchBar::findPrevious, this, &QTermWidget::findPrevious);

    m_layout->addWidget(m_searchBar);
    m_searchBar->hide();
}

void QTermWidget::connectTerminalDisplaySignals()
{
    // 对应 Python _connect_terminal_display_signals()。
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::copyAvailable,
            this, &QTermWidget::selectionChanged);
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::termGetFocus,
            this, &QTermWidget::termGetFocus);
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::termLostFocus,
            this, &QTermWidget::termLostFocus);
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::keyPressedSignal,
            this, [this](QKeyEvent *e, bool) { Q_EMIT termKeyPressed(e); });
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::configureRequest,
            this, &QTermWidget::showContextMenu);
    // Ctrl/Cmd+滚轮缩放字体。
    // 对应Python: cube-shell.py::SSHQTermWidget.eventFilter → zoom_in/zoom_out
    connect(m_impl->m_terminalDisplay, &TerminalDisplay::zoomRequested,
            this, [this](bool in) { in ? zoomIn() : zoomOut(); });

    // 键盘事件到模拟器的连接在 Session::addView() 中建立(遵循 C++ 逻辑)。
}

void QTermWidget::setupDefaultFont()
{
    // 对应 Python _setup_default_font()。
    QFont font = QApplication::font();
    font.setFamily(QTERMWIDGET_DEFAULT_FONT_FAMILY);
    font.setPointSize(10);
    font.setStyleHint(QFont::TypeWriter);
    setTerminalFont(font);
    m_searchBar->setFont(font);
}

void QTermWidget::connectSessionEvents()
{
    // 对应 Python _connect_session_events()。
    connect(m_impl->m_session, &Session::resizeRequest, this, &QTermWidget::setSize);
    connect(m_impl->m_session, &Session::finished, this, &QTermWidget::sessionFinished);
    connect(m_impl->m_session, &Session::titleChanged, this, &QTermWidget::titleChanged);
    connect(m_impl->m_session, &Session::cursorChanged, this, &QTermWidget::cursorChanged);
}

// ---------------------------------------------------------------------------
// 访问器(cube-shell 钩子)
// ---------------------------------------------------------------------------

TerminalDisplay *QTermWidget::terminalDisplay() const
{
    return m_impl ? m_impl->m_terminalDisplay : nullptr;
}

Session *QTermWidget::session() const
{
    return m_impl ? m_impl->m_session : nullptr;
}

// 对应Python: cube-shell.py::_should_disable_command_suggestions (L7530-7546)
// 终端进入 alternate screen（vim/less/top 等）时不应弹出命令补全提示；
// session/emulation 任一级为空或非 Vt102Emulation 时返回 false。
bool QTermWidget::isAppScreenMode() const
{
    Session *s = session();
    if (!s)
        return false;
    auto *vt = qobject_cast<Vt102Emulation *>(s->emulation());
    return vt ? vt->isAppScreenMode() : false;
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

QSize QTermWidget::sizeHint() const
{
    // 对应 Python sizeHint()。
    QSize size = m_impl->m_terminalDisplay->sizeHint();
    size.setHeight(150);
    return size;
}

void QTermWidget::setTerminalSizeHint(bool enabled)
{
    m_impl->m_terminalDisplay->setTerminalSizeHint(enabled);
}

bool QTermWidget::terminalSizeHint() const
{
    return m_impl->m_terminalDisplay->terminalSizeHint();
}

void QTermWidget::startShellProgram()
{
    if (m_impl->m_session->isRunning()) {
        return;
    }
    m_impl->m_session->run();
}

void QTermWidget::startTerminalTeletype()
{
    if (m_impl->m_session->isRunning()) {
        return;
    }
    m_impl->m_session->runEmptyPTY();
    // 重定向数据到外部接收者。
    if (Emulation *emulation = m_impl->m_session->emulation()) {
        connect(emulation, &Emulation::sendData, this, &QTermWidget::sendData);
    }
}

int QTermWidget::getShellPID()
{
    return m_impl->m_session->processId();
}

int QTermWidget::getForegroundProcessId()
{
    return m_impl->m_session->foregroundProcessId();
}

bool QTermWidget::getIsRunning()
{
    return m_impl->m_session->isRunning();
}

void QTermWidget::changeDir(const QString &dir)
{
    // 对应 Python changeDir()。黑客方式判断 shell 是否在前台(仅 POSIX)。
    const int shellPid = getShellPID();
    const QString cmd = QStringLiteral(
        "ps -j %1 | tail -1 | awk '{ print $5 }' | grep -q \\+").arg(shellPid);
    const int retval = std::system(cmd.toLocal8Bit().constData());

    if (retval == 0) {
        sendText(QStringLiteral("cd %1\n").arg(dir));
    }
}

void QTermWidget::setTerminalFont(const QFont &font)
{
    m_impl->m_terminalDisplay->setVTFont(font);
}

QFont QTermWidget::getTerminalFont() const
{
    return m_impl->m_terminalDisplay->getVTFont();
}

void QTermWidget::setTerminalOpacity(qreal level)
{
    m_impl->m_terminalDisplay->setOpacity(level);
}

void QTermWidget::setTerminalBackgroundImage(const QString &backgroundImage)
{
    m_impl->m_terminalDisplay->setBackgroundImage(backgroundImage);
}

void QTermWidget::setTerminalBackgroundMode(int mode)
{
    m_impl->m_terminalDisplay->setBackgroundMode(
        static_cast<Konsole::BackgroundMode>(mode));
}

void QTermWidget::setSuppressProgramBackgroundColors(bool suppress)
{
    m_impl->m_terminalDisplay->setSuppressProgramBackgroundColors(suppress);
}

void QTermWidget::setShellProgram(const QString &program)
{
    if (!m_impl->m_session) {
        return;
    }
    m_impl->m_session->setProgram(program);
}

void QTermWidget::setWorkingDirectory(const QString &dir)
{
    if (!m_impl->m_session) {
        return;
    }
    m_impl->m_session->setInitialWorkingDirectory(dir);
}

QString QTermWidget::workingDirectory()
{
    if (!m_impl->m_session) {
        return QString();
    }

#ifdef Q_OS_LINUX
    // Linux 上读取 /proc/<pid>/cwd。
    const QString procDir = QStringLiteral("/proc/%1/cwd").arg(getShellPID());
    if (QFileInfo::exists(procDir)) {
        return QFileInfo(procDir).canonicalFilePath();
    }
#endif

    // 回退到初始工作目录。
    return m_impl->m_session->initialWorkingDirectory();
}

void QTermWidget::setArgs(const QStringList &args)
{
    if (!m_impl->m_session) {
        return;
    }
    m_impl->m_session->setArguments(args);
}

void QTermWidget::setColorScheme(const QString &origName)
{
    // 对应 Python setColorScheme()。
    const ColorScheme *cs = nullptr;

    const bool isFile = QFileInfo::exists(origName);
    const QString name = isFile
        ? QFileInfo(origName).completeBaseName()
        : origName;

    if (!availableColorSchemes().contains(name)) {
        if (isFile) {
            if (ColorSchemeManager::instance()->loadCustomColorScheme(origName)) {
                cs = ColorSchemeManager::instance()->findColorScheme(name);
            } else {
                qWarning("cannot load color scheme from %s", qPrintable(origName));
            }
        }
        if (!cs) {
            cs = ColorSchemeManager::instance()->defaultColorScheme();
        }
    } else {
        cs = ColorSchemeManager::instance()->findColorScheme(name);
    }

    if (!cs) {
        QMessageBox::information(this, tr("Color Scheme Error"),
                                 tr("Cannot load color scheme: %1").arg(name));
        return;
    }

    ColorEntry colorTable[TABLE_COLORS];
    cs->getColorTable(colorTable);
    m_impl->m_terminalDisplay->setColorTable(colorTable);
    m_impl->m_session->setDarkBackground(cs->hasDarkBackground());
}

QStringList QTermWidget::getAvailableColorSchemes() const
{
    return availableColorSchemes();
}

QStringList QTermWidget::availableColorSchemes()
{
    QStringList ret;
    const QList<const ColorScheme *> allSchemes =
        ColorSchemeManager::instance()->allColorSchemes();
    ret.reserve(allSchemes.size());
    for (const ColorScheme *cs : allSchemes) {
        ret.append(cs->name());
    }
    return ret;
}

void QTermWidget::addCustomColorSchemeDir(const QString &customDir)
{
    ColorSchemeManager::instance()->addCustomColorSchemeDir(customDir);
}

void QTermWidget::setHistorySize(int lines)
{
    if (lines < 0) {
        m_impl->m_session->setHistoryType(HistoryTypeFile());
    } else if (lines == 0) {
        m_impl->m_session->setHistoryType(HistoryTypeNone());
    } else {
        m_impl->m_session->setHistoryType(HistoryTypeBuffer(lines));
    }
}

int QTermWidget::historySize()
{
    const HistoryType &currentHistory = m_impl->m_session->historyType();

    if (currentHistory.isEnabled()) {
        if (currentHistory.isUnlimited()) {
            return -1;
        }
        return currentHistory.maximumLineCount();
    }
    return 0;
}

void QTermWidget::setScrollBarPosition(ScrollBarPosition pos)
{
    // 顶层 using 引入的 ScrollBarPosition 即 Konsole::ScrollBarPosition,直接透传。
    m_impl->m_terminalDisplay->setScrollBarPosition(pos);
}

void QTermWidget::scrollToEnd()
{
    m_impl->m_terminalDisplay->scrollToEnd();
}

void QTermWidget::sendText(const QString &text)
{
    m_impl->m_session->sendText(text);
}

void QTermWidget::sendKeyEvent(QKeyEvent *e)
{
    m_impl->m_session->sendKeyEvent(e);
}

void QTermWidget::setFlowControlEnabled(bool enabled)
{
    m_impl->m_session->setFlowControlEnabled(enabled);
}

bool QTermWidget::flowControlEnabled()
{
    return m_impl->m_session->flowControlEnabled();
}

void QTermWidget::setFlowControlWarningEnabled(bool enabled)
{
    if (flowControlEnabled()) {
        m_impl->m_terminalDisplay->setFlowControlWarningEnabled(enabled);
    }
}

QString QTermWidget::keyBindings()
{
    return m_impl->m_session->keyBindings();
}

void QTermWidget::setMotionAfterPasting(int motion)
{
    m_impl->m_terminalDisplay->setMotionAfterPasting(
        static_cast<Konsole::MotionAfterPasting>(motion));
}

int QTermWidget::historyLinesCount()
{
    return m_impl->m_terminalDisplay->screenWindow()->screen()->getHistLines();
}

int QTermWidget::screenColumnsCount()
{
    return m_impl->m_terminalDisplay->screenWindow()->screen()->getColumns();
}

int QTermWidget::screenLinesCount()
{
    return m_impl->m_terminalDisplay->screenWindow()->screen()->getLines();
}

void QTermWidget::setSelectionStart(int row, int column)
{
    m_impl->m_terminalDisplay->screenWindow()->screen()->setSelectionStart(column, row, true);
}

void QTermWidget::setSelectionEnd(int row, int column)
{
    m_impl->m_terminalDisplay->screenWindow()->screen()->setSelectionEnd(column, row);
}

void QTermWidget::getSelectionStart(int &row, int &column)
{
    // 对应 Python getSelectionStart();C++ 用引用参数返回。
    m_impl->m_terminalDisplay->screenWindow()->screen()->getSelectionStart(column, row);
}

void QTermWidget::getSelectionEnd(int &row, int &column)
{
    m_impl->m_terminalDisplay->screenWindow()->screen()->getSelectionEnd(column, row);
}

QString QTermWidget::selectedText(bool preserveLineBreaks)
{
    return m_impl->m_terminalDisplay->screenWindow()->screen()->selectedText(preserveLineBreaks);
}

void QTermWidget::clearSelection()
{
    // cube-shell 公共 API:清除当前选择。
    m_impl->m_terminalDisplay->screenWindow()->screen()->clearSelection();
}

bool QTermWidget::findText(const QString &text, bool forwards, bool caseSensitive)
{
    // cube-shell 公共 API。封装为一次性的 HistorySearch。
    if (!m_impl->m_session || !m_impl->m_session->emulation()) {
        return false;
    }

    int startColumn = 0;
    int startLine = 0;
    m_impl->m_terminalDisplay->screenWindow()->screen()->getSelectionStart(startColumn, startLine);

    QRegularExpression regExp(QRegularExpression::escape(text));
    regExp.setPatternOptions(caseSensitive
        ? QRegularExpression::NoPatternOption
        : QRegularExpression::CaseInsensitiveOption);

    auto *historySearch = new HistorySearch(m_impl->m_session->emulation(), regExp,
                                            forwards, startColumn, startLine, this);

    bool found = false;
    connect(historySearch, &HistorySearch::matchFound, this,
            [this, &found](int sc, int sl, int ec, int el) {
                found = true;
                matchFound(sc, sl, ec, el);
            });
    connect(historySearch, &HistorySearch::noMatchFound, this, &QTermWidget::noMatchFound);

    historySearch->search();
    historySearch->deleteLater();
    return found;
}

void QTermWidget::setMonitorActivity(bool enabled)
{
    m_impl->m_session->setMonitorActivity(enabled);
}

void QTermWidget::setMonitorSilence(bool enabled)
{
    m_impl->m_session->setMonitorSilence(enabled);
}

void QTermWidget::setSilenceTimeout(int seconds)
{
    m_impl->m_session->setMonitorSilenceSeconds(seconds);
}

QList<QAction *> QTermWidget::filterActions(const QPoint &position)
{
    return m_impl->m_terminalDisplay->filterActions(position);
}

int QTermWidget::getPtySlaveFd()
{
    return m_impl->m_session->getPtySlaveFd();
}

void QTermWidget::setBlinkingCursor(bool blink)
{
    m_impl->m_terminalDisplay->setBlinkingCursor(blink);
}

void QTermWidget::setBidiEnabled(bool enabled)
{
    m_impl->m_terminalDisplay->setBidiEnabled(enabled);
}

bool QTermWidget::isBidiEnabled()
{
    return m_impl->m_terminalDisplay->isBidiEnabled();
}

void QTermWidget::setAutoClose(bool enabled)
{
    m_impl->m_session->setAutoClose(enabled);
}

QString QTermWidget::title() const
{
    QString title = m_impl->m_session->userTitle();
    if (title.isEmpty()) {
        title = m_impl->m_session->title(Session::NameRole);
    }
    return title;
}

QString QTermWidget::icon() const
{
    QString icon = m_impl->m_session->iconText();
    if (icon.isEmpty()) {
        icon = m_impl->m_session->iconName();
    }
    return icon;
}

bool QTermWidget::isTitleChanged()
{
    return m_impl->m_session->isTitleChanged();
}

void QTermWidget::bracketText(QString &text)
{
    // 对应C++: void bracketText(QString& text)。TerminalDisplay::bracketText 返回
    // 处理后的文本(参考参数在 Python 里无法就地修改),这里回写。
    text = m_impl->m_terminalDisplay->bracketText(text);
}

void QTermWidget::disableBracketedPasteMode(bool disable)
{
    m_impl->m_terminalDisplay->disableBracketedPasteMode(disable);
}

bool QTermWidget::bracketedPasteModeIsDisabled()
{
    return m_impl->m_terminalDisplay->bracketedPasteModeIsDisabled();
}

void QTermWidget::setMargin(int margin)
{
    m_impl->m_terminalDisplay->setMargin(margin);
}

int QTermWidget::getMargin()
{
    return m_impl->m_terminalDisplay->margin();
}

void QTermWidget::setDrawLineChars(bool drawLineChars)
{
    m_impl->m_terminalDisplay->setDrawLineChars(drawLineChars);
}

void QTermWidget::setBoldIntense(bool boldIntense)
{
    m_impl->m_terminalDisplay->setBoldIntense(boldIntense);
}

void QTermWidget::setConfirmMultilinePaste(bool confirmMultilinePaste)
{
    m_impl->m_terminalDisplay->setConfirmMultilinePaste(confirmMultilinePaste);
}

void QTermWidget::setTrimPastedTrailingNewlines(bool trimPastedTrailingNewlines)
{
    m_impl->m_terminalDisplay->setTrimPastedTrailingNewlines(trimPastedTrailingNewlines);
}

QString QTermWidget::wordCharacters()
{
    return m_impl->m_terminalDisplay->wordCharacters();
}

void QTermWidget::setWordCharacters(const QString &chars)
{
    m_impl->m_terminalDisplay->setWordCharacters(chars);
}

QTermWidget *QTermWidget::createWidget(int startnow)
{
    return new QTermWidget(startnow);
}

void QTermWidget::autoHideMouseAfter(int delay)
{
    m_impl->m_terminalDisplay->autoHideMouseAfter(delay);
}

void QTermWidget::setEnvironment(const QStringList &environment)
{
    m_impl->m_session->setEnvironment(environment);
}

QStringList QTermWidget::availableKeyBindings()
{
    return KeyboardTranslatorManager::instance()->allTranslators();
}

Filter::HotSpot *QTermWidget::getHotSpotAt(const QPoint &pos)
{
    int row = 0;
    int column = 0;
    // TerminalDisplay::getCharacterPosition(point, line, column)
    m_impl->m_terminalDisplay->getCharacterPosition(QPointF(pos), row, column);
    return getHotSpotAt(row, column);
}

Filter::HotSpot *QTermWidget::getHotSpotAt(int row, int column)
{
    return m_impl->m_terminalDisplay->filterChain()->hotSpotAt(row, column);
}

void QTermWidget::setKeyboardCursorShape(KeyboardCursorShape shape)
{
    m_impl->m_terminalDisplay->setKeyboardCursorShape(shape);
}

void QTermWidget::closeTerminal()
{
    // 对应 Python close()。Qt 的对象树负责回收子对象,
    // 这里安全地关闭会话(终止进程)。
    if (m_impl && m_impl->m_session) {
        m_impl->m_session->close();
    }
}

// ---------------------------------------------------------------------------
// 公共槽
// ---------------------------------------------------------------------------

void QTermWidget::copyClipboard()
{
    m_impl->m_terminalDisplay->copyClipboard();
}

void QTermWidget::pasteClipboard()
{
    m_impl->m_terminalDisplay->pasteClipboard();
}

void QTermWidget::pasteSelection()
{
    m_impl->m_terminalDisplay->pasteSelection();
}

void QTermWidget::zoomIn()
{
    setZoom(QTERMWIDGET_STEP_ZOOM);
}

void QTermWidget::zoomOut()
{
    setZoom(-QTERMWIDGET_STEP_ZOOM);
}

void QTermWidget::setSize(const QSize &size)
{
    m_impl->m_terminalDisplay->setSize(size.width(), size.height());
}

void QTermWidget::setKeyBindings(const QString &kb)
{
    m_impl->m_session->setKeyBindings(kb);
}

void QTermWidget::clear()
{
    // 对应 Python clear()。清屏并移动光标到 home,再清空历史。
    if (Emulation *emulation = m_impl->m_session->emulation()) {
        emulation->clearEntireScreen();
        // clearEntireScreen() 只擦内容不动光标 —— 这是 VT 的 ED(ESC[2J) 语义,
        // 必须保持,否则 ESC[2J 后跟 ESC[H 的程序会被清屏挪走光标而画错。
        // 但"清屏"按钮是 UI 动作,用户期望光标回到左上角。串口终端尤其明显:
        // 对端是裸设备,不会像 shell 那样重画提示符把光标带回去,
        // 光标会一直停在原来的行列上。故在此单独补一次 home。
        emulation->home();
    }

#ifdef Q_OS_WIN
    // 兼容 Windows 连接本地终端的清屏。
    const QString program = m_impl->m_session ? m_impl->m_session->program() : QString();
    const QString base = QFileInfo(program).fileName().toLower();
    if (base != QLatin1String("ssh") && base != QLatin1String("ssh.exe")) {
        sendText(QStringLiteral("cls\r"));
        m_impl->m_session->clearHistory();
        return;
    }
#endif

    m_impl->m_session->refresh();
    m_impl->m_session->clearHistory();
}

void QTermWidget::toggleShowSearchBar()
{
    if (m_searchBar->isHidden()) {
        m_searchBar->show();
    } else {
        m_searchBar->hide();
    }
}

void QTermWidget::saveHistory(QIODevice *device)
{
    // 对应 Python saveHistory()。经 PlainTextDecoder 写历史到设备。
    if (!device) {
        return;
    }
    PlainTextDecoder decoder;
    Emulation *emulation = m_impl->m_session->emulation();
    if (!emulation) {
        return;
    }
    QTextStream stream(device);
    decoder.begin(&stream);
    emulation->writeToStream(&decoder, 0, emulation->lineCount());
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------

void QTermWidget::resizeEvent(QResizeEvent *event)
{
    m_impl->m_terminalDisplay->resize(size());
    QWidget::resizeEvent(event);
}

void QTermWidget::closeEvent(QCloseEvent *event)
{
    // 对应 Python closeEvent():窗口关闭时安全终止终端进程。
    if (m_impl && m_impl->m_session && m_impl->m_session->isRunning()) {
        m_impl->m_session->close();
    }
    event->accept();
}

// ---------------------------------------------------------------------------
// 私有槽
// ---------------------------------------------------------------------------

void QTermWidget::sessionFinished()
{
    Q_EMIT finished();
}

void QTermWidget::selectionChanged(bool textSelected)
{
    Q_EMIT copyAvailable(textSelected);
}

void QTermWidget::find()
{
    search(true, false);
}

void QTermWidget::findNext()
{
    search(true, true);
}

void QTermWidget::findPrevious()
{
    search(false, false);
}

void QTermWidget::matchFound(int startColumn, int startLine, int endColumn, int endLine)
{
    ScreenWindow *sw = m_impl->m_terminalDisplay->screenWindow();
    sw->scrollTo(startLine);
    sw->setTrackOutput(false);
    sw->notifyOutputChanged();
    sw->setSelectionStart(startColumn, startLine - sw->currentLine(), false);
    sw->setSelectionEnd(endColumn, endLine - sw->currentLine());
}

void QTermWidget::noMatchFound()
{
    m_impl->m_terminalDisplay->screenWindow()->clearSelection();
}

void QTermWidget::cursorChanged(KeyboardCursorShape cursorShape, bool blinkingCursorEnabled)
{
    setKeyboardCursorShape(cursorShape);
    setBlinkingCursor(blinkingCursorEnabled);
}

void QTermWidget::showContextMenu(const QPoint &pos)
{
    // 对应 Python contextMenuEvent()/_add_custom_actions():终端右键菜单。
    // 严格复刻 Python 版 5 项:复制/粘贴 | 清屏 | 切换终端主题 | AI。
    QMenu menu(this);

    QAction *copyAction =
        menu.addAction(QIcon(QStringLiteral(":/copy.png")), QStringLiteral("复制"));
    copyAction->setIconVisibleInMenu(true);
    connect(copyAction, &QAction::triggered, this, &QTermWidget::copyClipboard);

    QAction *pasteAction =
        menu.addAction(QIcon(QStringLiteral(":/paste.png")), QStringLiteral("粘贴"));
    pasteAction->setIconVisibleInMenu(true);
    pasteAction->setEnabled(!QApplication::clipboard()->text().isEmpty());
    connect(pasteAction, &QAction::triggered, this, &QTermWidget::pasteClipboard);

    menu.addSeparator();

    QAction *clearAction =
        menu.addAction(QIcon(QStringLiteral(":/clear.png")), QStringLiteral("清屏"));
    clearAction->setIconVisibleInMenu(true);
    // 发送 Ctrl+L(\x0c)由 shell 自行清屏并重绘提示符,
    // 避免 clear() 擦空整个缓冲区导致提示符也消失。
    connect(clearAction, &QAction::triggered, this, [this]() {
        sendText(QStringLiteral("\x0c"));
    });

    menu.addSeparator();

    // 切换终端主题 — 子菜单列出所有可用配色方案。
    // 对应 Python show_theme_selector()/apply_theme()。
    QMenu *themeMenu = menu.addMenu(QIcon(QStringLiteral(":/icons8-bg-48.png")),
                                    QStringLiteral("切换终端主题"));
    const QStringList schemes = availableColorSchemes();
    for (const QString &name : schemes) {
        QAction *schemeAction = themeMenu->addAction(name);
        connect(schemeAction, &QAction::triggered, this,
                [this, name] { setColorScheme(name); });
    }

    menu.addSeparator();

    QAction *aiAction =
        menu.addAction(QIcon(QStringLiteral(":/icons8-ai-48.png")), QStringLiteral("AI"));
    aiAction->setIconVisibleInMenu(true);
    connect(aiAction, &QAction::triggered, this,
            [this] { Q_EMIT aiRequested(); });

    menu.exec(m_impl->m_terminalDisplay->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// 私有方法
// ---------------------------------------------------------------------------

void QTermWidget::search(bool forwards, bool next)
{
    // 对应 Python search()。
    int startColumn = 0;
    int startLine = 0;
    if (next) {  // 从当前选择后搜索
        m_impl->m_terminalDisplay->screenWindow()->screen()->getSelectionEnd(startColumn, startLine);
        startColumn += 1;
    } else {  // 从当前选择开始搜索
        m_impl->m_terminalDisplay->screenWindow()->screen()->getSelectionStart(startColumn, startLine);
    }

    QRegularExpression regExp;
    if (m_searchBar->useRegularExpression()) {
        regExp.setPattern(m_searchBar->searchText());
    } else {
        regExp.setPattern(QRegularExpression::escape(m_searchBar->searchText()));
    }

    regExp.setPatternOptions(m_searchBar->matchCase()
        ? QRegularExpression::NoPatternOption
        : QRegularExpression::CaseInsensitiveOption);

    auto *historySearch = new HistorySearch(m_impl->m_session->emulation(), regExp,
                                            forwards, startColumn, startLine, this);
    connect(historySearch, &HistorySearch::matchFound, this, &QTermWidget::matchFound);
    connect(historySearch, &HistorySearch::noMatchFound, this, &QTermWidget::noMatchFound);
    connect(historySearch, &HistorySearch::noMatchFound, m_searchBar, &SearchBar::noMatchFound);

    historySearch->search();
    historySearch->deleteLater();
}

void QTermWidget::setZoom(int step)
{
    QFont font = m_impl->m_terminalDisplay->getVTFont();
    // 字号限制 8–28。对应Python: cube-shell.py::zoom_in(<28)/zoom_out(>8)
    const int newSize = qBound(8, font.pointSize() + step, 28);
    if (newSize == font.pointSize())
        return;
    font.setPointSize(newSize);
    setTerminalFont(font);
    emit fontSizeChanged(newSize);
}

// ---------------------------------------------------------------------------
// 工厂函数
// ---------------------------------------------------------------------------

QTermWidget *createTermWidget(int startnow, QWidget *parent)
{
    return new QTermWidget(startnow, parent);
}
