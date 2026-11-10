// 进程管理重连与旧快照回归。默认离屏、无网络；--integration 使用测试 sshd。
// CUBESSH_HOST/PORT/USER/PASS；也可通过 CUBESSH_KEY 指定测试私钥。
#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTableView>
#include <QSortFilterProxyModel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QClipboard>
#include <QMap>
#include <QHeaderView>
#include <QTemporaryDir>
#include <QThread>
#include <libssh2.h>
#include <cerrno>
#include <functional>

#include "dialogs/ProcessManagerDialog.h"
#include "dialogs/ProcessTableModel.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SshClient.h"
#include "ssh_terminal_widget.h"

using namespace cubeshell;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

namespace cubeshell {
// 只用于构造远端结果/在途任务，不把可变测试入口暴露给应用调用方。
struct ProcessManagerDialogTestAccess {
    static void beginRefresh(ProcessManagerDialog &dialog) {
        dialog.m_task = ProcessManagerDialog::Task::Refresh;
        dialog.markStale(QStringLiteral("正在获取进程列表…"));
    }
    static void finish(ProcessManagerDialog &dialog, int code, const QString &out, const QString &err = {}) {
        dialog.onStreamFinished(code, out, err);
    }
    static void error(ProcessManagerDialog &dialog, const QString &message) {
        dialog.onStreamError(message);
    }
    static bool stale(const ProcessManagerDialog &dialog) { return dialog.m_stale; }
    static bool idle(const ProcessManagerDialog &dialog) { return dialog.m_task == ProcessManagerDialog::Task::None; }
    static QDateTime updated(const ProcessManagerDialog &dialog) { return dialog.m_lastUpdated; }
    static QPushButton *killButton(ProcessManagerDialog &dialog) { return dialog.m_killButton; }
    static ProcessTableModel *model(ProcessManagerDialog &dialog) { return dialog.m_model; }
    static QModelIndex index(ProcessManagerDialog &dialog, quint64 pid, int column = ProcessTableModel::Pid) {
        return dialog.m_proxy->mapFromSource(dialog.m_model->index(dialog.m_model->rowForPid(pid), column));
    }
    static void copyRows(ProcessManagerDialog &dialog) { dialog.copySelectedRows(); }
};
}
using Access = ProcessManagerDialogTestAccess;

static bool waitUntil(const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

static QString status(const ProcessManagerDialog &dialog)
{
    return dialog.findChild<QLabel *>(QStringLiteral("processStatus"))->text();
}

static QString snapshotOutput(const QString &ps, const QMap<quint64, QString> &paths = {}, const QString &ss = {})
{
    QString output = ps + QStringLiteral("\n===PS_SS_SEP===\n") + ss + QStringLiteral("\n===PS_EXE_SEP===\n");
    for (auto it = paths.begin(); it != paths.end(); ++it)
        output += QStringLiteral("/proc/%1").arg(it.key()) + QChar(QChar::Null) + it.value() + QChar(QChar::Null);
    return output + QStringLiteral("\0===PS_SNAPSHOT_END===\0");
}

static QString sampleOutput()
{
    return snapshotOutput(QStringLiteral(
        "root 1 0.0 0.1 2000 100 ? Ss 10:00 0:00 /sbin/init\n"
        "user 22 1.2 2.3 3000 200 ? S 10:01 0:01 worker-task\n"),
        {{1, QStringLiteral("/sbin/init")}, {22, QStringLiteral("/opt/worker task")}});
}

static void seedSnapshot(ProcessManagerDialog &dialog)
{
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, sampleOutput());
    CHECK(!Access::stale(dialog));
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 2);
}

static void testClientReplacement()
{
    ProcessManagerDialog dialog;
    auto first = std::make_shared<SshClient>();
    std::weak_ptr<SshClient> oldClient = first;
    dialog.setClient(first);
    QPointer<CommandExecutor> oldExecutor = dialog.findChild<CommandExecutor *>();
    CHECK(oldExecutor && oldExecutor->client() == first.get());
    dialog.setClient(first);
    CHECK(oldExecutor == dialog.findChild<CommandExecutor *>());
    seedSnapshot(dialog);

    // 旧线程的结束/失败通知已入队，但 UI 尚未消费。重连后不能污染新任务。
    emit oldExecutor->streamError(QStringLiteral("OLD CONNECTION ERROR"));
    emit oldExecutor->streamFinished(0, sampleOutput(), QString());
    CHECK(oldExecutor->execStream(QStringLiteral("echo offline")));
    first.reset();
    CHECK(!oldClient.expired()); // 执行器持有旧连接，线程退出前不能释放
    auto second = std::make_shared<SshClient>();
    dialog.setClient(second);   // 内部取消、join，随后才释放旧连接
    CHECK(oldExecutor.isNull());
    CHECK(oldClient.expired());
    CHECK(dialog.findChildren<CommandExecutor *>().size() == 1);
    CHECK(dialog.findChild<CommandExecutor *>()->client() == second.get());
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 0);
    Access::beginRefresh(dialog);
    const QString expected = status(dialog);
    QCoreApplication::processEvents();
    CHECK(status(dialog) == expected);
    CHECK(!Access::idle(dialog));
    Access::finish(dialog, 0, sampleOutput());
    CHECK(!Access::stale(dialog));

    dialog.setClient(nullptr);
    CHECK(!dialog.findChild<CommandExecutor *>());
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 2);
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(!Access::killButton(dialog)->isEnabled());
    dialog.setClient(second);
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 0);
    CHECK(!Access::updated(dialog).isValid());
}

static void testStaleSnapshot()
{
    ProcessManagerDialog dialog;
    dialog.setClient(std::make_shared<SshClient>());
    seedSnapshot(dialog);
    const QDateTime lastUpdated = Access::updated(dialog);
    Access::beginRefresh(dialog);
    Access::error(dialog, QStringLiteral("Unable to send channel-open request (code -7)"));
    CHECK(!Access::idle(dialog)); // 必须等配对的 finished，防止旧结果落到新任务上
    // 随后的 streamFinished(-1) 不能覆盖更准确的 streamError。
    const QString failureStatus = status(dialog);
    Access::finish(dialog, -1, QString());
    CHECK(status(dialog) == failureStatus);
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(status(dialog).contains(QStringLiteral("code -7")));
    CHECK(Access::updated(dialog) == lastUpdated);
    CHECK(!Access::killButton(dialog)->isEnabled());
    auto *search = dialog.findChild<QLineEdit *>();
    search->setText(QStringLiteral("worker-task"));
    CHECK(waitUntil([&]() { return dialog.findChild<QTableView *>()->model()->rowCount() == 1; }));
    CHECK(status(dialog).contains(QStringLiteral("共 2 个进程，显示 1 个")));
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(status(dialog).contains(QStringLiteral("code -7")));
    const QString details = dialog.findChild<QLabel *>(QStringLiteral("processStatus"))->toolTip();
    CHECK(details.contains(QStringLiteral("code -7")));
    CHECK(details.contains(QStringLiteral("旧数据")));
    CHECK(details.contains(lastUpdated.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    search->clear();
    seedSnapshot(dialog);
    CHECK(!status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(!status(dialog).contains(QStringLiteral("code -7")));

    // 非零退出码即使附带完整/部分输出，也不能覆盖已成功取得的列表。
    Access::beginRefresh(dialog);
    Access::finish(dialog, 1, sampleOutput(), QStringLiteral("ps failed"));
    CHECK(Access::stale(dialog));
    CHECK(status(dialog).contains(QStringLiteral("ps failed")));
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 2);
    // 缺失边界或无法解析的输出同样保留快照。
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, QStringLiteral("partial output\n"));
    CHECK(status(dialog).contains(QStringLiteral("数据不完整")));
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, snapshotOutput(QStringLiteral("malformed row\n")));
    CHECK(status(dialog).contains(QStringLiteral("无法解析")));
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() == 2);
    // 真实刷新入口必须拒绝未连接的 client，不能启动工作线程。
    dialog.refresh();
    CHECK(Access::idle(dialog));
    CHECK(!dialog.findChild<CommandExecutor *>()->isStreaming());
    CHECK(status(dialog).contains(QStringLiteral("连接不可用")));
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
}

static void testStableStatusLayout()
{
    QString output;
    for (int pid = 1; pid <= 120; ++pid)
        output += QStringLiteral("user %1 0.0 0.1 2000 100 ? S 10:00 0:00 worker-%1\n").arg(pid);
    output = snapshotOutput(output);

    // 覆盖最小窗口/较宽窗口及大字体；每种字体下的状态切换都必须保持布局稳定。
    for (const int pixels : {0, 24}) {
        for (const int width : {800, 1100}) {
            ProcessManagerDialog dialog;
            if (pixels > 0) {
                QFont font = dialog.font();
                font.setPixelSize(pixels);
                dialog.setFont(font);
            }
            dialog.setClient(std::make_shared<SshClient>());
            Access::beginRefresh(dialog);
            Access::finish(dialog, 0, output);
            dialog.resize(width, 600);
            dialog.show();
            auto settle = [&]() {
                QCoreApplication::processEvents();
                dialog.layout()->activate();
                QCoreApplication::processEvents();
            };
            settle();
            auto *table = dialog.findChild<QTableView *>();
            auto *label = dialog.findChild<QLabel *>(QStringLiteral("processStatus"));
            auto *killButton = Access::killButton(dialog);
            table->selectRow(60);
            table->scrollTo(table->model()->index(60, 0), QAbstractItemView::PositionAtCenter);
            settle();
            const QSize windowSize = dialog.size();
            const QRect tableRect = table->geometry();
            const QRect buttonRect = killButton->geometry();
            const QRect statusRect = label->geometry();
            const int scroll = table->verticalScrollBar()->value();
            CHECK(scroll > 0);
            auto stable = [&]() {
                settle();
                CHECK(dialog.size() == windowSize);
                CHECK(table->geometry() == tableRect);
                CHECK(killButton->geometry() == buttonRect);
                CHECK(label->geometry() == statusRect);
                CHECK(!label->wordWrap() && !label->hasHeightForWidth());
                CHECK(!label->text().contains(QLatin1Char('\n')));
                CHECK(!label->text().contains(QChar::LineSeparator));
            };
            auto selectionPreserved = [&]() {
                CHECK(table->selectionModel()->selectedRows().size() == 1);
                CHECK(table->currentIndex().row() == 60);
                CHECK(table->verticalScrollBar()->value() == scroll);
            };

            Access::beginRefresh(dialog);
            stable();
            selectionPreserved();
            CHECK(status(dialog).contains(QStringLiteral("正在获取进程列表")));
            CHECK(status(dialog).contains(QStringLiteral("旧数据")));

            const QString error = QStringLiteral("Unable to send channel-open request (code -7)\n")
                + QString(500, QLatin1Char('x')) + QStringLiteral("\r\n远端详情")
                + QChar(QChar::LineSeparator) + QStringLiteral("最后一行");
            Access::error(dialog, error);
            stable();
            selectionPreserved();
            CHECK(label->toolTip().contains(error)); // 完整多行错误仅放入悬停详情
            CHECK(label->text().indexOf(QStringLiteral("旧数据"))
                  < label->text().indexOf(QStringLiteral("获取进程列表失败")));
            Access::finish(dialog, -1, QString());
            stable();
            selectionPreserved();

            Access::beginRefresh(dialog);
            Access::finish(dialog, 0, output);
            stable();
            selectionPreserved(); // 成功换快照也按 PID 保留当前位置，而非跳回顶部
            CHECK(!status(dialog).contains(QStringLiteral("旧数据")));
            CHECK(!label->toolTip().contains(error));
            dialog.setClient(nullptr);
            stable();
            CHECK(status(dialog).contains(QStringLiteral("连接已断开")));
        }
    }
}

static QString psRow(quint64 pid, const QString &cpu, const QString &memory, const QString &command)
{
    return QStringLiteral("user %1 %2 %3 2000 100 ? S 10:00 0:00 %4\n").arg(pid).arg(cpu, memory, command);
}

static void selectPid(ProcessManagerDialog &dialog, quint64 pid)
{
    auto *table = dialog.findChild<QTableView *>();
    const auto index = Access::index(dialog, pid);
    CHECK(index.isValid());
    table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

static void testFullCommandAndPaths()
{
    const QString command = QStringLiteral("/opt/my-tool --arg='a  b\\tc' <raw>& ")
        + QString(8192, QLatin1Char('x')) + QStringLiteral(" --最后=完整  ");
    const QString path = QStringLiteral("/opt/目录 with spaces/bin/my-tool (deleted)");
    const QString strangePath = QStringLiteral("/srv/a\tb\nreal-file");
    const QString ps = psRow(9, QStringLiteral("125.5"), QStringLiteral("2.3"), command)
        + psRow(100, QStringLiteral("0.0"), QStringLiteral("0.0"), QStringLiteral("[kworker]"))
        + psRow(22, QStringLiteral("9.0"), QStringLiteral("1.0"), QStringLiteral("renamed-argv0"));
    const QString output = snapshotOutput(ps, {{9, path}, {22, strangePath}},
        QStringLiteral("tcp LISTEN 0 128 [::]:8080 [::]:* users:((\"my-tool\",pid=9,fd=3))\n"));
    const auto parsed = ProcessTableModel::parseSnapshot(output);
    CHECK(parsed.ok());
    CHECK(parsed.processes.size() == 3);
    if (parsed.processes.size() != 3) return;
    CHECK(parsed.processes[0].command == command);
    CHECK(parsed.processes[0].executablePath == path);
    CHECK(parsed.processes[0].port == QStringLiteral("8080"));
    CHECK(parsed.processes[1].executablePath.isEmpty());
    CHECK(parsed.processes[2].executablePath == strangePath);
    CHECK(ProcessTableModel::collectionCommand().contains(QStringLiteral("ps auxww")));
    CHECK(ProcessTableModel::collectionCommand().contains(QStringLiteral("find /proc")));
    CHECK(!ProcessTableModel::parseSnapshot(output.left(output.size() - 1)).ok());
    CHECK(!ProcessTableModel::parseSnapshot(snapshotOutput(psRow(9, "nan", "2", "bad"))).ok());
    CHECK(!ProcessTableModel::parseSnapshot(snapshotOutput(psRow(9, "1", "2", "a") + psRow(9, "1", "2", "b"))).ok());

    ProcessManagerDialog dialog;
    dialog.setClient(std::make_shared<SshClient>());
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, output);
    selectPid(dialog, 9);
    CHECK(Access::index(dialog, 9, ProcessTableModel::Command).data(ProcessTableModel::FullTextRole).toString() == command);
    CHECK(Access::index(dialog, 9, ProcessTableModel::Command).data(Qt::ToolTipRole).toString().contains(QStringLiteral("&lt;raw&gt;&amp;")));
    CHECK(dialog.findChild<QPlainTextEdit *>(QStringLiteral("processCommandDetails"))->toPlainText() == command);
    CHECK(dialog.findChild<QLineEdit *>(QStringLiteral("processExecutablePath"))->text() == path);
    dialog.findChild<QToolButton *>(QStringLiteral("copyProcessCommand"))->click();
    CHECK(QApplication::clipboard()->text() == command);
    dialog.findChild<QToolButton *>(QStringLiteral("copyProcessPath"))->click();
    CHECK(QApplication::clipboard()->text() == path);
    Access::copyRows(dialog);
    CHECK(QApplication::clipboard()->text().contains(command));
    selectPid(dialog, 100);
    CHECK(Access::index(dialog, 100, ProcessTableModel::ExecutablePath).data().toString() == QStringLiteral("不可读取"));
    CHECK(!dialog.findChild<QToolButton *>(QStringLiteral("copyProcessPath"))->isEnabled());
    CHECK(Access::model(dialog)->unavailablePathCount() == 1);
    selectPid(dialog, 22);
    dialog.findChild<QToolButton *>(QStringLiteral("copyProcessPath"))->click();
    CHECK(QApplication::clipboard()->text() == strangePath); // 复制原始路径，不用表格转义后的文字
}

static void testSortFilterAndRefresh()
{
    const auto initial = snapshotOutput(psRow(9, "9.0", "11.0", "old9")
        + psRow(100, "100.0", "2.0", "old100") + psRow(22, "2.0", "100.0", "old22"),
        {{9, QStringLiteral("/opt/agent[1].exe")}, {100, QStringLiteral("/bin/Alpha")}, {22, QStringLiteral("/bin/beta")}});
    ProcessManagerDialog dialog;
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, initial);
    auto *table = dialog.findChild<QTableView *>();
    auto firstPid = [&]() { return table->model()->index(0, 0).data(ProcessTableModel::PidRole).toULongLong(); };
    CHECK(firstPid() == 100); // 默认 CPU 降序，100 应高于 9
    table->sortByColumn(ProcessTableModel::Memory, Qt::DescendingOrder);
    CHECK(firstPid() == 22);
    table->sortByColumn(ProcessTableModel::Pid, Qt::AscendingOrder);
    CHECK(firstPid() == 9);
    table->sortByColumn(ProcessTableModel::ExecutablePath, Qt::AscendingOrder);
    CHECK(firstPid() == 100);

    int resets = 0;
    QObject::connect(Access::model(dialog), &QAbstractItemModel::modelReset, &dialog, [&]() { ++resets; });
    auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("processSearch"));
    search->setText(QStringLiteral("agent"));
    search->setText(QStringLiteral("agent["));
    search->setText(QStringLiteral("agent[1].exe"));
    CHECK(table->model()->rowCount() == 3); // 输入被合并，不同步重建全部行
    CHECK(waitUntil([&]() { return table->model()->rowCount() == 1; }));
    CHECK(firstPid() == 9); // 按字面量匹配，不把 [] 当正则表达式
    CHECK(resets == 0); // 筛选从未重建源模型
    search->clear();
    CHECK(table->model()->rowCount() == 3);

    table->sortByColumn(ProcessTableModel::Cpu, Qt::DescendingOrder);
    table->setColumnWidth(ProcessTableModel::ExecutablePath, 370);
    selectPid(dialog, 100);
    table->selectionModel()->select(Access::index(dialog, 9), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const auto refreshed = snapshotOutput(psRow(9, "99.0", "11.0", "new9")
        + psRow(100, "0.3", "2.0", "new100") + psRow(22, "9.0", "100.0", "new22"));
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, refreshed);
    CHECK(resets == 1);
    CHECK(table->currentIndex().data(ProcessTableModel::PidRole).toULongLong() == 100);
    CHECK(table->selectionModel()->selectedRows().size() == 2);
    CHECK(table->columnWidth(ProcessTableModel::ExecutablePath) == 370);
    CHECK(table->horizontalHeader()->sortIndicatorSection() == ProcessTableModel::Cpu);
    CHECK(firstPid() == 9);
    CHECK(dialog.findChild<QPlainTextEdit *>(QStringLiteral("processCommandDetails"))->toPlainText() == QStringLiteral("new100"));
    // 刷新完成时应用尚在防抖窗口中的新查询，不把旧筛选重新覆盖回来。
    search->setText(QStringLiteral("new100"));
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, refreshed);
    CHECK(table->model()->rowCount() == 1);
    CHECK(firstPid() == 100);
    search->clear();
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, snapshotOutput(psRow(9, "1", "2", "only9")));
    CHECK(!table->currentIndex().isValid()); // 已退出的进程不能被另一 PID 顶替
    CHECK(dialog.findChild<QPlainTextEdit *>(QStringLiteral("processCommandDetails"))->toPlainText().isEmpty());
}

static void testLargeSnapshot()
{
    QString ps;
    QMap<quint64, QString> paths;
    for (quint64 pid = 1; pid <= 10000; ++pid) {
        ps += psRow(pid, QString::number(pid % 137), QString::number(pid % 100), QStringLiteral("worker-%1 --args='a  b'").arg(pid));
        paths.insert(pid, QStringLiteral("/opt/workers/worker-%1").arg(pid));
    }
    const auto output = snapshotOutput(ps, paths);
    ProcessManagerDialog dialog;
    QElapsedTimer elapsed;
    elapsed.start();
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, output);
    qInfo() << "10000 processes: parse + model/sort refresh" << elapsed.elapsed() << "ms";
    CHECK(Access::model(dialog)->rowCount() == 10000);
    CHECK(!Access::stale(dialog));
    CHECK(Access::model(dialog)->unavailablePathCount() == 0);
    auto *table = dialog.findChild<QTableView *>();
    CHECK(table->findChildren<QLineEdit *>().isEmpty()); // 没有逐单元格控件/编辑器
    table->selectAll();
    elapsed.restart();
    Access::beginRefresh(dialog);
    Access::finish(dialog, 0, output);
    qInfo() << "10000 selected processes: refresh + restore selection" << elapsed.elapsed() << "ms";
    CHECK(table->selectionModel()->selectedRows().size() == 10000);
    CHECK(table->selectionModel()->selection().size() == 1);
    dialog.findChild<QLineEdit *>(QStringLiteral("processSearch"))->setText(QStringLiteral("worker-9999"));
    CHECK(waitUntil([&]() { return table->model()->rowCount() == 1; }));
    CHECK(table->model()->index(0, 0).data(ProcessTableModel::PidRole).toULongLong() == 9999);
}

static void testTransportState()
{
    auto client = std::make_shared<SshClient>();
    CommandExecutor executor(client);
    CHECK(!executor.hasUsableConnection());
    int notifications = 0;
    QObject::connect(client.get(), &SshClient::transportFailed, client.get(),
                     [&](const QString &) { ++notifications; });
    client->reportTransportError(LIBSSH2_ERROR_EAGAIN, QStringLiteral("retry"));
    client->reportTransportError(LIBSSH2_ERROR_CHANNEL_FAILURE, QStringLiteral("channel denied"));
    CHECK(notifications == 0);
    client->reportTransportError(LIBSSH2_ERROR_SOCKET_SEND, QStringLiteral("send failed"));
    client->reportTransportError(LIBSSH2_ERROR_SOCKET_RECV, QStringLiteral("read failed"));
    CHECK(notifications == 1);
    CHECK(!client->isConnected());
    CHECK(!client->isTransportAlive());
    const ExecResult result = executor.exec(QStringLiteral("echo must_not_run"));
    CHECK(!result.ok());
    CHECK(result.sshErrorCode == 0); // 前置拦截，没有尝试创建 libssh2 通道
    CHECK(result.errorMessage.contains(QStringLiteral("重新连接")));
}

static void testDisconnectNotification()
{
    SshTerminalWidget terminal(DeviceEntry{});
    int notifications = 0;
    QObject::connect(&terminal, &SshTerminalWidget::disconnected, &terminal,
                     [&]() { ++notifications; });
    // EOF 与 exec 错误可能先后到达，但只能让主窗口处理一次断线。
    terminal.notifyConnectionLost();
    terminal.notifyConnectionLost(QStringLiteral("send failed (code -7)"));
    terminal.notifyConnectionLost();
    CHECK(notifications == 1);
    bool hasReason = false;
    for (const auto *label : terminal.findChildren<QLabel *>())
        hasReason |= label->text().contains(QStringLiteral("code -7"));
    CHECK(hasReason);
}

static std::shared_ptr<SshClient> connectClient()
{
    auto client = std::make_shared<SshClient>();
    client->setHost(qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1"),
                    quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort()));
    client->setUsername(qEnvironmentVariable("CUBESSH_USER", "testuser"));
    const QString key = qEnvironmentVariable("CUBESSH_KEY");
    if (key.isEmpty()) client->setPassword(qEnvironmentVariable("CUBESSH_PASS", "testpass123"));
    else client->setPrivateKey(QStringLiteral("Ed25519Key"), key);
    client->setConnectTimeoutMs(3000);
    SshError error;
    if (!client->connectToHost(nullptr, error) || !client->openShell("xterm", 80, 24, error)) {
        qWarning() << "test sshd unavailable:" << error.message;
        return {};
    }
    return client;
}

static ssize_t failSend(libssh2_socket_t, const void *, size_t, int, void **)
{
    return -EPIPE; // 在 libssh2 真实发送入口注入失败，不修改服务器和业务进程
}

static int testLiveReconnect()
{
    auto first = connectClient();
    if (!first) return 2;
    ProcessManagerDialog dialog;
    dialog.setClient(first);
    dialog.refresh();
    CHECK(waitUntil([&]() { return Access::idle(dialog); }));
    CHECK(!Access::stale(dialog));
    CHECK(dialog.findChild<QTableView *>()->model()->rowCount() > 0);
    bool foundPath = false, foundFullCommand = false;
    for (int row = 0; row < Access::model(dialog)->rowCount(); ++row) {
        const auto *process = Access::model(dialog)->processAt(row);
        foundPath |= process->executablePath.startsWith(QLatin1Char('/'));
        // 批处理 shell 自身的命令超过普通 ps 的终端宽度，末尾标记不能被截断。
        foundFullCommand |= process->command.contains(QStringLiteral("ps auxww --no-headers"))
            && process->command.contains(QStringLiteral("===PS_SNAPSHOT_END==="));
    }
    CHECK(foundPath);
    CHECK(foundFullCommand);
    first->shutdownSocket();
    CHECK(first->rawSession()); // 回归：session 非空，但已知 socket 失效
    CHECK(!first->isConnected());
    dialog.refresh();
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(status(dialog).contains(QStringLiteral("连接不可用")));

    auto second = connectClient();
    CHECK(bool(second));
    if (!second) return 1;
    dialog.setClient(second);
    first.reset();
    dialog.refresh();
    CHECK(waitUntil([&]() { return Access::idle(dialog); }));
    CHECK(!Access::stale(dialog));
    auto *executor = dialog.findChild<CommandExecutor *>();
    executor->waitForFinished();
    CHECK(executor->client() == second.get());

    // SSH 已经认证且刷新成功后，让下一次真正的 channel-open 发送失败。
    int notifications = 0;
    QObject::connect(second.get(), &SshClient::transportFailed, &dialog,
                     [&](const QString &) { ++notifications; }, Qt::QueuedConnection);
#if LIBSSH2_VERSION_NUM >= 0x010B01
    libssh2_session_callback_set2(second->rawSession(), LIBSSH2_CALLBACK_SEND,
                                 reinterpret_cast<libssh2_cb_generic *>(&failSend));
#else
    libssh2_session_callback_set(second->rawSession(), LIBSSH2_CALLBACK_SEND,
                                reinterpret_cast<void *>(&failSend));
#endif
    dialog.refresh();
    CHECK(waitUntil([&]() { return Access::idle(dialog) && notifications > 0; }));
    executor->waitForFinished();
    CHECK(notifications == 1);
    CHECK(!second->isConnected());
    CHECK(!second->isTransportAlive());
    CHECK(status(dialog).contains(QStringLiteral("code -7")));
    CHECK(status(dialog).contains(QStringLiteral("旧数据")));
    CHECK(!Access::killButton(dialog)->isEnabled());
    const ExecResult retry = executor->exec(QStringLiteral("echo must_not_run"));
    CHECK(!retry.ok() && retry.sshErrorCode == 0);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--integration")))
        return testLiveReconnect();
    testClientReplacement();
    testStaleSnapshot();
    testStableStatusLayout();
    testFullCommandAndPaths();
    testSortFilterAndRefresh();
    testLargeSnapshot();
    testTransportState();
    testDisconnectNotification();
    qInfo() << "process_manager_test:" << failures << "failure(s)";
    return failures ? 1 : 0;
}
