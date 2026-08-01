// SessionPanel.cpp — see SessionPanel.h for the port map.
// 对应Python: core/claude_code/session_widget.py

#include "claude_code/SessionPanel.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cubeshell {

SessionPanel::SessionPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent)
    , m_backend(backend)
{
    buildUi();
    // backend 信号来自工作线程，必须 QueuedConnection 切回 UI 线程
    connect(m_backend, &ClaudeCodeBackend::sessionsLoaded,
            this, &SessionPanel::onSessionsLoaded, Qt::QueuedConnection);
}

// 对应Python: SessionWidget._init_ui（行 48-105）
void SessionPanel::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ─── 顶部工具栏 ───
    auto *toolbarLayout = new QHBoxLayout;
    toolbarLayout->setSpacing(6);

    m_btnRefresh = new QPushButton(tr("刷新"), this);
    connect(m_btnRefresh, &QPushButton::clicked, this, &SessionPanel::refresh);
    toolbarLayout->addWidget(m_btnRefresh);

    // 当前 claude CLI 无 agents stop/remove 与 --bg 子命令，
    // 故不提供停止/删除/新建后台任务按钮，仅支持列表查看与双击恢复。
    auto *hint = new QLabel(tr("双击会话可在终端中恢复，双击项目可展开/折叠"), this);
    hint->setStyleSheet(QStringLiteral("color: #888888;"));
    toolbarLayout->addWidget(hint);

    toolbarLayout->addStretch();
    mainLayout->addLayout(toolbarLayout);

    // ─── 会话树（按项目分组） ───
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("名称"), tr("ID"), tr("状态"), tr("创建时间")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);

    // 列宽策略：名称列自适应拉伸，其余列固定/紧凑
    QHeaderView *header = m_tree->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    m_tree->setColumnWidth(3, 160);

    // 双击会话恢复；双击项目分组则展开/折叠
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &SessionPanel::onItemDoubleClicked);

    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget::item { padding: 4px 6px; }"));

    mainLayout->addWidget(m_tree);

    // ─── 底部状态区 ───
    m_statusLabel = new QLabel(QString(), this);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: #888888; padding: 4px;"));
    mainLayout->addWidget(m_statusLabel);
}

// 对应Python: SessionWidget.refresh（行 111-123）
void SessionPanel::refresh()
{
    if (!m_backend) {
        setStatusText(tr("未连接 Backend"));
        return;
    }
    if (m_pending)   // 避免重复启动
        return;
    m_pending = true;
    setStatusText(tr("正在加载会话列表..."));
    m_btnRefresh->setEnabled(false);
    m_backend->refreshSessions();
}

// 对应Python: _on_item_double_clicked（行 125-131）
void SessionPanel::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    const QVariant data = item->data(0, Qt::UserRole);
    if (!data.isValid()) {
        // 项目分组节点：切换展开/折叠
        item->setExpanded(!item->isExpanded());
        return;
    }
    const int index = data.toInt();
    if (index >= 0 && index < m_sessions.size())
        resumeSession(m_sessions.at(index));
}

// 对应Python: _resume_session（行 133-145）
void SessionPanel::resumeSession(const ClaudeSessionInfo &session)
{
    const QString fullId = session.sessionId;
    if (fullId.isEmpty())
        return;
    // claude 的会话是按工作目录（project）划分的，--resume 只能在该会话
    // 所属的 cwd 下找到对应记录，否则报 "No conversation found"。
    // 因此优先 cd 到会话的 cwd 再执行 resume。
    const QString cmd = ClaudeCodeBackend::buildCdCommand(
        session.cwd, QStringLiteral("claude --resume %1").arg(fullId));
    emit openTerminalRequested(cmd);
}

// 对应Python: _on_sessions_loaded（行 159-168）
void SessionPanel::onSessionsLoaded(
    const QList<cubeshell::ClaudeSessionInfo> &sessions)
{
    m_pending = false;
    m_sessions = sessions;
    renderTree();
    m_btnRefresh->setEnabled(true);
    QSet<QString> projects;
    for (const ClaudeSessionInfo &s : sessions)
        projects.insert(s.cwd);
    setStatusText(tr("共 %1 个会话，%2 个项目")
                      .arg(sessions.size())
                      .arg(projects.size()));
}

// 对应Python: _render_tree（行 181-202）
void SessionPanel::renderTree()
{
    // 记录刷新前展开的项目，刷新后保持其展开状态
    const QSet<QString> previouslyExpanded = expandedProjects();
    const bool firstLoad =
        previouslyExpanded.isEmpty() && m_tree->topLevelItemCount() == 0;

    m_tree->clear();

    // 按 cwd 分组，保持会话原有顺序（已按创建时间降序）
    QStringList groupOrder;                     // 保持 Python dict 的插入序
    QMap<QString, QList<int>> groups;           // cwd -> m_sessions 索引列表
    for (int i = 0; i < m_sessions.size(); ++i) {
        const QString cwd = m_sessions.at(i).cwd;
        if (!groups.contains(cwd))
            groupOrder.append(cwd);
        groups[cwd].append(i);
    }

    for (const QString &cwd : groupOrder) {
        const QList<int> &indexes = groups.value(cwd);
        QList<ClaudeSessionInfo> items;
        items.reserve(indexes.size());
        for (int idx : indexes)
            items.append(m_sessions.at(idx));

        QTreeWidgetItem *groupItem = makeGroupItem(cwd, items);
        m_tree->addTopLevelItem(groupItem);
        for (int idx : indexes)
            groupItem->addChild(makeSessionItem(m_sessions.at(idx), idx));
        // 默认全部展开；刷新后保留用户之前的展开/折叠状态
        if (firstLoad || previouslyExpanded.contains(cwd))
            groupItem->setExpanded(true);
    }
}

// 对应Python: _make_group_item（行 204-221）
QTreeWidgetItem *SessionPanel::makeGroupItem(
    const QString &cwd, const QList<ClaudeSessionInfo> &sessions)
{
    const QString display = cwd.isEmpty() ? tr("（未知项目）") : cwd;
    QString basename = display;
    if (!cwd.isEmpty()) {
        QString trimmed = cwd;
        while (trimmed.endsWith(QLatin1Char('/')) && trimmed.size() > 1)
            trimmed.chop(1);
        const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
        basename = (slash >= 0) ? trimmed.mid(slash + 1) : trimmed;
    }
    int running = 0;
    for (const ClaudeSessionInfo &s : sessions) {
        if (isRunningStatus(s.status))
            ++running;
    }
    QString label = QStringLiteral("📁 %1  ·  %2")
                        .arg(basename)
                        .arg(sessions.size());
    if (running > 0)
        label += tr("（%1 运行中）").arg(running);

    auto *item = new QTreeWidgetItem(
        QStringList{label, QString(), QString(), QString()});
    item->setToolTip(0, display);
    // 标记为分组（无会话数据）
    item->setData(0, Qt::UserRole + 1, cwd);
    QFont font;
    font.setBold(true);
    item->setFont(0, font);
    item->setForeground(0, QBrush(QColor(QStringLiteral("#dcb67a"))));
    return item;
}

// 对应Python: _make_session_item（行 223-239）
QTreeWidgetItem *SessionPanel::makeSessionItem(const ClaudeSessionInfo &session,
                                               int index)
{
    const QString fullId = session.sessionId;
    const QString shortId = fullId.size() > 8 ? fullId.left(8) : fullId;
    const QString name = session.name.isEmpty() ? tr("(无标题)") : session.name;
    const QString status =
        session.status.isEmpty() ? QStringLiteral("unknown") : session.status;
    // 对应Python: _format_started_at（startedAt 毫秒时间戳）
    QString created;
    if (session.startedAtMs > 0) {
        created = QDateTime::fromMSecsSinceEpoch(session.startedAtMs)
                      .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    }

    auto *item = new QTreeWidgetItem(
        QStringList{name, shortId, status, created});
    item->setToolTip(0, name);
    item->setToolTip(1, fullId);
    if (isRunningStatus(status))
        item->setForeground(2, QBrush(QColor(QStringLiteral("#4caf50"))));
    else
        item->setForeground(2, QBrush(QColor(QStringLiteral("#888888"))));
    // 携带 m_sessions 索引供恢复使用（对应Python: 直接存 session dict）
    item->setData(0, Qt::UserRole, index);
    return item;
}

// 对应Python: _expanded_projects（行 241-248）
QSet<QString> SessionPanel::expandedProjects() const
{
    QSet<QString> expanded;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = m_tree->topLevelItem(i);
        if (top->isExpanded())
            expanded.insert(top->data(0, Qt::UserRole + 1).toString());
    }
    return expanded;
}

// 对应Python: 状态集合 ("running", "busy", "idle", "active")
bool SessionPanel::isRunningStatus(const QString &status)
{
    const QString s = status.toLower();
    return s == QLatin1String("running") || s == QLatin1String("busy")
        || s == QLatin1String("idle") || s == QLatin1String("active");
}

// 对应Python: _set_status
void SessionPanel::setStatusText(const QString &text)
{
    m_statusLabel->setText(text);
}

} // namespace cubeshell
