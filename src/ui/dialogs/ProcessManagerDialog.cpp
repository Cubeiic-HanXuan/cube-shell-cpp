#include "ProcessManagerDialog.h"
#include "ProcessTableModel.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "ssh/CommandExecutor.h"
#include "widgets/ConversationWidgets.h"

namespace cubeshell {

ProcessManagerDialog::ProcessManagerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("远程进程管理"));
    setMinimumSize(800, 500);
    resize(1240, 720);
    setModal(false);
    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("processSearch"));
    m_search->setPlaceholderText(tr("搜索 PID、用户、端口、路径或命令行…"));
    m_search->setClearButtonEnabled(true);
    searchRow->addWidget(m_search, 2);
    searchRow->addWidget(new sessionui::ElidedLabel(tr("点击表头排序 · 选择行查看完整详情"), this), 1);
    layout->addLayout(searchRow);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, [this]() { m_search->setFocus(); m_search->selectAll(); });
    m_search->setToolTip(tr("按文字匹配所有字段，支持完整命令行和文件路径。%1 定位搜索框。")
                            .arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(120);
    connect(m_search, &QLineEdit::textChanged, this, &ProcessManagerDialog::onFilterChanged);
    connect(m_filterTimer, &QTimer::timeout, this, &ProcessManagerDialog::applyFilter);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_model = new ProcessTableModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(ProcessTableModel::SortRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterRole(ProcessTableModel::SearchRole);
    m_proxy->setFilterKeyColumn(ProcessTableModel::Pid); // 搜索缓存一次，不逐列重复匹配
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_table = new QTableView(splitter);
    m_table->setObjectName(QStringLiteral("processTable"));
    m_table->setModel(m_proxy);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(qMax(26, fontMetrics().height() + 10));
    auto *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setMinimumSectionSize(48);
    header->setStretchLastSection(true);
    const int widths[] = {76, 110, 90, 90, 110, 260, 420};
    for (int column = 0; column < ProcessTableModel::ColumnCount; ++column)
        m_table->setColumnWidth(column, widths[column]);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(ProcessTableModel::Cpu, Qt::DescendingOrder);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, &ProcessManagerDialog::onTableContextMenu);
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, m_table);
    copyShortcut->setContext(Qt::WidgetShortcut); // 详情文本框保留自身的原生文本复制行为
    connect(copyShortcut, &QShortcut::activated, this, &ProcessManagerDialog::copySelectedRows);

    auto *details = new QWidget(splitter);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 4, 0, 0);
    auto *detailsHeader = new QHBoxLayout;
    m_detailsSummary = new sessionui::ElidedLabel(tr("选择一个进程查看详情"), details);
    m_detailsSummary->setObjectName(QStringLiteral("processDetailsSummary"));
    detailsHeader->addWidget(m_detailsSummary, 1);
    m_copyCommandButton = new QToolButton(details);
    m_copyCommandButton->setText(tr("复制命令行"));
    m_copyCommandButton->setObjectName(QStringLiteral("copyProcessCommand"));
    connect(m_copyCommandButton, &QToolButton::clicked, this, &ProcessManagerDialog::copyCommand);
    detailsHeader->addWidget(m_copyCommandButton);
    m_copyPathButton = new QToolButton(details);
    m_copyPathButton->setText(tr("复制路径"));
    m_copyPathButton->setObjectName(QStringLiteral("copyProcessPath"));
    connect(m_copyPathButton, &QToolButton::clicked, this, &ProcessManagerDialog::copyPath);
    detailsHeader->addWidget(m_copyPathButton);
    detailsLayout->addLayout(detailsHeader);
    auto *pathRow = new QHBoxLayout;
    auto *pathLabel = new QLabel(tr("可执行文件"), details);
    m_pathDetails = new QLineEdit(details);
    m_pathDetails->setObjectName(QStringLiteral("processExecutablePath"));
    m_pathDetails->setReadOnly(true);
    m_pathDetails->setPlaceholderText(tr("选择进程后显示实际可执行文件路径"));
    pathLabel->setBuddy(m_pathDetails);
    pathRow->addWidget(pathLabel);
    pathRow->addWidget(m_pathDetails, 1);
    detailsLayout->addLayout(pathRow);
    m_commandDetails = new QPlainTextEdit(details);
    m_commandDetails->setObjectName(QStringLiteral("processCommandDetails"));
    m_commandDetails->setAccessibleName(tr("完整命令行"));
    m_commandDetails->setReadOnly(true);
    m_commandDetails->setPlaceholderText(tr("此处显示完整命令行，可选择文本复制；双击进程行定位到这里。"));
    m_commandDetails->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_commandDetails->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_commandDetails->setMinimumHeight(60);
    detailsLayout->addWidget(m_commandDetails, 1);
    detailsLayout->addWidget(new sessionui::ElidedLabel(
        tr("路径来自 /proc/PID/exe；脚本进程显示解释器。无权限、内核线程或已退出进程可能无法读取。"), details));
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({460, 155});
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout;
    m_status = new sessionui::ElidedLabel(QString(), this);
    m_status->setObjectName(QStringLiteral("processStatus"));
    m_status->setWordWrap(false);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    buttons->addWidget(m_status, 1);
    m_killButton = new QPushButton(tr("终止进程"), this);
    connect(m_killButton, &QPushButton::clicked, this, &ProcessManagerDialog::onKillClicked);
    buttons->addWidget(m_killButton);
    m_refreshButton = new QPushButton(tr("刷新进程列表"), this);
    m_refreshButton->setShortcut(QKeySequence(Qt::Key_F5));
    m_refreshButton->setToolTip(tr("重新获取进程快照（F5），保留筛选、排序和仍存在的选中进程。"));
    connect(m_refreshButton, &QPushButton::clicked, this, &ProcessManagerDialog::refresh);
    buttons->addWidget(m_refreshButton);
    layout->addLayout(buttons);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updateDetails(); updateStatus();
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { updateDetails(); });
    connect(m_table, &QTableView::doubleClicked, this, [this]() { m_commandDetails->setFocus(); });
    markStale(tr("未连接 SSH 服务器"));
}

ProcessManagerDialog::~ProcessManagerDialog() { releaseExecutor(); }

void ProcessManagerDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::FontChange && m_table)
        m_table->verticalHeader()->setDefaultSectionSize(qMax(26, fontMetrics().height() + 10));
}

void ProcessManagerDialog::releaseExecutor()
{
    ++m_generation;
    m_task = Task::None;
    m_taskFailed = false;
    if (m_executor) {
        disconnect(m_executor, nullptr, this, nullptr);
        m_executor->cancel();
        delete m_executor; // 析构 join；共享所有权保持旧 SshClient 活到线程退出
        m_executor = nullptr;
    }
}

void ProcessManagerDialog::setClient(std::shared_ptr<SshClient> client)
{
    if (m_executor && m_executor->client() == client.get()) return;
    if (!m_executor && !client) return;
    releaseExecutor();
    if (!client) {
        markStale(tr("SSH 连接已断开，请重新连接后刷新"));
        return;
    }
    m_filterTimer->stop();
    m_model->replaceSnapshot({});
    ++m_snapshotRevision;
    m_lastUpdated = {};
    m_executor = new CommandExecutor(std::move(client), this);
    m_executor->setObjectName(QStringLiteral("processExecutor"));
    const quint64 generation = m_generation;
    connect(m_executor, &CommandExecutor::streamFinished, this,
            [this, generation](int code, const QString &out, const QString &err) {
                if (generation == m_generation) onStreamFinished(code, out, err);
            }, Qt::QueuedConnection);
    connect(m_executor, &CommandExecutor::streamError, this,
            [this, generation](const QString &message) {
                if (generation == m_generation) onStreamError(message);
            }, Qt::QueuedConnection);
    markStale(tr("尚未获取进程列表，请刷新"));
}

void ProcessManagerDialog::markStale(const QString &message)
{
    m_stale = true;
    m_notice = message;
    updateDetails();
    updateStatus();
}

void ProcessManagerDialog::refresh()
{
    if (m_task != Task::None || (m_executor && m_executor->isStreaming())) return;
    if (!m_executor || !m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    if (!m_executor->execStream(ProcessTableModel::collectionCommand(), false,
                                CommandExecutor::kDefaultTimeoutMs)) {
        markStale(tr("未能启动进程列表刷新，请稍后重试"));
        return;
    }
    m_task = Task::Refresh;
    markStale(tr("正在获取进程列表…"));
}

void ProcessManagerDialog::onKillClicked()
{
    if (!m_executor || !m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    if (m_stale || m_task != Task::None || m_executor->isStreaming()) return;
    const quint64 generation = m_generation;
    const quint64 revision = m_snapshotRevision;
    QStringList pids;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows())
        pids.append(QString::number(index.data(ProcessTableModel::PidRole).toULongLong()));
    if (pids.isEmpty()) return;
    const auto answer = QMessageBox::question(this, tr("确认终止"),
        tr("确认要终止选中的 %1 个进程吗？\nPID: %2").arg(pids.size()).arg(pids.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    // 确认框有嵌套事件循环，期间可能换服务器、刷新快照或改变连接状态。
    if (generation != m_generation || revision != m_snapshotRevision || !m_executor || m_stale
        || m_task != Task::None || m_executor->isStreaming()) return;
    if (!m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    if (!m_executor->execStream(QStringLiteral("kill -15 %1").arg(pids.join(QLatin1Char(' '))))) return;
    m_task = Task::Kill;
    markStale(tr("正在终止进程…"));
}

void ProcessManagerDialog::onFilterChanged(const QString &text)
{
    if (text.isEmpty()) { m_filterTimer->stop(); applyFilter(); }
    else m_filterTimer->start(); // 合并连续键入，不在每个按键上遍历整个列表
}

ProcessManagerDialog::ViewState ProcessManagerDialog::captureViewState() const
{
    ViewState state;
    for (const auto &index : m_table->selectionModel()->selectedRows())
        state.selected.append(index.data(ProcessTableModel::PidRole).toULongLong());
    state.current = m_table->currentIndex().data(ProcessTableModel::PidRole).toULongLong();
    const auto top = m_table->indexAt(QPoint(1, 1));
    state.top = top.data(ProcessTableModel::PidRole).toULongLong();
    state.topOffset = m_table->visualRect(top).top();
    state.vertical = m_table->verticalScrollBar()->value();
    state.horizontal = m_table->horizontalScrollBar()->value();
    return state;
}

void ProcessManagerDialog::restoreViewState(const ViewState &state)
{
    auto indexForPid = [this](quint64 pid) {
        return m_proxy->mapFromSource(m_model->index(m_model->rowForPid(pid), ProcessTableModel::Pid));
    };
    QVector<int> rows;
    rows.reserve(state.selected.size());
    for (quint64 pid : state.selected) {
        const auto index = indexForPid(pid);
        if (index.isValid()) rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end());
    QItemSelection selection;
    // 合并连续行，避免 Ctrl+A 后刷新为每行创建一个独立选择区间。
    for (int i = 0; i < rows.size(); ++i) {
        const int first = rows.at(i);
        int last = first;
        while (i + 1 < rows.size() && rows.at(i + 1) == last + 1) last = rows.at(++i);
        selection.select(m_proxy->index(first, 0), m_proxy->index(last, ProcessTableModel::ColumnCount - 1));
    }
    m_table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_table->selectionModel()->setCurrentIndex(indexForPid(state.current), QItemSelectionModel::NoUpdate);
    const auto top = indexForPid(state.top);
    if (state.vertical > 0 && top.isValid()) {
        m_table->scrollTo(top, QAbstractItemView::PositionAtTop);
        m_table->verticalScrollBar()->setValue(m_table->verticalScrollBar()->value() - state.topOffset);
    } else {
        m_table->verticalScrollBar()->setValue(state.vertical);
    }
    m_table->horizontalScrollBar()->setValue(state.horizontal);
}

void ProcessManagerDialog::applyFilter()
{
    const auto state = captureViewState();
    {
        const QSignalBlocker blocker(m_table->selectionModel());
        m_proxy->setFilterFixedString(m_search->text().trimmed());
        restoreViewState(state);
    }
    updateDetails();
    updateStatus();
}

void ProcessManagerDialog::onStreamFinished(int exitCode, const QString &out, const QString &err)
{
    if (m_taskFailed) {
        m_taskFailed = false;
        m_task = Task::None;
        updateStatus();
        return;
    }
    if (m_task == Task::Refresh) {
        m_task = Task::None;
        if (exitCode != 0) {
            markStale(tr("获取进程列表失败：%1").arg(err.trimmed().isEmpty()
                ? tr("命令退出码 %1").arg(exitCode) : err.trimmed()));
            return;
        }
        auto snapshot = ProcessTableModel::parseSnapshot(out);
        if (!snapshot.ok()) { markStale(tr("获取进程列表失败：%1").arg(snapshot.error)); return; }
        const auto state = captureViewState();
        m_filterTimer->stop();
        m_lastUpdated = QDateTime::currentDateTime();
        ++m_snapshotRevision;
        m_stale = false;
        m_notice.clear();
        {
            const QSignalBlocker blocker(m_table->selectionModel());
            m_table->setUpdatesEnabled(false);
            m_model->replaceSnapshot(std::move(snapshot.processes));
            m_proxy->setFilterFixedString(m_search->text().trimmed());
            restoreViewState(state);
            m_table->setUpdatesEnabled(true);
        }
        updateDetails();
        updateStatus();
    } else if (m_task == Task::Kill) {
        m_task = Task::None;
        if (exitCode != 0) {
            markStale(tr("终止进程失败：%1").arg(err.trimmed().isEmpty()
                ? tr("命令退出码 %1").arg(exitCode) : err.trimmed()));
            return;
        }
        refresh();
    }
}

void ProcessManagerDialog::onStreamError(const QString &message)
{
    if (m_task == Task::None) return;
    m_taskFailed = true; // 等配对的 finished，避免旧完成信号落到下一次任务上
    markStale((m_task == Task::Refresh ? tr("获取进程列表失败：%1") : tr("终止进程失败：%1")).arg(message));
}

const RemoteProcess *ProcessManagerDialog::currentProcess() const
{
    const auto source = m_proxy->mapToSource(m_table->currentIndex());
    return source.isValid() ? m_model->processAt(source.row()) : nullptr;
}

void ProcessManagerDialog::updateDetails()
{
    const auto *process = currentProcess();
    const QString command = process ? process->command : QString();
    const QString path = process ? process->executablePath : QString();
    // CPU/内存刷新不应打断用户在相同命令行中选择文本、滚动阅读。
    if (m_commandDetails->toPlainText() != command) m_commandDetails->setPlainText(command);
    if (m_pathDetails->text() != path) { m_pathDetails->setText(path); m_pathDetails->setCursorPosition(0); }
    m_pathDetails->setPlaceholderText(process ? tr("不可读取（权限不足、内核线程、已退出或远端不支持）")
                                             : tr("选择进程后显示实际可执行文件路径"));
    QString summary = tr("选择一个进程查看详情");
    if (process) {
        summary = tr("PID %1 · 用户 %2 · CPU %3% · 内存 %4%")
            .arg(process->pid).arg(process->user, process->cpuText, process->memoryText);
        if (m_stale) summary = tr("旧数据 · ") + summary;
    }
    m_detailsSummary->setText(summary);
    m_detailsSummary->setToolTip(summary);
    m_copyCommandButton->setEnabled(!command.isEmpty());
    m_copyPathButton->setEnabled(!path.isEmpty());
}

void ProcessManagerDialog::updateStatus()
{
    QString status = tr("共 %1 个进程，显示 %2 个").arg(m_model->rowCount()).arg(m_proxy->rowCount());
    const int selected = m_table->selectionModel()->selectedRows().size();
    if (selected) status += tr(" · 已选 %1 个").arg(selected);
    QString details = status;
    if (m_lastUpdated.isValid()) {
        if (m_stale) status += tr(" · 旧数据");
        else status += tr(" · 更新于 %1").arg(m_lastUpdated.toString(QStringLiteral("HH:mm:ss")));
        details += tr("\n%1上次成功获取：%2")
            .arg(m_stale ? tr("当前列表为旧数据；") : QString(),
                 m_lastUpdated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }
    if (m_model->unavailablePathCount())
        details += tr("\n%1 个进程的可执行文件路径不可读取；不影响进程列表，未自动提权。")
                       .arg(m_model->unavailablePathCount());
    if (!m_notice.isEmpty()) {
        status += QStringLiteral(" · ") + m_notice.simplified();
        details = m_notice + QLatin1Char('\n') + details;
    }
    m_status->setText(status);
    m_status->setToolTip(details);
    const bool connected = m_executor && m_executor->hasUsableConnection();
    const bool idle = m_task == Task::None && (!m_executor || !m_executor->isStreaming());
    m_killButton->setEnabled(connected && idle && !m_stale && selected > 0);
    m_refreshButton->setEnabled(connected && idle);
}

void ProcessManagerDialog::copyCommand()
{
    if (const auto *process = currentProcess()) QApplication::clipboard()->setText(process->command);
}

void ProcessManagerDialog::copyPath()
{
    if (const auto *process = currentProcess(); process && !process->executablePath.isEmpty())
        QApplication::clipboard()->setText(process->executablePath);
}

void ProcessManagerDialog::copySelectedRows()
{
    auto rows = m_table->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    QStringList lines;
    for (const auto &row : rows) {
        QStringList fields;
        for (int column = 0; column < ProcessTableModel::ColumnCount; ++column) {
            QString text = row.siblingAtColumn(column).data(ProcessTableModel::FullTextRole).toString();
            if (text.contains(QLatin1Char('\t')) || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('"')))
                text = QLatin1Char('"') + text.replace(QStringLiteral("\""), QStringLiteral("\"\"")) + QLatin1Char('"');
            fields.append(text);
        }
        lines.append(fields.join(QLatin1Char('\t')));
    }
    if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void ProcessManagerDialog::onTableContextMenu(const QPoint &pos)
{
    const auto clicked = m_table->indexAt(pos);
    if (clicked.isValid()) {
        const auto flags = m_table->selectionModel()->isSelected(clicked)
            ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
        m_table->selectionModel()->setCurrentIndex(clicked, flags);
    }
    QMenu menu(this);
    if (clicked.isValid()) {
        const QString text = clicked.data(ProcessTableModel::FullTextRole).toString();
        auto *copyCell = menu.addAction(tr("复制单元格"), this, [text]() { QApplication::clipboard()->setText(text); });
        copyCell->setEnabled(!text.isEmpty());
        menu.addAction(tr("复制命令行"), this, &ProcessManagerDialog::copyCommand)->setEnabled(m_copyCommandButton->isEnabled());
        menu.addAction(tr("复制可执行文件路径"), this, &ProcessManagerDialog::copyPath)->setEnabled(m_copyPathButton->isEnabled());
        menu.addAction(tr("复制所选行"), this, &ProcessManagerDialog::copySelectedRows);
        menu.addSeparator();
    }
    menu.addAction(tr("刷新进程列表"), this, &ProcessManagerDialog::refresh)->setEnabled(m_refreshButton->isEnabled());
    if (m_killButton->isEnabled())
        menu.addAction(tr("终止进程"), this, &ProcessManagerDialog::onKillClicked);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace cubeshell
