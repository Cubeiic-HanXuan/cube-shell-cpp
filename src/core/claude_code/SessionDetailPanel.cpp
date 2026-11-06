// SessionDetailPanel.cpp — 会话详情的分页状态机与时间线 / 目录联动。
// 读取、导出在后端执行；此处仅更新界面状态，并在切换会话后忽略过期回调。

#include "claude_code/SessionDetailPanel.h"
#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/SessionTimelineView.h"
#include "claude_code/SessionWidgets.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace cubeshell {
using namespace sessionui;
namespace {
// 页头可能显示很长的路径 / 标题，绘制时省略而不把窗口的最小宽度撑大。
class HeaderLabel : public QLabel {
public:
    explicit HeaderLabel(QWidget *parent) : QLabel(parent)
    { setTextFormat(Qt::PlainText); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this); p.setFont(font()); p.setPen(palette().color(QPalette::WindowText));
        p.drawText(rect(), Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, width()));
    }
};
QString dateTime(qint64 ms)
{
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("未知");
}
}
SessionDetailPanel::SessionDetailPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent), m_backend(backend)
{
    buildUi();
    connect(backend, &ClaudeCodeBackend::sessionPageReady, this, &SessionDetailPanel::onPage, Qt::QueuedConnection);
    connect(backend, &ClaudeCodeBackend::sessionSourceChanged, this, &SessionDetailPanel::reset);
    connect(backend, &ClaudeCodeBackend::sessionExported, this,
        [this](quint64 source, quint64 request, const QString &path, const QString &error) {
            if (source != m_source || request != m_exportRequest) return;
            m_exportAction->setEnabled(true);
            if (!error.isEmpty()) QMessageBox::warning(this, tr("导出失败"), error);
            else m_status->setText(tr("已导出：%1").arg(path));
        }, Qt::QueuedConnection);
}
void SessionDetailPanel::buildUi()
{
    const auto c = colors(QApplication::palette());
    setObjectName(QStringLiteral("sessionDetail"));
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    m_header = new QFrame(this); m_header->setObjectName(QStringLiteral("sessionHeader"));
    auto *headerLayout = new QVBoxLayout(m_header); headerLayout->setContentsMargins(12, 10, 12, 12); headerLayout->setSpacing(10);
    auto *top = new QHBoxLayout; top->setSpacing(8);
    auto *back = iconButton(Glyph::Back, tr("返回会话列表（Esc）"), c, m_header);
    back->setObjectName(QStringLiteral("sessionBack"));
    connect(back, &QToolButton::clicked, this, &SessionDetailPanel::backRequested); top->addWidget(back);
    auto *mark = new QLabel(m_header); mark->setObjectName(QStringLiteral("sessionMark"));
    mark->setPixmap(icon(Glyph::Cube, c.accent).pixmap(24, 24)); mark->setFixedSize(28, 28); top->addWidget(mark);
    m_title = new HeaderLabel(m_header); m_title->setObjectName(QStringLiteral("sessionTitle"));
    auto bold = m_title->font(); bold.setBold(true); m_title->setFont(bold); top->addWidget(m_title, 1);
    m_previous = iconButton(Glyph::Previous, tr("上一会话"), c, m_header);
    m_next = iconButton(Glyph::Next, tr("下一会话"), c, m_header);
    top->addWidget(m_previous); top->addWidget(m_next); top->addSpacing(6);
    connect(m_previous, &QToolButton::clicked, this, [this] { emit navigateRequested(-1); });
    connect(m_next, &QToolButton::clicked, this, [this] { emit navigateRequested(1); });
    m_resume = new QPushButton(tr("恢复会话"), m_header); m_resume->setObjectName(QStringLiteral("sessionResume"));
    m_resume->setIcon(icon(Glyph::Play, Qt::white)); m_resume->setMinimumHeight(36);
    m_resume->setToolTip(tr("在所属本地 / SSH 终端中恢复会话"));
    m_resume->setCursor(Qt::PointingHandCursor);
    connect(m_resume, &QPushButton::clicked, this, [this] { emit resumeRequested(m_session); }); top->addWidget(m_resume);
    auto *more = iconButton(Glyph::More, tr("更多操作"), c, m_header);
    auto *menu = new QMenu(more);
    menu->addAction(tr("刷新会话"), this, &SessionDetailPanel::refreshRequested);
    m_exportAction = menu->addAction(tr("导出完整会话…"), this, &SessionDetailPanel::exportFile);
    menu->addAction(tr("复制会话 ID"), this, [this] { QApplication::clipboard()->setText(m_session.sessionId); m_status->setText(tr("已复制会话 ID")); });
    menu->addSeparator();
    menu->addAction(tr("展开所有工具步骤"), this, [this] { m_timeline->setStepsExpanded(true); });
    menu->addAction(tr("收起所有工具步骤"), this, [this] { m_timeline->setStepsExpanded(false); });
    menu->addAction(tr("折叠所有执行过程"), this, [this] { m_timeline->setProcessesCollapsed(true); });
    menu->addAction(tr("展开所有执行过程"), this, [this] { m_timeline->setProcessesCollapsed(false); });
    more->setMenu(menu); more->setPopupMode(QToolButton::InstantPopup); top->addWidget(more);
    headerLayout->addLayout(top);

    auto *info = new QHBoxLayout; info->setSpacing(10);
    auto *projectChip = new QFrame(m_header); projectChip->setObjectName(QStringLiteral("sessionProjectChip"));
    projectChip->setMaximumWidth(410); projectChip->setMinimumWidth(110);
    auto *pathLayout = new QHBoxLayout(projectChip); pathLayout->setContentsMargins(9, 0, 3, 0); pathLayout->setSpacing(6);
    auto *folder = new QLabel(projectChip); folder->setObjectName(QStringLiteral("sessionProjectIcon"));
    folder->setPixmap(icon(Glyph::Folder, c.muted).pixmap(16, 16)); pathLayout->addWidget(folder);
    m_project = new HeaderLabel(projectChip); pathLayout->addWidget(m_project, 1);
    auto *copyPath = iconButton(Glyph::Copy, tr("复制项目路径"), c, projectChip);
    copyPath->setFixedSize(26, 28);
    connect(copyPath, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_session.cwd); m_status->setText(tr("已复制项目路径")); });
    pathLayout->addWidget(copyPath); info->addWidget(projectChip, 2);
    m_metadata = new HeaderLabel(m_header); m_metadata->setObjectName(QStringLiteral("sessionMetadata")); info->addWidget(m_metadata, 3);
    m_failures = new QToolButton(m_header); m_failures->setObjectName(QStringLiteral("sessionFailures"));
    m_failures->setCheckable(true); m_failures->setToolTip(tr("仅查看失败的工具步骤")); m_failures->hide();
    connect(m_failures, &QToolButton::clicked, this, [this](bool checked) { setFilter(checked ? SessionTimelineView::Failures : SessionTimelineView::All); });
    info->addWidget(m_failures);
    auto *segments = new QFrame(m_header); segments->setObjectName(QStringLiteral("sessionSegments"));
    auto *segmentLayout = new QHBoxLayout(segments); segmentLayout->setContentsMargins(3, 3, 3, 3); segmentLayout->setSpacing(2);
    m_filterGroup = new QButtonGroup(this); m_filterGroup->setObjectName(QStringLiteral("sessionDetailFilter"));
    const QStringList filters{tr("全部"), tr("对话"), tr("改动")};
    for (int i = 0; i < filters.size(); ++i) {
        auto *button = new QToolButton(segments); button->setObjectName(QStringLiteral("sessionFilter_%1").arg(i));
        button->setProperty("sessionSegment", true); button->setText(filters[i]); button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor); button->setMinimumSize(46, 27);
        m_filterGroup->addButton(button, i); segmentLayout->addWidget(button);
        if (!i) button->setChecked(true);
    }
    connect(m_filterGroup, &QButtonGroup::idClicked, this, &SessionDetailPanel::setFilter);
    info->addWidget(segments);
    m_directoryToggle = iconButton(Glyph::Sidebar, tr("显示 / 隐藏对话目录"), c, m_header);
    m_directoryToggle->setObjectName(QStringLiteral("sessionDirectoryToggle"));
    m_directoryToggle->setCheckable(true); m_directoryToggle->setChecked(true);
    info->addWidget(m_directoryToggle);
    auto *searchToggle = iconButton(Glyph::Search, tr("搜索会话（%1）").arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)), c, m_header);
    connect(searchToggle, &QToolButton::clicked, this, &SessionDetailPanel::focusSearch); info->addWidget(searchToggle);
    headerLayout->addLayout(info);

    m_searchBar = new QFrame(m_header); m_searchBar->setObjectName(QStringLiteral("sessionSearchBar"));
    auto *searchLayout = new QHBoxLayout(m_searchBar); searchLayout->setContentsMargins(4, 4, 4, 0); searchLayout->setSpacing(6);
    m_search = new QLineEdit(m_searchBar); m_search->setObjectName(QStringLiteral("sessionDetailSearch"));
    m_search->setPlaceholderText(tr("搜索消息、命令、参数与输出…")); m_search->setClearButtonEnabled(true); searchLayout->addWidget(m_search, 1);
    m_matchLabel = new QLabel(m_searchBar); searchLayout->addWidget(m_matchLabel);
    auto *prevMatch = iconButton(Glyph::Previous, tr("上个匹配（Shift+Enter）"), c, m_searchBar);
    auto *nextMatch = iconButton(Glyph::Next, tr("下个匹配（Enter）"), c, m_searchBar);
    auto *closeSearch = iconButton(Glyph::Close, tr("关闭搜索"), c, m_searchBar);
    searchLayout->addWidget(prevMatch); searchLayout->addWidget(nextMatch); searchLayout->addWidget(closeSearch);
    connect(prevMatch, &QToolButton::clicked, this, [this] { findText(true); });
    connect(nextMatch, &QToolButton::clicked, this, [this] { findText(false); });
    connect(closeSearch, &QToolButton::clicked, m_searchBar, &QWidget::hide);
    connect(m_search, &QLineEdit::returnPressed, this, [this] { findText(QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchIndex = -1; m_matchLabel->clear(); });
    headerLayout->addWidget(m_searchBar); m_searchBar->hide(); layout->addWidget(m_header);

    m_splitter = new QSplitter(this); m_splitter->setHandleWidth(1);
    m_timeline = new SessionTimelineView(m_splitter);
    m_directoryPanel = new QFrame(m_splitter); m_directoryPanel->setObjectName(QStringLiteral("sessionDirectoryPanel"));
    m_directoryPanel->setMinimumWidth(225); m_directoryPanel->setMaximumWidth(320);
    auto *directoryLayout = new QVBoxLayout(m_directoryPanel); directoryLayout->setContentsMargins(12, 18, 12, 8); directoryLayout->setSpacing(10);
    auto *directoryHeader = new QHBoxLayout;
    auto *directoryTitle = new QLabel(tr("对话目录"), m_directoryPanel); directoryTitle->setFont(bold);
    directoryHeader->addWidget(directoryTitle); directoryHeader->addStretch();
    m_directoryCount = new QLabel(QStringLiteral("0"), m_directoryPanel); m_directoryCount->setObjectName(QStringLiteral("sessionDirectoryCount"));
    directoryHeader->addWidget(m_directoryCount); directoryLayout->addLayout(directoryHeader);
    m_directory = new QListWidget(m_directoryPanel); m_directory->setObjectName(QStringLiteral("sessionDirectory"));
    m_directory->setItemDelegate(new SessionDirectoryDelegate(m_directory));
    m_directory->setMouseTracking(true); m_directory->setFrameShape(QFrame::NoFrame);
    m_directory->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_directory->setSpacing(3);
    m_directory->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    directoryLayout->addWidget(m_directory);
    // 目录选择会回到完整视图；正文滚动仅更新目录选中态，不反向触发目录点击。
    connect(m_directory, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        setFilter(SessionTimelineView::All); m_timeline->scrollToTurn(item->data(Qt::UserRole).toInt());
    });
    connect(m_directory, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        setFilter(SessionTimelineView::All); m_timeline->scrollToTurn(item->data(Qt::UserRole).toInt());
    });
    connect(m_directoryToggle, &QToolButton::clicked, this, [this](bool on) {
        m_directoryWanted = on; m_directoryOverride = true; applyResponsiveLayout();
    });
    connect(m_timeline, &SessionTimelineView::currentTurnChanged, this, [this](int turn) {
        for (int i = 0; i < m_directory->count(); ++i) {
            auto *item = m_directory->item(i);
            if (item->data(Qt::UserRole).toInt() == turn) {
                m_directory->setCurrentItem(item); m_directory->scrollToItem(item, QAbstractItemView::EnsureVisible); break;
            }
        }
    });
    connect(m_timeline, &SessionTimelineView::needMore, this, [this] { if (!m_rebuildingView && m_loadError.isEmpty()) loadMore(); });
    connect(m_timeline, &SessionTimelineView::copied, this, [this] { m_status->setText(tr("已复制完整内容")); });
    // 历史文本是不受信任的外部内容，仅允许 HTTP(S) 链接，并由用户确认后交给浏览器。
    connect(m_timeline, &SessionTimelineView::externalLinkRequested, this, [this](const QUrl &url) {
        if (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")) return;
        if (QMessageBox::question(this, tr("打开链接"), tr("在浏览器中打开此历史记录中的链接？\n%1").arg(url.toDisplayString())) == QMessageBox::Yes)
            QDesktopServices::openUrl(url);
    });
    m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 0); m_splitter->setSizes({960, 280});
    layout->addWidget(m_splitter, 1);

    auto *footer = new QFrame(this); footer->setObjectName(QStringLiteral("sessionFooter"));
    auto *bottom = new QHBoxLayout(footer); bottom->setContentsMargins(18, 8, 14, 8); bottom->setSpacing(12);
    m_status = new QLabel(footer); m_status->setObjectName(QStringLiteral("sessionStatus")); m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); bottom->addWidget(m_status, 1);
    m_loadMore = new QPushButton(tr("继续读取"), footer); m_loadMore->setObjectName(QStringLiteral("sessionLoadMore"));
    connect(m_loadMore, &QPushButton::clicked, this, &SessionDetailPanel::loadMore); bottom->addWidget(m_loadMore);
    m_readAll = new QToolButton(footer); m_readAll->setText(tr("读取全部"));
    connect(m_readAll, &QToolButton::clicked, this, &SessionDetailPanel::readAllOrPause); bottom->addWidget(m_readAll);
    auto *latest = new QPushButton(tr("最新"), footer); latest->setObjectName(QStringLiteral("sessionLatest"));
    latest->setIcon(icon(Glyph::Down, c.text)); latest->setMinimumHeight(32); latest->setCursor(Qt::PointingHandCursor);
    connect(latest, &QPushButton::clicked, this, [this] {
        if (!m_eof) { m_jumpToEnd = m_loadAll = true; loadMore(); }
        else m_timeline->scrollToLatest();
    });
    bottom->addWidget(latest); layout->addWidget(footer);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (m_searchBar->isVisible()) m_searchBar->hide(); else emit backRequested();
    });
    applyTheme();
}
// 使用共享配色刷新页头 / 页尾、时间线和目录；图标也需重绘，否则深浅主题切换后仍留旧颜色。
void SessionDetailPanel::applyTheme()
{
    const auto c = colors(QApplication::palette());
    const auto sheet = QStringLiteral(
        "QWidget#sessionDetail { background:%1; color:%2; }"
        "QFrame#sessionHeader { background:%1; border:0; border-bottom:1px solid %3; }"
        "QFrame#sessionFooter { background:%1; border:0; border-top:1px solid %3; }"
        "QFrame#sessionDirectoryPanel { background:%4; border:0; border-left:1px solid %3; }"
        "QFrame#sessionProjectChip { background:%4; border:1px solid %3; border-radius:7px; }"
        "QFrame#sessionSegments { background:%4; border:0; border-radius:9px; }"
        "QLabel { background:transparent; border:0; color:%2; }"
        "QLabel#sessionMetadata, QLabel#sessionStatus { color:%5; }"
        "QLabel#sessionDirectoryCount { background:%6; color:%7; border-radius:8px; padding:2px 7px; }"
        "QListWidget#sessionDirectory { background:transparent; border:0; outline:0; }"
        "QToolButton[sessionSegment=\"true\"] { background:transparent; color:%5; border:1px solid transparent; border-radius:6px; padding:3px 7px; }"
        "QToolButton[sessionSegment=\"true\"]:hover { background:%6; color:%2; }"
        "QToolButton[sessionSegment=\"true\"]:checked { background:%6; color:%2; font-weight:bold; }"
        "QToolButton[sessionSegment=\"true\"]:focus { border-color:%7; }"
        "QToolButton#sessionFailures { background:transparent; color:%8; border:1px solid transparent; padding:3px 6px; border-radius:6px; }"
        "QToolButton#sessionFailures:hover, QToolButton#sessionFailures:checked { background:%6; }"
        "QToolButton#sessionFailures:focus { border-color:%8; }"
        "QPushButton#sessionResume { border:1px solid transparent; border-radius:8px; background:%7; color:white; font-weight:bold; padding:7px 17px; }"
        "QPushButton#sessionResume:hover { border-color:white; }"
        "QPushButton#sessionResume:focus { border-color:%2; }"
        "QPushButton#sessionResume:pressed { background:%5; }"
        "QPushButton#sessionResume:disabled { background:%3; color:%5; }"
        "QPushButton#sessionLatest { background:%4; border:1px solid %3; border-radius:8px; color:%2; padding:4px 12px; }"
        "QPushButton#sessionLatest:hover { background:%6; } QPushButton#sessionLatest:focus { border-color:%7; }")
        .arg(css(c.canvas), css(c.text), css(c.border), css(c.surface), css(c.muted), css(c.accentSoft), css(c.accent), css(c.error));
    if (styleSheet() != sheet) setStyleSheet(sheet);
    for (auto *button : m_header->findChildren<QToolButton *>()) {
        if (button->property("sessionGlyph").isValid()) styleIconButton(button, c);
    }
    if (auto *folder = findChild<QLabel *>(QStringLiteral("sessionProjectIcon"))) folder->setPixmap(icon(Glyph::Folder, c.muted).pixmap(16, 16));
    if (auto *latest = findChild<QPushButton *>(QStringLiteral("sessionLatest"))) latest->setIcon(icon(Glyph::Down, c.text));
    if (m_timeline) m_timeline->applyTheme();
    if (m_directory) m_directory->viewport()->update();
    if (auto *mark = findChild<QLabel *>(QStringLiteral("sessionMark"))) mark->setPixmap(icon(Glyph::Cube, c.accent).pixmap(24, 24));
}
void SessionDetailPanel::changeEvent(QEvent *event)
{
    // 调色板 / 样式切换可能连续触发多次事件，合并到下一轮事件循环，避免重入刷新。
    QWidget::changeEvent(event);
    if (m_status && !m_themePending && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange
        || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) {
        m_themePending = true;
        QTimer::singleShot(0, this, [this] { m_themePending = false; applyTheme(); });
    }
}
void SessionDetailPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event); applyResponsiveLayout(); updateMetadata();
}
void SessionDetailPanel::applyResponsiveLayout()
{
    if (!m_directoryPanel) return;
    const bool visible = m_directoryWanted && (width() >= 940 || m_directoryOverride);
    m_directoryPanel->setVisible(visible);
    const QSignalBlocker blocker(m_directoryToggle); m_directoryToggle->setChecked(visible);
    m_previous->setVisible(width() >= 680); m_next->setVisible(width() >= 680);
}
void SessionDetailPanel::reset()
{
    ++m_request; ++m_exportRequest; m_source = m_backend->sessionSource();
    m_session = {}; m_data = std::make_shared<ClaudeSessionData>(); m_offset = 0;
    m_pending = m_eof = m_loadAll = m_autoFill = m_jumpToEnd = m_searchAfterLoad = false;
    m_loadError.clear(); m_searchIndex = -1;
    m_timeline->clear(); m_directory->clear(); m_directoryCount->setText(QStringLiteral("0"));
    m_title->clear(); m_metadata->clear(); m_project->clear(); m_status->clear(); m_matchLabel->clear();
    m_exportAction->setEnabled(true); m_resume->setEnabled(false); m_failures->hide();
}
void SessionDetailPanel::openSession(const ClaudeSessionInfo &session)
{
    reset(); m_session = session; m_autoFill = true;
    m_title->setText(session.name); m_title->setToolTip(session.name);
    m_search->clear(); m_searchBar->hide(); setFilter(SessionTimelineView::All);
    updateMetadata(); applyResponsiveLayout(); loadMore();
}
void SessionDetailPanel::setNavigationEnabled(bool previous, bool next)
{
    m_previous->setEnabled(previous); m_next->setEnabled(next);
}
void SessionDetailPanel::focusSearch() { m_searchBar->show(); m_search->setFocus(); m_search->selectAll(); }
void SessionDetailPanel::loadMore()
{
    if (m_pending || m_eof || m_session.relativePath.isEmpty()) return;
    m_pending = true; m_loadError.clear(); updateMetadata();
    m_backend->readSessionPage(m_session, m_offset, ++m_request, m_data);
}
void SessionDetailPanel::onPage(quint64 source, quint64 request, const ClaudeSessionPage &page,
                                const ClaudeSessionSnapshot &snapshot)
{
    // 先校验身份再修改 pending / offset，旧请求即使失败也不能干扰当前会话。
    if (source != m_source || request != m_request) return;
    m_pending = false;
    if (!page.error.isEmpty()) {
        m_loadAll = m_autoFill = false; m_loadError = page.error; updateMetadata(); return;
    }
    m_data = snapshot; m_offset = page.nextOffset; m_eof = page.endOfFile;
    if (!m_data->summary().cwd.isEmpty()) m_session.cwd = m_data->summary().cwd;
    if (!m_data->summary().name.isEmpty()) m_session.name = m_data->summary().name;
    m_title->setText(m_session.name); m_title->setToolTip(m_session.name);
    m_rebuildingView = true;
    m_timeline->setSnapshot(m_data, m_eof);
    updateDirectory(); m_rebuildingView = false;
    if (m_eof) {
        m_loadAll = m_autoFill = false;
        if (m_jumpToEnd) { m_jumpToEnd = false; m_timeline->scrollToLatest(); }
        if (m_searchAfterLoad) { m_searchAfterLoad = false; findText(m_searchBackwards); }
    } else if (m_loadAll || (m_autoFill && m_data->entries().size() < 80)) {
        // 续读留出事件循环间隙，避免连续大页阻塞交互；回调执行前再次检查会话身份和暂停态。
        QTimer::singleShot(15, this, [this, source, request] {
            if (m_source == source && m_request == request && (m_loadAll || m_autoFill)) loadMore();
        });
    } else m_autoFill = false;
    updateMetadata();
}
void SessionDetailPanel::updateMetadata()
{
    if (!m_status) return;
    m_project->setText(m_session.cwd.isEmpty() ? tr("项目目录未知") : m_session.cwd);
    m_project->setToolTip(m_session.cwd);
    const auto models = m_data->models().join(QStringLiteral(" / "));
    QString metadata = tr("%1 次提问  ·  %2 次工具调用").arg(m_data->turns().size()).arg(m_data->toolCount());
    if (width() >= 1150 && !models.isEmpty()) metadata += QStringLiteral("  ·  ") + models;
    if (!m_eof) metadata += tr("  ·  已加载部分");
    m_metadata->setText(metadata);
    m_metadata->setToolTip(tr("创建：%1\n最近活跃：%2\n模型：%3\n会话 ID：%4")
        .arg(dateTime(m_data->summary().startedAtMs ? m_data->summary().startedAtMs : m_session.startedAtMs),
             dateTime(m_session.updatedAtMs), models, m_session.sessionId));
    m_failures->setVisible(m_data->failureCount() > 0);
    m_failures->setText(tr("%1 失败").arg(m_data->failureCount()));
    m_resume->setEnabled(!ClaudeSessionData::resumeCommand(m_session).isEmpty());
    m_loadMore->setVisible(!m_eof); m_loadMore->setEnabled(!m_pending);
    m_readAll->setVisible(!m_eof); m_readAll->setText(m_loadAll ? tr("暂停读取") : tr("读取全部"));
    QString status = m_eof ? tr("历史已完整读取  ·  %1 轮对话").arg(m_data->turns().size())
        : tr("%1  %2 / %3 KiB").arg(m_pending ? tr("读取中…") : tr("按需加载"))
            .arg(m_offset / 1024).arg(m_session.fileSize / 1024);
    if (m_data->malformedLines() || (m_eof && m_data->hasIncompleteLine()))
        status += tr("  ·  %1 条损坏/超长记录，%2 条不完整末行，可导出原始日志")
            .arg(m_data->malformedLines()).arg(m_data->hasIncompleteLine() ? 1 : 0);
    m_status->setText(m_loadError.isEmpty() ? status : m_loadError);
}
// 优先复用现有目录项并恢复滚动位置；没有用户提问的前导轮次不加入提问目录。
void SessionDetailPanel::updateDirectory()
{
    const auto &turns = m_timeline->turns();
    const int scroll = m_directory->verticalScrollBar()->value();
    int row = 0;
    for (int i = 0; i < turns.size(); ++i) {
        const auto &turn = turns[i];
        if (turn.userEntry < 0) continue;
        auto *item = row < m_directory->count() ? m_directory->item(row) : new QListWidgetItem(m_directory);
        item->setData(Qt::UserRole, i); item->setData(QuestionRole, turn.question);
        item->setData(TimeRole, timeLabel(turn.startedAtMs, true));
        QString stats = tr("%1 步").arg(turn.steps);
        if (turn.failures) stats += tr("  ·  %1 次失败").arg(turn.failures);
        item->setData(StatsRole, stats); item->setData(FailureRole, turn.failures);
        item->setData(ReplyRole, turn.replyPreview.isEmpty() ? tr("未记录最终回复") : messagePreview(turn.replyPreview));
        item->setData(HasReplyRole, turn.firstReply >= 0); item->setToolTip(turn.question.left(2000));
        if (i == m_timeline->currentTurn()) m_directory->setCurrentItem(item);
        ++row;
    }
    while (m_directory->count() > row) delete m_directory->takeItem(m_directory->count() - 1);
    m_directoryCount->setText(QString::number(row)); m_directory->verticalScrollBar()->setValue(scroll);
}
void SessionDetailPanel::setFilter(int filter)
{
    const QSignalBlocker failures(m_failures);
    m_failures->setChecked(filter == SessionTimelineView::Failures);
    m_filterGroup->setExclusive(false);
    for (auto *button : m_filterGroup->buttons()) button->setChecked(m_filterGroup->id(button) == filter);
    m_filterGroup->setExclusive(true);
    m_timeline->setFilter(static_cast<SessionTimelineView::Filter>(filter));
}
void SessionDetailPanel::findText(bool backwards)
{
    // 未读完时先补齐文件快照，再按条目循环搜索；否则可能把后续页的匹配误报为不存在。
    const auto query = m_search->text().trimmed(); if (query.isEmpty()) return;
    if (!m_eof) {
        m_searchAfterLoad = m_loadAll = true; m_searchBackwards = backwards;
        m_matchLabel->setText(tr("读取完整会话后搜索…")); loadMore(); return;
    }
    QList<int> matches;
    for (int i = 0; i < m_data->entries().size(); ++i)
        if (m_data->entries()[i].searchText().contains(query, Qt::CaseInsensitive)) matches.append(i);
    if (matches.isEmpty()) { m_matchLabel->setText(tr("没有匹配")); return; }
    int position = matches.indexOf(m_searchIndex);
    position = position < 0 ? (backwards ? matches.size() - 1 : 0)
                           : (position + (backwards ? -1 : 1) + matches.size()) % matches.size();
    m_searchIndex = matches[position]; setFilter(SessionTimelineView::All);
    m_timeline->scrollToEntry(m_searchIndex, query);
    m_matchLabel->setText(tr("%1 / %2 处").arg(position + 1).arg(matches.size()));
}
void SessionDetailPanel::readAllOrPause()
{
    m_loadAll = !m_loadAll; m_autoFill = false;
    if (!m_loadAll) m_jumpToEnd = m_searchAfterLoad = false;
    if (m_loadAll) loadMore();
    updateMetadata();
}
void SessionDetailPanel::exportFile()
{
    QString selected = tr("Markdown 文档 (*.md)");
    const auto path = QFileDialog::getSaveFileName(this, tr("导出完整会话（当前文件快照）"),
        m_session.sessionId + QStringLiteral(".md"), selected + QStringLiteral(";;") + tr("原始 JSONL (*.jsonl)"), &selected);
    if (path.isEmpty()) return;
    m_exportAction->setEnabled(false); m_status->setText(tr("正在后台导出完整会话…"));
    m_backend->exportSession(m_session, path, !selected.contains(QStringLiteral("*.jsonl")), ++m_exportRequest);
}
} // namespace cubeshell
