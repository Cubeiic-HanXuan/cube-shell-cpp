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
    QStringLiteral("__CUBE_AI_END__:(\\d+):(-?\\d+)\\r?\\n"));

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

    resetConnection();
    if (m_terminal)
        disconnect(m_terminal, nullptr, this, nullptr);

    m_terminal = terminal;

    // 换了终端就是换了 shell —— 环境 hook 必须重新注入。
    m_initializedTerminalId = 0;

    QMutexLocker locker(&m_mutex);
    resetOutputLocked();
}

QTermWidget *TerminalExecutor::terminal() const
{
    return m_terminal.data();
}

// 对应Python: _TerminalExecutor.run_blocking
void TerminalExecutor::resetConnection()
{
    cancel();
    m_initializedTerminalId = 0;
}

QPair<int, QString> TerminalExecutor::runBlocking(const QString &cmd, int timeoutMs,
                                                 const std::atomic<bool> *stopFlag)
{
    if (cmd.trimmed().isEmpty())
        return {-1, QStringLiteral("(空命令)")};

    int id = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_inFlight) return {-1, QStringLiteral("(已有命令正在执行)")};
        if (stopFlag && stopFlag->load()) return {-1, QStringLiteral("(已取消)")};
        m_inFlight = true;
        id = m_currentId = m_nextId++;
        m_resultReady = false;
        m_resultExitCode = -1;
        m_resultOutput.clear();
        resetOutputLocked();
        m_outputTruncated = false;
        m_cancelled.store(false);
    }

    // sendText 会走 Session/Emulation，必须在本对象所属线程（主线程）执行。
    QMetaObject::invokeMethod(this, [this, cmd, id] { dispatch(cmd, id); }, Qt::QueuedConnection);

    // 主线程自己调用时不能用 QWaitCondition：事件循环被挂死后哨兵永远送不到。
    if (thread() == QThread::currentThread())
        return waitOnOwnThread(timeoutMs);

    QMutexLocker locker(&m_mutex);
    const QDeadlineTimer deadline(timeoutMs);
    while (!m_resultReady) {
        if (!m_condition.wait(&m_mutex, deadline)) {
            m_currentId = 0;
            m_inFlight = false;
            const QString output = (m_outputTruncated ? QStringLiteral("[较早输出已截断]\n") : QString()) + capturedOutputLocked();
            resetOutputLocked();
            return {-1, output + QStringLiteral("\n(命令执行超时，远端状态未知)")};
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
    m_inFlight = false;
    const QString output = (m_outputTruncated ? QStringLiteral("[较早输出已截断]\n") : QString()) + capturedOutputLocked();
    resetOutputLocked();
    return {-1, output + QStringLiteral("\n(命令执行超时，远端状态未知)")};
}

QPair<int, QString> TerminalExecutor::takeResultLocked()
{
    const QPair<int, QString> result(m_resultExitCode, m_resultOutput);
    m_resultReady = false;
    m_inFlight = false;
    m_resultOutput.clear();
    m_currentId = 0;
    resetOutputLocked();
    return result;
}

// 主线程执行：注入环境 hook（首次）并把带 _CUBE_ID 前缀的命令喂给终端。
void TerminalExecutor::dispatch(const QString &cmd, int id)
{
    {
        QMutexLocker locker(&m_mutex);
        if (id != m_currentId || m_resultReady || !m_inFlight) return;
    }
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

    {
        QMutexLocker locker(&m_mutex);
        if (id != m_currentId || m_resultReady) return;
    }

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
        // 前缀 _CUBE_ID=0：让本行命中 HISTIGNORE 规则；末尾 _CUBE_ID= 清空，
        // 避免 __cube_end 误发哨兵（0 非空会触发 echo）。
        "_CUBE_ID=0; "
        "export PAGER=cat SYSTEMD_PAGER=cat GIT_PAGER=cat MANPAGER=cat "
        "DEBIAN_FRONTEND=noninteractive LESS=FRX > /dev/null 2>&1; "
        // 后续 AI 命令（_CUBE_ID=N; ...）不进历史：HISTIGNORE 模式排除，
        // 追加而非覆盖以保留用户已有设置。
        "HISTIGNORE=\"${HISTIGNORE:+$HISTIGNORE:}*_CUBE_ID=*\"; export HISTIGNORE; "
        "__cube_end(){ local rc=$?; [ -n \"$_CUBE_ID\" ] || return; "
        "echo \"__CUBE_AI_END__:$_CUBE_ID:$rc\"; _CUBE_ID=\"\"; "
        "printf '\\033[A\\033[2K\\r'; }; "
        // 幂等 + 保留已有 PROMPT_COMMAND（SshBridge 的 OSC7 cwd 钩子挂在那里）。
        "if [ -z \"$_CUBE_HOOK\" ]; then _CUBE_HOOK=1; "
        "if [ -n \"$ZSH_VERSION\" ]; then precmd_functions+=(__cube_end); "
        "else PROMPT_COMMAND=\"__cube_end${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"; fi; fi; "
        "_CUBE_ID=; "
        // 删掉本 init 行自身的历史：HISTIGNORE 在行尾才生效、救不了当前行，
        // 只能事后删。仅当最后一条历史确实是本行（含 _CUBE_ID=0 前缀）时才删，
        // 避免误删用户的命令。
        "case \"$(history 1)\" in *_CUBE_ID=0*PAGER=cat*)history -d $(history 1|awk '{print $1}');;esac; "
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

    const QString clean = cleanChunkLocked(text);
    appendOutputLocked(clean);
    // Generated IDs and exit codes are 32-bit decimal values. Retain enough
    // overlap for a complete split marker; never rescan the accumulated log.
    constexpr qsizetype markerOverlap = 64;
    const qsizetype scanFrom = qMax(m_outputStart, m_outputBuffer.size() - clean.size() - markerOverlap);
    QRegularExpressionMatchIterator it = s_sentinelRe.globalMatch(m_outputBuffer, scanFrom);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(1).toInt() != m_currentId)
            continue; // 上一条命令的残留哨兵，忽略

        m_resultExitCode = match.captured(2).toInt();
        m_resultOutput = extractOutput(m_outputBuffer.mid(m_outputStart), int(match.capturedStart(0) - m_outputStart));
        if (m_outputTruncated)
            m_resultOutput.prepend(QStringLiteral("[输出过长，已截断；仅保留最后 %1 字符]\n").arg(kMaxBufferChars));
        m_resultReady = true;
        m_currentId = 0;
        resetOutputLocked();
        m_condition.wakeAll();
        return;
    }
    locker.unlock();
    if (!clean.isEmpty()) emit outputReceived(clean);
}

void TerminalExecutor::cancel()
{
    QMutexLocker locker(&m_mutex);
    if (m_currentId == 0 && !m_resultReady)
        return; // 没有在跑的命令

    m_cancelled.store(true);
    m_resultExitCode = -1;
    m_resultOutput = (m_outputTruncated ? QStringLiteral("[较早输出已截断]\n") : QString())
        + capturedOutputLocked() + QStringLiteral("\n(已停止等待，远端进程可能仍在运行)");
    m_resultReady = true;
    m_currentId = 0;
    resetOutputLocked();
    m_condition.wakeAll();
}

// 哨兵之前的内容即命令输出：掐掉首行命令回显与哨兵所在行的行首残余。
QString TerminalExecutor::extractOutput(const QString &cleanBuffer, int sentinelStart)
{
    QString body = cleanBuffer.left(sentinelStart);
    body.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    QStringList lines = body.split(QLatin1Char('\n'));

    // A command such as printf has no trailing newline: its final fragment is
    // real output, not a prompt to discard.
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
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

void TerminalExecutor::resetOutputLocked()
{
    m_outputBuffer.clear();
    m_outputStart = 0;
    m_pendingAnsi.clear();
    m_outputTruncated = false;
}

void TerminalExecutor::appendOutputLocked(const QString &text)
{
    m_outputBuffer += text;
    if (m_outputBuffer.size() - m_outputStart > kMaxBufferChars) {
        m_outputStart = m_outputBuffer.size() - kMaxBufferChars;
        m_outputTruncated = true;
    }
    // Amortize copying the retained tail while keeping physical storage bounded.
    if (m_outputStart >= kMaxBufferChars / 4) {
        m_outputBuffer.remove(0, m_outputStart);
        m_outputStart = 0;
    }
}

QString TerminalExecutor::capturedOutputLocked() const
{
    // Incomplete escapes on timeout/cancellation remain visible rather than
    // silently losing output. Normal packet processing never rescans this tail.
    return stripAnsi((m_outputBuffer.mid(m_outputStart) + m_pendingAnsi).right(kMaxBufferChars));
}

QString TerminalExecutor::cleanChunkLocked(const QString &text)
{
    const QString data = m_pendingAnsi + text;
    m_pendingAnsi.clear();
    QString clean;
    clean.reserve(data.size());
    qsizetype position = 0;
    while (position < data.size()) {
        const qsizetype escape = data.indexOf(QChar(0x1b), position);
        if (escape < 0) { clean += data.mid(position); break; }
        clean += data.mid(position, escape - position);
        const auto match = s_ansiRe.match(data, escape, QRegularExpression::NormalMatch,
                                        QRegularExpression::AnchorAtOffsetMatchOption);
        if (match.hasMatch()) { position = match.capturedEnd(); continue; }
        // Keep only a possible incomplete supported sequence, not arbitrary log
        // text. CSI, OSC/ST and charset introducers may straddle any packet.
        bool incomplete = escape + 1 == data.size();
        if (!incomplete) {
            const QChar kind = data.at(escape + 1);
            if (kind == '[') {
                qsizetype i = escape + 2;
                while (i < data.size() && (data.at(i).isDigit() || data.at(i) == ';' || data.at(i) == '?')) ++i;
                incomplete = i == data.size();
            } else if (kind == ']') {
                qsizetype i = escape + 2;
                while (i < data.size() && data.at(i) != QChar(0x1b) && data.at(i) != QChar(7)) ++i;
                incomplete = i == data.size() || (i + 1 == data.size() && data.at(i) == QChar(0x1b));
            } else if (kind == '(' || kind == ')') incomplete = escape + 2 == data.size();
        }
        if (incomplete && data.size() - escape <= 4096) {
            const auto tail = data.mid(escape);
            if (!tail.contains(QStringLiteral("__CUBE_AI_END__:"))) {
                m_pendingAnsi = tail;
                break;
            }
        }
        // Malformed/unbounded escapes must not swallow visible output or the
        // completion marker forever. Preserve them as the old normalizer did.
        clean += data.at(escape);
        position = escape + 1;
    }
    clean.remove(QChar(7));
    return clean;
}

QString TerminalExecutor::stripAnsi(const QString &text)
{
    QString clean = text;
    clean.remove(s_ansiRe);
    clean.remove(QLatin1Char('\a')); // BEL（OSC 之外的裸响铃）
    return clean;
}

} // namespace cubeshell
