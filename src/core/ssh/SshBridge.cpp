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

// OSC 7: ESC ] 7 ; file://hostname/path  BEL  or  ESC ] 7 ; ... ESC \
// 对应C++: re.compile(rb'\x1b\]7;file://[^/]*(.*?)(?:\x07|\x1b\\)')
const QRegularExpression SshBridge::s_osc7Pattern(
    QStringLiteral("\x1b\\]7;file://[^/]*(.*?)(?:\x07|\x1b\\\\)"));

// AI command marker prefix: _CUBE_ID=<digits>;<optional whitespace>
// 对应C++: re.compile(rb'_CUBE_ID=\d+;\s*')
const QRegularExpression SshBridge::s_cubeIdPattern(
    QStringLiteral("_CUBE_ID=\\d+;\\s*"));

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

        // MFA / OTP prompt detection (watcher strips ANSI itself; non-blocking,
        // has a cooldown to avoid re-triggering).
        const QString prompt = m_mfaWatcher->feed(data);
        if (!prompt.isEmpty())
            emit shellMfaPromptDetected(prompt);

        const QString dataStr = QString::fromUtf8(data);

        // TerminalExecutor 需要原始数据（含哨兵），必须在过滤之前发出。
        emit rawDataForAi(dataStr);

        // OSC 7 cwd reporting.
        auto it = s_osc7Pattern.globalMatch(dataStr);
        while (it.hasNext()) {
            const QString path = it.next().captured(1);
            if (!path.isEmpty())
                emit cwdChanged(path);
        }

        // Filtering is done on the UTF-8 string form (OSC7 / cube markers are
        // all ASCII), then converted back to bytes for the terminal.
        QString clean = dataStr;

        // Strip OSC 7 sequences so they don't render as garbage.
        clean.remove(s_osc7Pattern);

        // Drop shell-integration hook echo lines (__cs_osc7).
        if (clean.contains(QLatin1String("__cs_osc7"))) {
            const QStringList lines = clean.split(QLatin1Char('\r'));
            QStringList kept;
            for (const QString &l : lines)
                if (!l.contains(QLatin1String("__cs_osc7")))
                    kept << l;
            clean = kept.join(QLatin1Char('\r'));
        }

        // Drop AI command-marker echo lines (hook 回显 + 哨兵输出行)。
        if (clean.contains(QLatin1String("__cube_end")) || clean.contains(QLatin1String("PAGER=cat"))
            || clean.contains(QLatin1String("__CUBE_AI_END__"))) {
            const QStringList lines = clean.split(QLatin1Char('\r'));
            QStringList kept;
            for (const QString &l : lines)
                if (!l.contains(QLatin1String("__cube_end")) && !l.contains(QLatin1String("PAGER=cat"))
                    && !l.contains(QLatin1String("__CUBE_AI_END__")))
                    kept << l;
            clean = kept.join(QLatin1Char('\r'));
        }
        // Replace _CUBE_ID=N; prefix, keeping the actual command.
        if (clean.contains(QLatin1String("_CUBE_ID=")))
            clean.remove(s_cubeIdPattern);

        const QByteArray cleanBytes = clean.toUtf8();
        if (!cleanBytes.isEmpty())
            emit dataReceived(cleanBytes);
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
                ::shutdown(reinterpret_cast<SOCKET>(m_client->socketFd()), SD_BOTH);
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
