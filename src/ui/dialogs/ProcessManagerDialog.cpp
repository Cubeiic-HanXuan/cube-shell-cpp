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

namespace cubeshell {

namespace {
// 列序对应Python: display_processes 的 PID/用户/内存/CPU/端口/命令行
enum Column { ColPid = 0, ColUser, ColMem, ColCpu, ColPort, ColCommand, ColCount };

// ps 与 ss 批处理输出的分隔行（见 refresh()）。
const QLatin1String kPsSsSeparator("===PS_SS_SEP===");
} // namespace

ProcessManagerDialog::ProcessManagerDialog(CommandExecutor *executor, QWidget *parent)
    : QDialog(parent)
    , m_executor(nullptr)
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
    m_status = new QLabel(this);
    btnRow->addWidget(m_status);
    btnRow->addStretch(1);
    auto *killBtn = new QPushButton(tr("终止进程"), this);
    connect(killBtn, &QPushButton::clicked, this, &ProcessManagerDialog::onKillClicked);
    btnRow->addWidget(killBtn);
    auto *refreshBtn = new QPushButton(tr("刷新进程列表"), this);
    connect(refreshBtn, &QPushButton::clicked, this, &ProcessManagerDialog::refresh);
    btnRow->addWidget(refreshBtn);
    layout->addLayout(btnRow);

    setExecutor(executor);
}

void ProcessManagerDialog::setExecutor(CommandExecutor *executor)
{
    if (m_executor == executor)
        return;
    if (m_executor)
        disconnect(m_executor, nullptr, this, nullptr);
    m_executor = executor;
    m_task = Task::None;
    if (m_executor) {
        // 流式信号来自工作线程 — 显式 Queued。
        connect(m_executor, &CommandExecutor::streamFinished,
                this, &ProcessManagerDialog::onStreamFinished, Qt::QueuedConnection);
        connect(m_executor, &CommandExecutor::streamError,
                this, &ProcessManagerDialog::onStreamError, Qt::QueuedConnection);
    } else {
        m_all.clear();
        applyFilter();
        m_status->setText(tr("未连接 SSH 服务器"));
    }
}

void ProcessManagerDialog::refresh()
{
    if (!m_executor) {
        m_status->setText(tr("未连接 SSH 服务器"));
        return;
    }
    if (m_task != Task::None || m_executor->isStreaming())
        return; // 上一个命令还没结束
    // 对应Python: get_filtered_process_list 的 ps aux --no-headers + ss -tulnpe，
    // 单次批处理执行，用分隔行拆分两段输出。
    if (!m_executor->execStream(
            QStringLiteral("ps aux --no-headers; echo '%1'; ss -tulnpe 2>/dev/null")
                .arg(kPsSsSeparator),
            /*pty=*/false))
        return;
    m_task = Task::Refresh;
    m_status->setText(tr("正在获取进程列表…"));
}

// 对应Python: cube-shell.py::kill_selected_process
void ProcessManagerDialog::onKillClicked()
{
    if (!m_executor) {
        m_status->setText(tr("未连接 SSH 服务器"));
        return;
    }
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

    if (m_task != Task::None || m_executor->isStreaming())
        return;
    // kill -15 优雅终止（与 Python 侧一致）。
    if (!m_executor->execStream(QStringLiteral("kill -15 %1").arg(pids.join(QLatin1Char(' ')))))
        return;
    m_task = Task::Kill;
    m_status->setText(tr("正在终止进程…"));
}

void ProcessManagerDialog::onFilterChanged(const QString &text)
{
    Q_UNUSED(text);
    applyFilter();
}

void ProcessManagerDialog::onStreamFinished(int exitCode, const QString &stdoutText,
                                            const QString &stderrText)
{
    if (m_task == Task::Refresh) {
        m_task = Task::None;
        if (exitCode != 0 && stdoutText.trimmed().isEmpty()) {
            m_status->setText(tr("获取进程列表失败：%1").arg(stderrText.trimmed()));
            return;
        }
        // 按分隔行拆分 ps 与 ss 两段输出（未找到分隔符时 ss 段为空）。
        QString psOutput = stdoutText;
        QString ssOutput;
        const int sepIndex = stdoutText.indexOf(kPsSsSeparator);
        if (sepIndex >= 0) {
            psOutput = stdoutText.left(sepIndex);
            ssOutput = stdoutText.mid(sepIndex + kPsSsSeparator.size());
        }
        const auto pidPorts = parseSsOutput(ssOutput);
        parseProcessList(psOutput, pidPorts);
        applyFilter();
    } else if (m_task == Task::Kill) {
        m_task = Task::None;
        if (exitCode != 0 && !stderrText.trimmed().isEmpty())
            m_status->setText(tr("终止进程失败：%1").arg(stderrText.trimmed()));
        else
            m_status->setText(tr("进程已终止"));
        refresh();
    }
    // 其它组件发起的流式执行：不处理。
}

void ProcessManagerDialog::onStreamError(const QString &message)
{
    if (m_task == Task::None)
        return;
    m_task = Task::None;
    m_status->setText(tr("命令执行失败：%1").arg(message));
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
void ProcessManagerDialog::parseProcessList(const QString &psOutput,
                                            const QHash<QString, QStringList> &pidPorts)
{
    m_all.clear();
    const QStringList lines = psOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND...
        const QStringList fields = line.simplified().split(QLatin1Char(' '));
        if (fields.size() < 11)
            continue;
        RemoteProcess p;
        p.user = fields.at(0);
        p.pid = fields.at(1);
        p.cpu = fields.at(2);
        p.memory = fields.at(3);
        p.port = pidPorts.value(p.pid).join(QLatin1Char(','));
        p.command = QStringList(fields.mid(10)).join(QLatin1Char(' '));
        m_all.append(p);
    }
}

// 对应Python: apply_filter + display_processes
void ProcessManagerDialog::applyFilter()
{
    const QString needle = m_search->text().trimmed();
    m_table->setRowCount(0);
    int shown = 0;
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
        ++shown;
    }
    m_status->setText(tr("共 %1 个进程，显示 %2 个").arg(m_all.size()).arg(shown));
}

// 对应Python: cube-shell.py::showContextMenu
void ProcessManagerDialog::onTableContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(tr("刷新进程列表"), this, &ProcessManagerDialog::refresh);
    // 已选择进程时才提供终止选项
    if (m_table->selectionModel() && !m_table->selectionModel()->selectedRows().isEmpty())
        menu.addAction(tr("终止进程"), this, &ProcessManagerDialog::onKillClicked);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace cubeshell
