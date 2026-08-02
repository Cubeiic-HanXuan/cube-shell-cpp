// TerminalExecutor.cpp — 见 TerminalExecutor.h（对应Python: ssh_agent.py::_TerminalExecutor）。

#include "TerminalExecutor.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QStringList>
#include <QThread>

#include "qtermwidget.h"

namespace cubeshell {

// 哨兵：__CUBE_AI_END__:<命令id>:<退出码>
// 对应Python: re.compile(r'__CUBE_AI_END__:(\d+):(-?\d+)')
const QRegularExpression TerminalExecutor::s_sentinelRe(
    QStringLiteral("__CUBE_AI_END__:(\\d+):(-?\\d+)"));

// 控制序列：OSC(ESC ] ... BEL/ST)、CSI(ESC [ ... 字母)、字符集选择、单字符转义。
// 对应Python: re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\x1b\].*?\x07')
const QRegularExpression TerminalExecutor::s_ansiRe(
    QStringLiteral("\x1b\\][^\x07\x1b]*(?:\x07|\x1b\\\\)"
                   "|\x1b\\[[0-9;?]*[A-Za-z]"
                   "|\x1b[()][0-9A-Za-z]"
                   "|\x1b[=>MDEHc78]"));

TerminalExecutor::TerminalExecutor(QObject *parent)
    : QObject(parent)
{
}

TerminalExecutor::~TerminalExecutor()
{
    // 唤醒可能仍阻塞在 runBlocking 里的工作线程，避免它拿着已析构的对象等下去。
    cancel();
}

void TerminalExecutor::setTerminal(QTermWidget *terminal)
{
    if (m_terminal == terminal)
        return;

    if (m_terminal)
        disconnect(m_terminal, nullptr, this, nullptr);

    m_terminal = terminal;

    // 换了终端就是换了 shell —— 环境 hook 必须重新注入。
    m_initializedTerminalId = 0;

    QMutexLocker locker(&m_mutex);
    m_outputBuffer.clear();
}

QTermWidget *TerminalExecutor::terminal() const
{
    return m_terminal.data();
}

// 对应Python: _TerminalExecutor.run_blocking
QPair<int, QString> TerminalExecutor::runBlocking(const QString &cmd, int timeoutMs)
{
    if (cmd.trimmed().isEmpty())
        return {-1, QStringLiteral("(空命令)")};

    {
        QMutexLocker locker(&m_mutex);
        m_currentId = m_nextId++;
        m_resultReady = false;
        m_resultExitCode = -1;
        m_resultOutput.clear();
        m_outputBuffer.clear();
        m_cancelled.store(false);
    }

    // sendText 会走 Session/Emulation，必须在本对象所属线程（主线程）执行。
    QMetaObject::invokeMethod(this, "dispatch", Qt::QueuedConnection,
                              Q_ARG(QString, cmd));

    // 主线程自己调用时不能用 QWaitCondition：事件循环被挂死后哨兵永远送不到。
    if (thread() == QThread::currentThread())
        return waitOnOwnThread(timeoutMs);

    QMutexLocker locker(&m_mutex);
    const QDeadlineTimer deadline(timeoutMs);
    while (!m_resultReady) {
        if (!m_condition.wait(&m_mutex, deadline)) {
            m_currentId = 0;
            m_outputBuffer.clear();
            return {-1, QStringLiteral("(命令执行超时)")};
        }
    }
    return takeResultLocked();
}

QPair<int, QString> TerminalExecutor::waitOnOwnThread(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!timer.hasExpired(timeoutMs)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        {
            QMutexLocker locker(&m_mutex);
            if (m_resultReady)
                return takeResultLocked();
        }
        QThread::msleep(5); // processEvents 无事件时立即返回，避免忙等
    }

    QMutexLocker locker(&m_mutex);
    m_currentId = 0;
    m_outputBuffer.clear();
    return {-1, QStringLiteral("(命令执行超时)")};
}

QPair<int, QString> TerminalExecutor::takeResultLocked()
{
    const QPair<int, QString> result(m_resultExitCode, m_resultOutput);
    m_resultReady = false;
    m_resultOutput.clear();
    m_currentId = 0;
    m_outputBuffer.clear();
    return result;
}

// 主线程执行：注入环境 hook（首次）并把带 _CUBE_ID 前缀的命令喂给终端。
void TerminalExecutor::dispatch(const QString &cmd)
{
    if (!m_terminal) {
        QMutexLocker locker(&m_mutex);
        m_resultExitCode = -1;
        m_resultOutput = QStringLiteral("(终端不可用)");
        m_resultReady = true;
        m_currentId = 0;
        m_condition.wakeAll();
        return;
    }

    if (m_initializedTerminalId != quintptr(m_terminal.data()))
        setupEnvironment();

    int id = 0;
    {
        QMutexLocker locker(&m_mutex);
        id = m_currentId;
    }
    if (id == 0)
        return; // 已被 cancel() / 超时接管，别再往终端里塞命令

    // _CUBE_ID 前缀既是哨兵的关联 id，也是 SshBridge 过滤回显的依据。
    const QString commandStr = QStringLiteral("_CUBE_ID=%1; %2").arg(id).arg(cmd);
    m_terminal->sendText(commandStr + QLatin1Char('\n'));
}

// 主线程执行：一次性注入分页器禁用 + PROMPT_COMMAND 哨兵钩子。
// 整行都含 __cube_end / PAGER=cat，SshBridge 会把这段回显整行丢掉，用户看不见。
void TerminalExecutor::setupEnvironment()
{
    if (!m_terminal)
        return;

    static const QString initCmd = QStringLiteral(
        "export PAGER=cat SYSTEMD_PAGER=cat GIT_PAGER=cat MANPAGER=cat "
        "DEBIAN_FRONTEND=noninteractive LESS=FRX > /dev/null 2>&1; "
        "__cube_end(){ local rc=$?; [ -n \"$_CUBE_ID\" ] || return; "
        "echo \"__CUBE_AI_END__:$_CUBE_ID:$rc\"; _CUBE_ID=\"\"; "
        "printf '\\033[A\\033[2K\\r'; }; "
        // 幂等 + 保留已有 PROMPT_COMMAND（SshBridge 的 OSC7 cwd 钩子挂在那里）。
        "if [ -z \"$_CUBE_HOOK\" ]; then _CUBE_HOOK=1; "
        "if [ -n \"$ZSH_VERSION\" ]; then precmd_functions+=(__cube_end); "
        "else PROMPT_COMMAND=\"__cube_end${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"; fi; fi; "
        "printf '\\033[A\\033[2K\\r'\n");

    m_terminal->sendText(initCmd);

    // 同一个终端只注入一次；换终端时 setTerminal 会清零。
    m_initializedTerminalId = quintptr(m_terminal.data());
}

// 主线程槽：接收 SshBridge 过滤前的原始数据（含哨兵）。
// 对应Python: _TerminalExecutor._on_received
void TerminalExecutor::onRawData(const QString &text)
{
    QMutexLocker locker(&m_mutex);
    if (m_currentId == 0 || m_resultReady)
        return; // 没有在跑的命令

    m_outputBuffer += text;
    if (m_outputBuffer.size() > kMaxBufferChars)
        m_outputBuffer = m_outputBuffer.right(kMaxBufferChars);

    const QString clean = stripAnsi(m_outputBuffer);
    QRegularExpressionMatchIterator it = s_sentinelRe.globalMatch(clean);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(1).toInt() != m_currentId)
            continue; // 上一条命令的残留哨兵，忽略

        m_resultExitCode = match.captured(2).toInt();
        m_resultOutput = extractOutput(clean, match.capturedStart(0));
        m_resultReady = true;
        m_currentId = 0;
        m_outputBuffer.clear();
        m_condition.wakeAll();
        return;
    }
}

void TerminalExecutor::cancel()
{
    QMutexLocker locker(&m_mutex);
    if (m_currentId == 0 && !m_resultReady)
        return; // 没有在跑的命令

    m_cancelled.store(true);
    m_resultExitCode = -1;
    m_resultOutput = QStringLiteral("(已取消)");
    m_resultReady = true;
    m_currentId = 0;
    m_outputBuffer.clear();
    m_condition.wakeAll();
}

// 哨兵之前的内容即命令输出：掐掉首行命令回显与哨兵所在行的行首残余。
QString TerminalExecutor::extractOutput(const QString &cleanBuffer, int sentinelStart)
{
    QString body = cleanBuffer.left(sentinelStart);
    body.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    QStringList lines = body.split(QLatin1Char('\n'));

    // 末段是哨兵行的行首（提示符等），不属于输出。
    if (!lines.isEmpty())
        lines.removeLast();
    // 首行是命令回显："<提示符> _CUBE_ID=N; <cmd>"。
    if (!lines.isEmpty() && lines.first().contains(QLatin1String("_CUBE_ID=")))
        lines.removeFirst();

    for (QString &line : lines) {
        // 行内 \r 是覆盖写（进度条），只保留最后一次覆盖的结果。
        const int cr = line.lastIndexOf(QLatin1Char('\r'));
        if (cr >= 0)
            line = line.mid(cr + 1);
        while (!line.isEmpty() && line.at(line.size() - 1).isSpace())
            line.chop(1);
    }

    QString out = lines.join(QLatin1Char('\n'));
    while (!out.isEmpty() && out.at(out.size() - 1).isSpace())
        out.chop(1);
    while (out.startsWith(QLatin1Char('\n')))
        out.remove(0, 1);
    return out;
}

QString TerminalExecutor::stripAnsi(const QString &text)
{
    QString clean = text;
    clean.remove(s_ansiRe);
    clean.remove(QLatin1Char('\a')); // BEL（OSC 之外的裸响铃）
    return clean;
}

} // namespace cubeshell
