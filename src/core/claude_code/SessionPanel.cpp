// SessionPanel.cpp — 会话列表的搜索 / 分组 / 紧凑双行绘制及详情导航。
// 历史数据由后端异步加载；列表重建时保持分组展开状态与文件身份。

#include "claude_code/SessionPanel.h"
#include "claude_code/SessionDetailPanel.h"
#include "claude_code/SessionWidgets.h"

#include <QAction>
#include <QEvent>
#include <QStyleOptionComboBox>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cubeshell {
namespace {
constexpr int detailRole = Qt::UserRole + 2;
constexpr int groupRole = Qt::UserRole + 1;
class GroupingCombo : public QComboBox {
public:
    explicit GroupingCombo(QWidget *parent) : QComboBox(parent)
    {
        setSizeAdjustPolicy(QComboBox::AdjustToContents);
        setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    }
    void updateMetrics()
    {
        ensurePolished();
        const auto fm = fontMetrics();
        int textWidth = 0;
        for (int i = 0; i < count(); ++i)
            textWidth = qMax(textWidth, fm.horizontalAdvance(itemText(i)));
        textWidth += fm.horizontalAdvance(QLatin1Char('M'));
        QStyleOptionComboBox option;
        initStyleOption(&option);
        QSize size = style()->sizeFromContents(QStyle::CT_ComboBox, &option,
                                               QSize(textWidth, fm.height()), this);
        size.setHeight(qMax(36, size.height()));
        // QSS 内边距与下拉箭头都会挤占文本区，按样式实际返回的子控件矩形补足空间。
        option.rect = QRect(QPoint(), size);
        const auto field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        size.rwidth() += qMax(0, textWidth - field.width());
        size.rheight() += qMax(0, fm.height() - field.height());
        setMinimumSize(size);
        view()->setMinimumWidth(size.width());
    }
protected:
    void changeEvent(QEvent *event) override
    {
        QComboBox::changeEvent(event);
        if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) && !m_pending) {
            m_pending = true;
            QTimer::singleShot(0, this, [this] { m_pending = false; updateMetrics(); });
        }
    }
private:
    bool m_pending = false;
};
// 项目标题 / 路径与会话标题 / 摘要均使用双行布局，紧凑行高仍预留两行字体的完整高度。
class SessionRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return {200, qMax(52, option.fontMetrics.lineSpacing() * 2 + 18)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        using namespace sessionui;
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const auto c = colors(QApplication::palette());
        const bool group = !index.siblingAtColumn(0).data(Qt::UserRole).isValid();
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        painter->save();
        painter->setClipRect(opt.rect);
        painter->fillRect(opt.rect, selected ? c.accentSoft : hovered ? c.raised : group ? c.surface : c.canvas);
        QColor divider = c.border; divider.setAlpha(100);
        painter->setPen(divider);
        painter->drawLine(opt.rect.bottomLeft(), opt.rect.bottomRight());
        if (selected && index.column() == 0)
            painter->fillRect(QRect(opt.rect.left(), opt.rect.top() + 9, 3, opt.rect.height() - 18), c.accent);
        if (opt.state & QStyle::State_HasFocus) {
            painter->setPen(QPen(c.accent, 1, Qt::DotLine));
            painter->drawRect(opt.rect.adjusted(2, 2, -3, -3));
        }
        QRect rect = opt.rect.adjusted(14, 7, -16, -7);
        auto titleFont = opt.font; titleFont.setWeight(QFont::DemiBold);
        auto bodyFont = opt.font; bodyFont.setBold(false);
        if (index.column() == 0) {
            if (group) {
                icon(Glyph::Folder, c.muted).paint(painter, QRect(rect.left(), rect.top() + 1, 17, 17));
                rect.adjust(26, 0, 0, 0);
            }
            const int line = QFontMetrics(titleFont).lineSpacing();
            painter->setFont(titleFont); painter->setPen(c.text);
            painter->drawText(QRect(rect.left(), rect.top(), rect.width(), line), Qt::AlignVCenter,
                              QFontMetrics(titleFont).elidedText(index.data().toString(), Qt::ElideRight, rect.width()));
            painter->setFont(bodyFont); painter->setPen(c.muted);
            painter->drawText(QRect(rect.left(), rect.top() + line + 3, rect.width(), QFontMetrics(bodyFont).lineSpacing()), Qt::AlignVCenter,
                              QFontMetrics(bodyFont).elidedText(index.data(detailRole).toString(), group ? Qt::ElideMiddle : Qt::ElideRight, rect.width()));
        } else {
            painter->setFont(bodyFont); painter->setPen(c.muted);
            painter->drawText(rect, Qt::AlignRight | Qt::AlignVCenter,
                              QFontMetrics(bodyFont).elidedText(index.data().toString(), Qt::ElideRight, rect.width()));
        }
        painter->restore();
    }
};
QString absoluteTime(qint64 ts)
{
    return ts > 0 ? QDateTime::fromMSecsSinceEpoch(ts).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                  : QStringLiteral("未知");
}
}

SessionPanel::SessionPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_source(backend->sessionSource())
{
    buildUi();
    connect(backend, &ClaudeCodeBackend::sessionListReady, this, &SessionPanel::onSessionsLoaded, Qt::QueuedConnection);
    connect(backend, &ClaudeCodeBackend::sessionSourceChanged, this, &SessionPanel::sourceChanged);
}
void SessionPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack);
    m_listPage = new QWidget(m_stack);
    m_listPage->setObjectName(QStringLiteral("sessionListPage"));
    auto *listLayout = new QVBoxLayout(m_listPage);
    listLayout->setContentsMargins(12, 10, 12, 8);
    listLayout->setSpacing(12);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(10);
    auto *title = new QLabel(tr("会话记录"), m_listPage);
    title->setObjectName(QStringLiteral("sessionListTitle"));
    auto titleFont = title->font(); titleFont.setWeight(QFont::DemiBold); title->setFont(titleFont);
    heading->addWidget(title);
    m_count = new QLabel(tr("0 个会话"), m_listPage);
    m_count->setObjectName(QStringLiteral("sessionListCount"));
    heading->addWidget(m_count); heading->addStretch();
    listLayout->addLayout(heading);
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_search = new QLineEdit(m_listPage);
    m_search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_search->setObjectName(QStringLiteral("sessionSearch"));
    m_search->setPlaceholderText(tr("搜索标题、目录、首条消息或会话 ID"));
    m_search->setClearButtonEnabled(true);
    m_searchIcon = m_search->addAction(QIcon(), QLineEdit::LeadingPosition);
    m_searchIcon->setToolTip(tr("搜索会话"));
    connect(m_searchIcon, &QAction::triggered, m_search, [this] { m_search->setFocus(); });
    toolbar->addWidget(m_search, 1);
    // 搜索去抖，避免每输入一个字符就立即清空并重建整棵树。
    auto *debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this, &SessionPanel::renderTree);
    m_grouping = new GroupingCombo(m_listPage);
    m_grouping->setObjectName(QStringLiteral("sessionGrouping"));
    m_grouping->addItems({tr("按项目"), tr("按时间")});
    m_grouping->setToolTip(tr("会话分组方式"));
    m_grouping->setAccessibleName(tr("会话分组方式"));
    m_grouping->setCursor(Qt::PointingHandCursor);
    connect(m_grouping, &QComboBox::currentIndexChanged, this, &SessionPanel::renderTree);
    toolbar->addWidget(m_grouping);
    m_btnRefresh = new QPushButton(tr("刷新"), m_listPage);
    m_btnRefresh->setObjectName(QStringLiteral("sessionListRefresh"));
    m_btnRefresh->setCursor(Qt::PointingHandCursor);
    connect(m_btnRefresh, &QPushButton::clicked, this, &SessionPanel::refresh);
    toolbar->addWidget(m_btnRefresh);
    auto *resume = m_resume = new QPushButton(tr("恢复所选"), m_listPage);
    resume->setObjectName(QStringLiteral("sessionListResume"));
    resume->setCursor(Qt::PointingHandCursor);
    resume->setEnabled(false);
    resume->setToolTip(tr("Ctrl+Enter：在所属本地/远程终端恢复所选会话"));
    toolbar->addWidget(resume);
    connect(resume, &QPushButton::clicked, this, [this] {
        const int index = selectedIndex();
        if (index >= 0)
            resumeSession(m_sessions[index]);
    });
    listLayout->addLayout(toolbar);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("sessionTree"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 135);
    m_tree->setIndentation(22);
    m_tree->setUniformRowHeights(true);
    m_tree->setMouseTracking(true);
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setItemDelegate(new SessionRowDelegate(m_tree));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SessionPanel::showContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) { openItem(item); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) { openItem(item); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this, resume] { resume->setEnabled(selectedIndex() >= 0); });
    listLayout->addWidget(m_tree, 1);
    m_statusLabel = new QLabel(tr("单击会话查看完整过程 · Ctrl+F 搜索 · Ctrl+Enter 恢复"), this);
    m_statusLabel->setObjectName(QStringLiteral("sessionListStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    listLayout->addWidget(m_statusLabel);
    m_stack->addWidget(m_listPage);
    m_detail = new SessionDetailPanel(m_backend, m_stack);
    m_stack->addWidget(m_detail);
    connect(m_detail, &SessionDetailPanel::backRequested, this, [this] { m_stack->setCurrentWidget(m_listPage); m_tree->setFocus(); });
    connect(m_detail, &SessionDetailPanel::resumeRequested, this, &SessionPanel::resumeSession);
    connect(m_detail, &SessionDetailPanel::navigateRequested, this, [this](int delta) {
        const auto filtered = filteredSessions();
        const int position = filtered.indexOf(m_detailIndex) + delta;
        if (position >= 0 && position < filtered.size())
            openSession(filtered[position]);
    });
    connect(m_detail, &SessionDetailPanel::refreshRequested, this, [this] {
        if (m_detailIndex >= 0)
            m_reloadDetail = m_sessions[m_detailIndex].relativePath;
        refresh();
    });
    auto *search = new QShortcut(QKeySequence::Find, this);
    search->setContext(Qt::WidgetWithChildrenShortcut);
    connect(search, &QShortcut::activated, this, [this] {
        if (m_stack->currentWidget() == m_detail)
            m_detail->focusSearch();
        else { m_search->setFocus(); m_search->selectAll(); }
    });
    auto *restore = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    restore->setContext(Qt::WidgetWithChildrenShortcut);
    connect(restore, &QShortcut::activated, this, [this] {
        const int index = m_stack->currentWidget() == m_detail ? m_detailIndex : selectedIndex();
        if (index >= 0)
            resumeSession(m_sessions[index]);
    });
    applyTheme();
    updateMetrics();
}
// 主题变化时重新取应用配色；紧凑行高由字体计算，不使用固定像素高度裁切大字体。
void SessionPanel::applyTheme()
{
    using namespace sessionui;
    const auto c = colors(QApplication::palette());
    const auto sheet = QStringLiteral(
        "QWidget#sessionListPage { background:%1; }"
        "QLabel#sessionListTitle { color:%2; background:transparent; }"
        "QLabel#sessionListCount { color:%3; background:%4; border-radius:8px; padding:3px 8px; }"
        "QLabel#sessionListStatus { color:%3; background:transparent; }"
        "QTreeWidget#sessionTree { background:%1; color:%2; border:1px solid %5; border-radius:8px; }"
        "QLineEdit#sessionSearch, QComboBox#sessionGrouping { background:%4; color:%2; border:1px solid %5; border-radius:7px; }"
        "QLineEdit#sessionSearch { padding:5px 8px; }"
        "QComboBox#sessionGrouping { padding:5px 32px 5px 12px; }"
        "QComboBox#sessionGrouping::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:26px; margin:0; border:0; }"
        "QComboBox#sessionGrouping QAbstractItemView { background:%4; color:%2; padding:4px; selection-background-color:%6; selection-color:%2; }"
        "QComboBox#sessionGrouping QAbstractItemView::item { padding:6px 10px; min-height:1.5em; }"
        "QComboBox#sessionGrouping:hover { border-color:%3; }"
        "QLineEdit#sessionSearch:focus, QComboBox#sessionGrouping:focus { border-color:%7; }"
        "QPushButton { background:%4; color:%2; border:1px solid %5; border-radius:7px; padding:5px 12px; }"
        "QPushButton:hover { background:%8; border-color:%3; }"
        "QPushButton:pressed { background:%6; } QPushButton:focus { border-color:%7; }"
        "QPushButton:disabled { color:%3; border-color:%5; background:%1; }")
        .arg(css(c.canvas), css(c.text), css(c.muted), css(c.surface), css(c.border), css(c.accentSoft), css(c.accent), css(c.raised));
    if (m_listPage->styleSheet() != sheet) m_listPage->setStyleSheet(sheet);
    m_searchIcon->setIcon(icon(Glyph::Search, c.muted));
    m_btnRefresh->setIcon(icon(Glyph::Refresh, c.muted));
    m_resume->setIcon(icon(Glyph::Play, c.accent));
    m_tree->viewport()->update();
}
void SessionPanel::updateMetrics()
{
    const int height = qMax(36, fontMetrics().height() + 16);
    const QList<QWidget *> controls{m_search, m_btnRefresh, m_resume};
    for (auto *widget : controls) {
        widget->setMinimumHeight(height);
        widget->setSizePolicy(widget->sizePolicy().horizontalPolicy(), QSizePolicy::Minimum);
    }
    static_cast<GroupingCombo *>(m_grouping)->updateMetrics();
    m_tree->setColumnWidth(1, qMax(120, m_tree->fontMetrics().horizontalAdvance(tr("9999 个会话")) + 32));
}
void SessionPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (m_tree && !m_stylePending && (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange)) {
        m_stylePending = true;
        QTimer::singleShot(0, this, [this] { m_stylePending = false; applyTheme(); updateMetrics(); });
    }
}
int SessionPanel::selectedIndex() const
{
    const auto *item = m_tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return -1;
    const int index = item->data(0, Qt::UserRole).toInt();
    return index >= 0 && index < m_sessions.size() ? index : -1;
}
QList<int> SessionPanel::filteredSessions() const
{
    QList<int> result;
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (ClaudeSessionData::matches(m_sessions[i], m_search->text()))
            result.append(i);
    }
    return result;
}
void SessionPanel::refresh()
{
    if (m_pending)
        return;
    m_pending = true;
    m_statusLabel->setText(tr("正在读取会话列表…"));
    m_btnRefresh->setEnabled(false);
    m_backend->refreshSessions(++m_request);
}
void SessionPanel::sourceChanged()
{
    m_source = m_backend->sessionSource();
    ++m_request;
    m_pending = false;
    m_detailIndex = -1;
    m_reloadDetail.clear();
    m_sessions.clear();
    m_error.clear();
    m_expandedGroups.clear();
    m_tree->clear();
    m_stack->setCurrentWidget(m_listPage);
    m_btnRefresh->setEnabled(true);
    m_count->setText(tr("0 个会话"));
    m_statusLabel->setText(tr("连接已切换，等待刷新"));
}
void SessionPanel::onSessionsLoaded(quint64 source, quint64 request, const QList<ClaudeSessionInfo> &sessions, const QString &error)
{
    if (source != m_source || request != m_request)
        return;
    m_pending = false;
    m_btnRefresh->setEnabled(true);
    // 刷新可能改变排序或删除记录；用相对文件路径恢复选中项，不能沿用旧行号。
    const auto current = m_tree->currentItem();
    const auto currentPath = current ? current->data(0, Qt::UserRole + 3).toString() : QString();
    const QString detailPath = m_detailIndex >= 0 && m_detailIndex < m_sessions.size()
        ? m_sessions[m_detailIndex].relativePath : QString();
    m_sessions = sessions;
    m_error = error;
    m_detailIndex = -1;
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].relativePath == detailPath)
            m_detailIndex = i;
    }
    if (m_detailIndex < 0 && m_stack->currentWidget() == m_detail) {
        m_detail->reset();
        m_stack->setCurrentWidget(m_listPage);
    }
    renderTree();
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole + 3).toString() == currentPath && !currentPath.isEmpty())
            m_tree->setCurrentItem(*it);
    }
    if (!m_reloadDetail.isEmpty()) {
        const auto path = m_reloadDetail;
        m_reloadDetail.clear();
        for (int i = 0; i < m_sessions.size(); ++i) {
            if (m_sessions[i].relativePath == path) {
                openSession(i);
                break;
            }
        }
    }
}
void SessionPanel::renderTree()
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const auto *item = m_tree->topLevelItem(i);
        m_expandedGroups.insert(item->data(0, groupRole).toString(), item->isExpanded());
    }
    const int scroll = m_tree->verticalScrollBar()->value();
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    const auto filtered = filteredSessions();
    const QDate today = QDate::currentDate();
    for (int index : filtered) {
        const auto &s = m_sessions[index];
        QString group = s.cwd;
        QString groupTitle = ClaudeSessionData::projectName(s.cwd);
        if (m_grouping->currentIndex() == 1) {
            const int days = QDateTime::fromMSecsSinceEpoch(s.updatedAtMs).toLocalTime().date().daysTo(today);
            groupTitle = days <= 0 ? tr("今天") : days == 1 ? tr("昨天") : days < 7 ? tr("最近 7 天")
                         : days < 30 ? tr("最近 30 天") : tr("更早");
            group = QStringLiteral("time:") + groupTitle;
        } else {
            group = QStringLiteral("project:") + group;
        }
        auto *parent = groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree);
            parent->setData(0, groupRole, group);
            parent->setText(0, groupTitle);
            parent->setData(0, detailRole, m_grouping->currentIndex() == 0 ? s.cwd : tr("按最近活跃时间排序"));
            parent->setToolTip(0, m_grouping->currentIndex() == 0 ? s.cwd : groupTitle);
            // 仅首次出现的项目默认收起；已操作过的分组沿用用户选择，时间分组仍默认展开。
            parent->setExpanded(m_expandedGroups.value(group, m_grouping->currentIndex() != 0));
            groups.insert(group, parent);
        }
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, s.name.isEmpty() ? tr("无标题会话") : s.name);
        item->setText(1, ClaudeSessionData::relativeTime(s.updatedAtMs));
        item->setData(0, detailRole, s.lastMessage.isEmpty() ? s.cwd : sessionui::messagePreview(s.lastMessage));
        item->setData(0, Qt::UserRole, index);
        item->setData(0, Qt::UserRole + 3, s.relativePath);
        item->setToolTip(0, tr("%1\n%2\nID：%3\n创建：%4\n最近活跃：%5%6")
            .arg(s.name, s.cwd, s.sessionId, absoluteTime(s.startedAtMs), absoluteTime(s.updatedAtMs),
                 s.partialSummary ? tr("\n摘要来自日志首尾，详情可读取完整记录。") : QString()));
        item->setToolTip(1, absoluteTime(s.updatedAtMs));
        parent->setText(1, tr("%1 个会话").arg(parent->childCount()));
    }
    m_tree->setUpdatesEnabled(true);
    m_tree->verticalScrollBar()->setValue(scroll);
    m_count->setText(tr("%1 个会话").arg(m_sessions.size()));
    QString status = filtered.isEmpty() ? (m_sessions.isEmpty() ? tr("没有已保存的会话。使用 Claude Code 后刷新即可查看。")
        : tr("没有匹配的会话，请调整搜索条件。")) : tr("显示 %1 / %2 个会话 · 单击查看过程 · Ctrl+F 搜索 · Ctrl+Enter 恢复")
        .arg(filtered.size()).arg(m_sessions.size());
    if (!m_error.isEmpty())
        status += QStringLiteral("\n") + m_error;
    m_statusLabel->setText(status);
}
void SessionPanel::openItem(QTreeWidgetItem *item)
{
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return;
    openSession(item->data(0, Qt::UserRole).toInt());
}
void SessionPanel::openSession(int index)
{
    if (index < 0 || index >= m_sessions.size())
        return;
    m_detailIndex = index;
    const auto filtered = filteredSessions();
    const int position = filtered.indexOf(index);
    m_detail->setNavigationEnabled(position > 0, position >= 0 && position + 1 < filtered.size());
    m_detail->openSession(m_sessions[index]);
    m_stack->setCurrentWidget(m_detail);
}
// Windows 本地终端使用 PowerShell 引号规则；SSH 远端仍按 POSIX shell 生成命令。
QString SessionPanel::resumeCommand(const ClaudeSessionInfo &session) const
{
    bool powershell = false;
#ifdef Q_OS_WIN
    powershell = !m_backend->sessionSourceIsRemote();
#endif
    return ClaudeSessionData::resumeCommand(session, powershell);
}
void SessionPanel::resumeSession(const ClaudeSessionInfo &session)
{
    if (m_source != m_backend->sessionSource())
        return;
    const auto command = resumeCommand(session);
    if (command.isEmpty()) {
        QMessageBox::warning(this, tr("无法恢复"), tr("会话 ID 或项目目录无效。请先打开详情读取完整日志，确认项目目录仍然存在。"));
        return;
    }
    emit openTerminalRequested(command);
}
void SessionPanel::showContextMenu(const QPoint &position)
{
    auto *item = m_tree->itemAt(position);
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    const auto session = m_sessions[index];
    QMenu menu(this);
    auto *view = menu.addAction(tr("查看完整过程"));
    auto *resume = menu.addAction(tr("在终端中恢复"));
    resume->setEnabled(!resumeCommand(session).isEmpty());
    menu.addSeparator();
    auto *copyId = menu.addAction(tr("复制会话 ID"));
    auto *copyCommand = menu.addAction(tr("复制恢复命令"));
    copyCommand->setEnabled(resume->isEnabled());
    auto *copyPath = menu.addAction(tr("复制项目路径"));
    const auto *action = menu.exec(m_tree->viewport()->mapToGlobal(position));
    if (action == view) openSession(index);
    else if (action == resume) resumeSession(session);
    else if (action == copyId) QApplication::clipboard()->setText(session.sessionId);
    else if (action == copyCommand) QApplication::clipboard()->setText(resumeCommand(session));
    else if (action == copyPath) QApplication::clipboard()->setText(session.cwd);
}
} // namespace cubeshell
