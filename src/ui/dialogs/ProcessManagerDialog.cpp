// ProcessManagerDialog.cpp — see ProcessManagerDialog.h.
// 对应Python: cube-shell.py 进程管理（processInitUI 一族）

#include "ProcessManagerDialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ssh/CommandExecutor.h"
#include "widgets/ConversationWidgets.h"

namespace cubeshell {

namespace {
// 列序对应Python: display_processes 的 PID/用户/内存/CPU/端口/命令行
enum Column { ColPid = 0, ColUser, ColMem, ColCpu, ColPort, ColCommand, ColCount };

// ps 与 ss 批处理输出的分隔行（见 refresh()）。
const QLatin1String kPsSsSeparator("===PS_SS_SEP===");
} // namespace

ProcessManagerDialog::ProcessManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    // 对应Python: _ensure_process_manager_dialog 的窗口参数
    setWindowTitle(tr("远程进程管理"));
    setMinimumSize(800, 500);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索进程..."));
    connect(m_search, &QLineEdit::textChanged, this, &ProcessManagerDialog::onFilterChanged);
    layout->addWidget(m_search);

    // 对应Python: processInitUI 的表格初始化
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("PID"), tr("用户"), tr("内存"),
                                        QStringLiteral("CPU"), tr("端口"), tr("命令行")});
    QHeaderView *header = m_table->horizontalHeader();
    for (int col = ColPid; col < ColCommand; ++col)
        header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColCommand, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 对应Python: processInitUI 的右键菜单
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &ProcessManagerDialog::onTableContextMenu);
    layout->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    // 状态始终占一行；绘制时省略长文本，不让错误长度改变对话框最小尺寸。
    m_status = new sessionui::ElidedLabel(QString(), this);
    m_status->setObjectName(QStringLiteral("processStatus"));
    m_status->setWordWrap(false);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    btnRow->addWidget(m_status, 1);
    m_killButton = new QPushButton(tr("终止进程"), this);
    connect(m_killButton, &QPushButton::clicked, this, &ProcessManagerDialog::onKillClicked);
    btnRow->addWidget(m_killButton);
    auto *refreshBtn = new QPushButton(tr("刷新进程列表"), this);
    connect(refreshBtn, &QPushButton::clicked, this, &ProcessManagerDialog::refresh);
    btnRow->addWidget(refreshBtn);
    layout->addLayout(btnRow);

    markStale(tr("未连接 SSH 服务器"));
}

ProcessManagerDialog::~ProcessManagerDialog()
{
    releaseExecutor();
}

void ProcessManagerDialog::releaseExecutor()
{
    // disconnect 不能撤回已投递的 queued signal；代次检查负责拦截这些迟到回调。
    ++m_generation;
    m_task = Task::None;
    m_taskFailed = false;
    if (m_executor) {
        disconnect(m_executor, nullptr, this, nullptr);
        m_executor->cancel();
        // 析构会 join 工作线程；其共享所有权确保期间旧 SshClient 不会先被释放。
        delete m_executor;
        m_executor = nullptr;
    }
}

void ProcessManagerDialog::setClient(std::shared_ptr<SshClient> client)
{
    if (m_executor && m_executor->client() == client.get())
        return;
    if (!m_executor && !client)
        return;
    releaseExecutor();
    if (!client) {
        markStale(tr("SSH 连接已断开，请重新连接后刷新"));
        return;
    }

    m_all.clear();
    m_table->setRowCount(0);
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
    updateStatus();
}

void ProcessManagerDialog::refresh()
{
    if (!m_executor || !m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    if (m_task != Task::None || m_executor->isStreaming())
        return; // 上一个命令还没结束
    // ps 失败必须传播退出码；ss 不存在或无权查询时仅缺失端口，不影响进程快照。
    if (!m_executor->execStream(
            QStringLiteral("ps aux --no-headers && { echo '%1'; ss -tulnpe 2>/dev/null || :; }")
                .arg(kPsSsSeparator),
            /*pty=*/false)) {
        markStale(tr("未能启动进程列表刷新，请稍后重试"));
        return;
    }
    m_task = Task::Refresh;
    markStale(tr("正在获取进程列表…"));
}

// 对应Python: cube-shell.py::kill_selected_process
void ProcessManagerDialog::onKillClicked()
{
    if (!m_executor || !m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    // 旧 PID 可能已被服务器复用；必须先取得当前连接上的有效快照。
    if (m_stale || m_task != Task::None || m_executor->isStreaming())
        return;
    const quint64 generation = m_generation;
    QStringList pids;
    const auto selected = m_table->selectionModel()
                              ? m_table->selectionModel()->selectedRows(ColPid)
                              : QModelIndexList();
    for (const QModelIndex &index : selected) {
        const QString pid = index.data().toString();
        if (!pid.isEmpty())
            pids << pid;
    }
    if (pids.isEmpty())
        return;

    const auto answer = QMessageBox::question(
        this, tr("确认终止"),
        tr("确认要终止选中的 %1 个进程吗?\nPID: %2")
            .arg(pids.size())
            .arg(pids.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    // 确认框有嵌套事件循环，期间可能断线或切换服务器，必须重新校验。
    if (generation != m_generation || !m_executor || m_stale
        || m_task != Task::None || m_executor->isStreaming())
        return;
    if (!m_executor->hasUsableConnection()) {
        markStale(tr("SSH 连接不可用，请重新连接后刷新"));
        return;
    }
    // kill -15 优雅终止（与 Python 侧一致）。
    if (!m_executor->execStream(QStringLiteral("kill -15 %1").arg(pids.join(QLatin1Char(' ')))))
        return;
    m_task = Task::Kill;
    markStale(tr("正在终止进程…"));
}

void ProcessManagerDialog::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    applyFilter();
}

void ProcessManagerDialog::onStreamFinished(int exitCode, const QString &stdoutText,
                                            const QString &stderrText)
{
    // streamError 与 streamFinished 成对发出；不能在两者之间启动新请求，
    // 否则旧 finished 会被误认为新任务的结果。错误提示由前一个信号保留。
    if (m_taskFailed) {
        m_taskFailed = false;
        m_task = Task::None;
        updateStatus();
        return;
    }
    if (m_task == Task::Refresh) {
        m_task = Task::None;
        if (exitCode != 0) {
            markStale(tr("获取进程列表失败：%1")
                          .arg(stderrText.trimmed().isEmpty()
                                   ? tr("命令退出码 %1").arg(exitCode) : stderrText.trimmed()));
            return;
        }
        // 没有分隔行说明输出不完整，不能把部分输出当作一次成功刷新。
        const QString separator = QString(kPsSsSeparator) + QLatin1Char('\n');
        const int sepIndex = stdoutText.indexOf(separator);
        if (sepIndex < 0 || (sepIndex > 0 && stdoutText.at(sepIndex - 1) != QLatin1Char('\n'))) {
            markStale(tr("获取进程列表失败：返回数据不完整"));
            return;
        }
        const auto pidPorts = parseSsOutput(stdoutText.mid(sepIndex + separator.size()));
        if (!parseProcessList(stdoutText.left(sepIndex), pidPorts)) {
            markStale(tr("获取进程列表失败：无法解析进程数据"));
            return;
        }
        m_lastUpdated = QDateTime::currentDateTime();
        m_stale = false;
        m_notice.clear();
        applyFilter();
    } else if (m_task == Task::Kill) {
        m_task = Task::None;
        if (exitCode != 0) {
            markStale(tr("终止进程失败：%1")
                          .arg(stderrText.trimmed().isEmpty()
                                   ? tr("命令退出码 %1").arg(exitCode) : stderrText.trimmed()));
            return;
        }
        refresh();
    }
    // 其它组件发起的流式执行：不处理。
}

void ProcessManagerDialog::onStreamError(const QString &message)
{
    if (m_task == Task::None)
        return;
    const bool refreshing = m_task == Task::Refresh;
    m_taskFailed = true;
    markStale((refreshing ? tr("获取进程列表失败：%1") : tr("终止进程失败：%1")).arg(message));
}

// 对应Python: get_filtered_process_list 的 ss -tulnpe 输出解析（pid -> 端口列表）
QHash<QString, QStringList> ProcessManagerDialog::parseSsOutput(const QString &ssOutput)
{
    QHash<QString, QStringList> pidPorts;
    static const QRegularExpression pidRe(QStringLiteral("pid=(\\d+)"));
    const QStringList lines = ssOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // 跳过标题行
        if (line.startsWith(QLatin1String("Netid")) || line.startsWith(QLatin1String("State")))
            continue;
        const QStringList fields = line.simplified().split(QLatin1Char(' '));
        if (fields.size() < 5)
            continue;
        // 本地地址:端口，取最后一个冒号后的端口号
        const QString localAddr = fields.at(4);
        const int colonIndex = localAddr.lastIndexOf(QLatin1Char(':'));
        if (colonIndex < 0)
            continue;
        const QString port = localAddr.mid(colonIndex + 1);
        // 格式示例: users:(("sshd",pid=123,fd=3))
        if (!line.contains(QLatin1String("users:")))
            continue;
        auto it = pidRe.globalMatch(line);
        while (it.hasNext()) {
            const QString pid = it.next().captured(1);
            QStringList &ports = pidPorts[pid];
            if (!ports.contains(port))
                ports.append(port);
        }
    }
    return pidPorts;
}

// 对应Python: get_filtered_process_list 的 ps 输出解析
bool ProcessManagerDialog::parseProcessList(const QString &psOutput,
                                            const QHash<QString, QStringList> &pidPorts)
{
    QList<RemoteProcess> processes;
    const QStringList lines = psOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND...
        const QStringList fields = line.simplified().split(QLatin1Char(' '));
        if (fields.size() < 11)
            return false;
        bool validPid = false;
        const uint pid = fields.at(1).toUInt(&validPid);
        if (!validPid || pid == 0)
            return false;
        RemoteProcess p;
        p.user = fields.at(0);
        p.pid = QString::number(pid);
        p.cpu = fields.at(2);
        p.memory = fields.at(3);
        p.port = pidPorts.value(p.pid).join(QLatin1Char(','));
        p.command = QStringList(fields.mid(10)).join(QLatin1Char(' '));
        processes.append(p);
    }
    m_all = std::move(processes);
    return true;
}

// 对应Python: apply_filter + display_processes
void ProcessManagerDialog::applyFilter()
{
    const QString needle = m_search->text().trimmed();
    m_table->setRowCount(0);
    for (const RemoteProcess &p : m_all) {
        if (!needle.isEmpty()
            && !p.pid.contains(needle, Qt::CaseInsensitive)
            && !p.user.contains(needle, Qt::CaseInsensitive)
            && !p.cpu.contains(needle, Qt::CaseInsensitive)
            && !p.memory.contains(needle, Qt::CaseInsensitive)
            && !p.port.contains(needle, Qt::CaseInsensitive)
            && !p.command.contains(needle, Qt::CaseInsensitive))
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, ColPid, new QTableWidgetItem(p.pid));
        m_table->setItem(row, ColUser, new QTableWidgetItem(p.user));
        m_table->setItem(row, ColMem, new QTableWidgetItem(p.memory));
        m_table->setItem(row, ColCpu, new QTableWidgetItem(p.cpu));
        m_table->setItem(row, ColPort, new QTableWidgetItem(p.port));
        m_table->setItem(row, ColCommand, new QTableWidgetItem(p.command));
    }
    updateStatus();
}

void ProcessManagerDialog::updateStatus()
{
    QString status = tr("共 %1 个进程，显示 %2 个").arg(m_all.size()).arg(m_table->rowCount());
    QString details = status;
    if (m_stale && m_lastUpdated.isValid()) {
        // 把旧数据标记放在长错误之前，窄窗口省略尾部时也能看到当前快照已过期。
        status += tr(" · 旧数据");
        details += tr(" · 当前列表为旧数据（上次成功获取：%1）")
                       .arg(m_lastUpdated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }
    if (!m_notice.isEmpty()) {
        // 远端错误本身也可能含换行；单行展示，完整格式和时间信息保留在 tooltip。
        status += QStringLiteral(" · ") + m_notice.simplified();
        details = m_notice + QLatin1Char('\n') + details;
    }
    m_status->setText(status);
    m_status->setToolTip(details);
    m_killButton->setEnabled(m_executor && !m_stale && m_task == Task::None
                             && m_executor->hasUsableConnection());
}

// 对应Python: cube-shell.py::showContextMenu
void ProcessManagerDialog::onTableContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(tr("刷新进程列表"), this, &ProcessManagerDialog::refresh);
    // 已选择进程时才提供终止选项
    if (m_killButton->isEnabled() && m_table->selectionModel()
        && !m_table->selectionModel()->selectedRows().isEmpty())
        menu.addAction(tr("终止进程"), this, &ProcessManagerDialog::onKillClicked);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace cubeshell
