// SshBridge.cpp — C++ port of core/paramiko_bridge.py. See SshBridge.h.

#include "SshBridge.h"

#include <QDebug>
#include <QMetaObject>

// shutdown()/SHUT_RDWR (POSIX) or shutdown()/SD_BOTH (Winsock)：stop() 里用它
// 强制唤醒仍卡在 socket I/O 上的读线程，避免主线程 detach 后的 UAF 窗口。
#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "SshClient.h"
#include "ShellMfaWatcher.h"

#include "Session.h"
#include "Emulation.h"

namespace cubeshell {

// OSC 7 剥离：ESC ] 7 ; file://<host>/<path> 以 BEL(\x07) 或 ST(ESC \) 结束。
//
// 纯字节扫描而非正则 —— 正则要先 QString::fromUtf8()，而 readChannel 按固定
// 大小切块，切点可能落在多字节 UTF-8 字符中间；fromUtf8 会把不完整序列换成
// U+FFFD，原始字节永久丢失且长度改变，转回 toUtf8 后送进终端就是乱码，
// 提示符里的 ANSI 序列一旦被截断，整段渲染都会错乱。
QByteArray SshBridge::stripOsc7(const QByteArray &in, QList<QByteArray> &paths)
{
    static const QByteArray kPrefix = QByteArrayLiteral("\x1b]7;file://");
    if (!in.contains(kPrefix))
        return in;

    QByteArray out;
    out.reserve(in.size());
    int pos = 0;
    while (pos < in.size()) {
        const int start = in.indexOf(kPrefix, pos);
        if (start < 0) {
            out.append(in.mid(pos));
            break;
        }
        out.append(in.mid(pos, start - pos));

        // 找结束符：BEL 或 ESC \。
        const int bodyStart = start + kPrefix.size();
        int end = -1;
        int endLen = 0;
        for (int i = bodyStart; i < in.size(); ++i) {
            if (in.at(i) == '\x07') {
                end = i;
                endLen = 1;
                break;
            }
            if (in.at(i) == '\x1b' && i + 1 < in.size() && in.at(i + 1) == '\\') {
                end = i;
                endLen = 2;
                break;
            }
        }
        if (end < 0) {
            // 序列未结束（被分包切断）：整段留给下一块拼接后再处理。
            out.append(in.mid(start));
            break;
        }

        // host 与 path 以第一个 '/' 分界，path 含该斜杠。
        const QByteArray body = in.mid(bodyStart, end - bodyStart);
        const int slash = body.indexOf('/');
        if (slash >= 0)
            paths.append(body.mid(slash));

        pos = end + endLen;
    }
    return out;
}

// 丢弃包含任一标记的整行。行以 \r 分隔（shell 回显用 \r\n）。
QByteArray SshBridge::dropMarkerLines(const QByteArray &in,
                                      const QList<QByteArray> &markers)
{
    bool hit = false;
    for (const QByteArray &m : markers) {
        if (in.contains(m)) {
            hit = true;
            break;
        }
    }
    if (!hit)
        return in;

    const QList<QByteArray> lines = in.split('\r');
    QList<QByteArray> kept;
    kept.reserve(lines.size());
    for (const QByteArray &line : lines) {
        bool drop = false;
        for (const QByteArray &m : markers) {
            if (line.contains(m)) {
                drop = true;
                break;
            }
        }
        if (!drop)
            kept.append(line);
    }

    QByteArray out;
    for (int i = 0; i < kept.size(); ++i) {
        if (i > 0)
            out.append('\r');
        out.append(kept.at(i));
    }
    return out;
}

SshBridge::SshBridge(Konsole::Session *session, SshClient *client, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_client(client)
    , m_mfaWatcher(new ShellMfaWatcher())
{
}

SshBridge::~SshBridge()
{
    stop();
    delete m_mfaWatcher;
}

void SshBridge::start()
{
    using Konsole::Emulation;

    // 0. Detach the default emulation→Pty sendData connection that Session's
    //    constructor establishes.  Without this, keystrokes are forwarded to
    //    BOTH the local (idle) PTY and the SSH channel; the PTY's line
    //    discipline echoes them back via receivedData → onReceiveBlock,
    //    producing double characters on screen.
    //    This mirrors what Session::runEmptyPTY() does for the same purpose.
    m_session->runEmptyPTY();

    // 1. User keystrokes from the emulation -> channel.
    Emulation *emulation = m_session->emulation();
    if (emulation) {
        connect(emulation, &Emulation::sendData,
                this, &SshBridge::onEmulationSendData);
    }

    // 1b. Terminal resize → SSH channel pty-size request.
    //     对应 Python: shell_process.setWindowSize = self._proxy_set_window_size
    connect(m_session, &Konsole::Session::terminalSizeApplied,
            this, &SshBridge::resize);

    // 2. Channel output -> Session::onReceiveBlock on the UI thread (queued).
    connect(this, &SshBridge::dataReceived,
            m_session,
            [session = m_session](const QByteArray &data) {
                session->onReceiveBlock(data.constData(), int(data.size()));
            },
            Qt::QueuedConnection);

    // 3. Run the read loop on a worker thread.
    m_running = true;
    m_readerThread = QThread::create([this]() { readLoop(); });
    m_readerThread->start();
}

void SshBridge::onEmulationSendData(const char *data, int length)
{
    if (!m_running || !m_client || !m_client->isChannelOpen())
        return;
    m_client->writeChannel(QByteArray(data, length));
}

// 剥除 _CUBE_ID=<数字>;<后随空白> 前缀，保留其后的真实命令。
// 等价于旧的 s_cubeIdPattern 正则，但在字节上做，避免 UTF-8 往返。
QByteArray SshBridge::stripCubeIdPrefix(const QByteArray &in)
{
    static const QByteArray kMarker = QByteArrayLiteral("_CUBE_ID=");
    if (!in.contains(kMarker))
        return in;

    QByteArray out;
    out.reserve(in.size());
    int pos = 0;
    while (pos < in.size()) {
        const int start = in.indexOf(kMarker, pos);
        if (start < 0) {
            out.append(in.mid(pos));
            break;
        }
        out.append(in.mid(pos, start - pos));

        int i = start + kMarker.size();
        // 必须紧跟至少一位数字，否则不是标记，原样保留。
        const int digitsStart = i;
        while (i < in.size() && in.at(i) >= '0' && in.at(i) <= '9')
            ++i;
        if (i == digitsStart || i >= in.size() || in.at(i) != ';') {
            out.append(kMarker);
            pos = start + kMarker.size();
            continue;
        }
        ++i;   // 跳过 ';'
        // 吞掉后随空白（对应正则的 \s*）。
        while (i < in.size() && QChar::isSpace(static_cast<unsigned char>(in.at(i))))
            ++i;
        pos = i;
    }
    return out;
}

// 返回末尾不完整多字节 UTF-8 序列的字节数（0 表示尾部完整）。
// 连续字节规律（RFC 3629）：
//   0xxxxxxx                  — 1 字节 (ASCII)
//   110xxxxx 10xxxxxx          — 2 字节
//   1110xxxx 10xxxxxx 10xxxxxx — 3 字节
//   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx — 4 字节
int SshBridge::incompleteUtf8TailLen(const QByteArray &in)
{
    if (in.isEmpty())
        return 0;
    // 从尾部倒查续字节（0b10xxxxxx），一个合法序列最多 3 个。
    int need = 0;
    int i = in.size() - 1;
    while (i >= 0 && need < 3
           && (static_cast<unsigned char>(in.at(i)) & 0xC0) == 0x80) {
        ++need;
        --i;
    }
    if (i < 0)
        return need;   // 全是续字节：前缀在上一块里，整块留待拼接

    const unsigned char c = static_cast<unsigned char>(in.at(i));
    if ((c & 0x80) == 0)
        return 0;      // ASCII：已是序列边界（孤立续字节属编码错误，照原样送出）

    int expect = 0;
    if ((c & 0xE0) == 0xC0)      expect = 1;
    else if ((c & 0xF0) == 0xE0) expect = 2;
    else if ((c & 0xF8) == 0xF0) expect = 3;
    else return 0;     // 非法前缀，不保留

    // 续字节已齐 → 尾部完整；否则连前缀一起留下（need + 前缀 1 字节）。
    return need >= expect ? 0 : need + 1;
}

void SshBridge::readLoop()
{
    while (m_running) {
        if (!m_client || !m_client->isChannelOpen())
            break;

        bool wouldBlock = false;
        QByteArray data = m_client->readChannel(4096, &wouldBlock);
        if (wouldBlock) {
            m_client->waitReadable(100); // brief poll; avoids a busy spin
            continue;
        }
        if (data.isEmpty())
            break; // EOF / closed

        // 先拼上一块留下的不完整 UTF-8 尾巴，再把本块新的不完整尾巴留下。
        // 顺序很重要：必须在任何 fromUtf8 / 内容过滤之前完成。
        if (!m_residual.isEmpty()) {
            data.prepend(m_residual);
            m_residual.clear();
        }
        if (const int tail = incompleteUtf8TailLen(data); tail > 0) {
            m_residual = data.right(tail);
            data.chop(tail);
            if (data.isEmpty())
                continue; // 整块都是半个字符，等下一块
        }

        // MFA / OTP prompt detection (watcher strips ANSI itself; non-blocking,
        // has a cooldown to avoid re-triggering).
        const QString prompt = m_mfaWatcher->feed(data);
        if (!prompt.isEmpty())
            emit shellMfaPromptDetected(prompt);

        // TerminalExecutor 需要原始数据（含哨兵）,必须在过滤之前发出。
        // AI 侧做文本分析,U+FFFD 无害,可以安全转 QString。
        emit rawDataForAi(QString::fromUtf8(data));

        // 字节级过滤：OSC 7、shell-integration hook、AI marker。
        QList<QByteArray> cwdPaths;
        QByteArray clean = stripOsc7(data, cwdPaths);
        for (const QByteArray &path : std::as_const(cwdPaths))
            emit cwdChanged(QString::fromUtf8(path));

        static const QList<QByteArray> kMarkers = {
            QByteArrayLiteral("__cs_osc7"),
            QByteArrayLiteral("__cube_end"),
            QByteArrayLiteral("PAGER=cat"),
            QByteArrayLiteral("__CUBE_AI_END__")
        };
        clean = dropMarkerLines(clean, kMarkers);

        // _CUBE_ID=N;<空白> 前缀剥除（保留其后的真实命令）。
        clean = stripCubeIdPrefix(clean);

        if (!clean.isEmpty())
            emit dataReceived(clean);
    }

    m_running = false;
    emit channelClosed();
}

void SshBridge::sendInput(const QString &text)
{
    if (!m_running || !m_client || !m_client->isChannelOpen())
        return;
    // KoKo-style line readers treat '\r' as submit; '\n' would time out.
    m_client->writeChannel((text + QLatin1Char('\r')).toUtf8());
}

void SshBridge::resize(int columns, int rows)
{
    if (!m_running || !m_client || !m_client->isChannelOpen())
        return;
    m_client->resizePty(columns, rows);
}

void SshBridge::injectShellIntegration()
{
    if (!m_running || !m_client || !m_client->isChannelOpen())
        return;
    // Leading space: bash skips commands starting with a space in history
    // (HISTCONTROL=ignorespace). Emits OSC 7 + ST before each prompt.
    static const char hookCmd[] =
        " __cs_osc7(){ printf '\\e]7;file://%s%s\\e\\\\' \"$(hostname)\" \"$(pwd)\"; };"
        "if [ -n \"$ZSH_VERSION\" ];then precmd(){ __cs_osc7; };"
        "elif [ -n \"$BASH_VERSION\" ];then "
        "PROMPT_COMMAND=\"${PROMPT_COMMAND:+$PROMPT_COMMAND;} __cs_osc7\";fi\n";
    m_client->writeChannel(QByteArray(hookCmd));
}

void SshBridge::stop()
{
    m_running = false;

    // Disconnect the emulation→SshBridge sendData connection so late
    // emissions (e.g. during teardown) don't reach a dead channel.
    Konsole::Emulation *emulation = m_session ? m_session->emulation() : nullptr;
    if (emulation) {
        disconnect(emulation, &Konsole::Emulation::sendData,
                   this, &SshBridge::onEmulationSendData);
    }

    // Disconnect resize forwarding.
    if (m_session) {
        disconnect(m_session, &Konsole::Session::terminalSizeApplied,
                   this, &SshBridge::resize);
    }

    // Null out m_session so a second stop() call (e.g. from ~SshBridge after
    // the Session has already been destroyed by Qt’s child-deletion order)
    // does not dereference a dangling pointer.
    m_session = nullptr;

    // 先把读线程 join 掉、确认它真正退出，再去 closeChannel()。
    //
    // 之前的写法是 wait(2000) 超时后就【不管读线程是否还活着】直接
    // closeChannel()：一旦读线程没能在 2s 内退出（它每次 readChannel 都要
    // 短暂持有 m_sessionMutex 做 libssh2_channel_read，网络慢/VM 抖动时退出
    // 会变慢），主线程就会去和仍持锁的读线程抢 m_sessionMutex，双方互等 →
    // 死锁（gdb 实测主线程停在 closeChannel → QBasicMutex::lockInternal →
    // futex_wait_queue）。Linux/虚拟机下读线程退出更慢，更容易撞上这个窗口，
    // 所以表现为"只有 Linux 卡死"。
    //
    // m_running 已在上面置 false；readLoop 每轮 waitReadable(≤100ms) 后都会
    // 重查 m_running，故读线程至多 ~100ms 内退出。给它充足但有限的余量循环
    // join，直到确认退出才往下走。
    if (m_readerThread) {
        for (int i = 0; i < 20 && !m_readerThread->wait(100); ++i) {
            // 每次迭代最多等 100ms；读线程退出后 wait 立即返回 true 跳出。
        }
        if (m_readerThread->isRunning()) {
            // 读线程仍卡在 socket I/O（select/read）上。先 shutdown socket
            // 强制唤醒它，使其从阻塞中返回并重查 m_running == false 后退出。
            if (m_client && m_client->socketFd() >= 0) {
#ifdef Q_OS_WIN
                // qintptr(有符号) → SOCKET(UINT_PTR,无符号) 需经 uintptr_t 过渡（同 SshClient）。
                ::shutdown(static_cast<SOCKET>(static_cast<uintptr_t>(m_client->socketFd())), SD_BOTH);
#else
                ::shutdown(static_cast<int>(m_client->socketFd()), SHUT_RDWR);
#endif
            }
            // shutdown 后给读线程最后一次退出机会（socket 不可读会立即返回错误）。
            if (!m_readerThread->wait(200)) {
                // 极端情况：读线程仍未退出（可能卡在内核深处）。分离以避免主线程
                // 死等，但存在短暂 UAF 窗口——实践中 shutdown 后几乎立即退出。
                qWarning() << "SshBridge::stop: reader thread did not exit even after"
                              " socket shutdown; detaching (channel close skipped)";
                m_readerThread = nullptr;
                return;
            }
        }
        m_readerThread->deleteLater();
        m_readerThread = nullptr;
    }
    if (m_client)
        m_client->closeChannel();
}

} // namespace cubeshell
