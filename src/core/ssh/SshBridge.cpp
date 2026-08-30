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
#include "terminal/session_recorder.h"

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

const QList<QByteArray> &SshBridge::markerList()
{
    static const QList<QByteArray> kMarkers = {
        QByteArrayLiteral("__cs_osc7"),
        QByteArrayLiteral("__cube_end"),
        QByteArrayLiteral("PAGER=cat"),
        QByteArrayLiteral("__CUBE_AI_END__")
    };
    return kMarkers;
}

// 折叠 readline 软换行：把 「空格+\r」「\r\n」「\r」「\n」从 buf 中去掉，
// 返回折叠后的字节，并在 pos 里记录每个保留字节在 buf 中的原始下标。
// 注入发生在 bash readline 激活后，回显是按 80 列折行的编辑重绘，软换行
// 处插入「空格+\r」，内容本身逐字节不变——折叠后即可与 hook 文本精确匹配。
static QByteArray foldSoftBreaks(const QByteArray &buf, QList<int> &pos)
{
    QByteArray fold;
    fold.reserve(buf.size());
    pos.clear();
    pos.reserve(buf.size());
    int i = 0;
    while (i < buf.size()) {
        const char c = buf.at(i);
        if (c == '\r') {
            // 软换行填充的空格一并去掉
            if (!fold.isEmpty() && fold.at(fold.size() - 1) == ' ') {
                fold.chop(1);
                pos.removeLast();
            }
            i += (i + 1 < buf.size() && buf.at(i + 1) == '\n') ? 2 : 1;
        } else if (c == '\n') {
            ++i;
        } else {
            fold.append(c);
            pos.append(i);
            ++i;
        }
    }
    return fold;
}

// hook 回显是否已完整到达（折叠软换行后能匹配到完整 hook 文本）。
bool SshBridge::containsHookEcho(const QByteArray &buf)
{
    QList<int> pos;
    const QByteArray fold = foldSoftBreaks(buf, pos);
    const QByteArray hook = shellHookCommand().trimmed().prepend(' ');
    // trimmed() 去掉结尾 \n；prepend 补回前导空格（trimmed 也会去掉它）。
    return fold.contains(hook);
}

// 折叠匹配删掉启动期缓冲里的每一处完整 hook 回显（可能两次：tty 驱动层
// 一次、readline 折行重绘一次），保住同行的 banner 和提示符。
//
// 同时删掉回显所在行的行首残余：bash 启动后会先打印一个提示符，然后才读到
// 注入命令（readline 折行回显），执行完又打印一个提示符。若只删 hook 文本，
// 那个「早产的提示符」会留下来，导致连接后出现两个提示符。把回显前面的同行
// 内容（提示符 + 标题转义序列）一并删掉，让后面的提示符顶替。
QByteArray SshBridge::stripHookEcho(const QByteArray &buf)
{
    const QByteArray hook = shellHookCommand().trimmed().prepend(' ');
    QList<int> pos;
    const QByteArray fold = foldSoftBreaks(buf, pos);
    if (pos.isEmpty())
        return buf;

    QByteArray out;
    out.reserve(buf.size());
    int src = 0;    // fold 里的扫描起点
    int keep = 0;   // buf 里保留内容的起点（上一段删除的结尾之后）
    while (true) {
        const int f = fold.indexOf(hook, src);
        if (f < 0)
            break;
        const int rawStart = pos.at(f);
        // 回显所在行的行首：向前扫到 \r/\n，但不超过上一段删除的结尾。
        // 行首的早产提示符连同回显一起删，避免双提示符。
        int lineStart = rawStart;
        while (lineStart > keep
               && buf.at(lineStart - 1) != '\r'
               && buf.at(lineStart - 1) != '\n')
            --lineStart;

        const int rawEnd = pos.at(f + hook.size() - 1);   // hook 末字节原始下标
        // 吞掉结尾的 \r\n / \r / \n
        int e = rawEnd + 1;
        if (e < buf.size() && buf.at(e) == '\r') ++e;
        if (e < buf.size() && buf.at(e) == '\n') ++e;
        out.append(buf.mid(keep, lineStart - keep));
        keep = e;
        src = f + 1;
    }
    out.append(buf.mid(keep));
    return out;
}

// 丢弃包含任一标记的整行（按 \r 切分，回显以 \r\n 结束）。
// 出现在流里的标记行只有两类，且都带换行，因此直接整行丢弃：
//  1) 连接时 hook 命令的回显——由启动期缓冲攒成完整行后送进来；
//  2) AI 执行器命令的回显——整行回显，带 \r\n。
// readline 翻历史产生的行内重绘（\r + 提示符 + 历史 + \e[K，不带换行）
// 里不会含标记：hook 已被 shellHookCommand() 从历史中删除，翻不出它。
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

void SshBridge::setRecorder(std::shared_ptr<SessionRecorder> rec)
{
    QMutexLocker locker(&m_recorderMutex);
    m_recorder = std::move(rec);
}

std::shared_ptr<SessionRecorder> SshBridge::recorder() const
{
    QMutexLocker locker(&m_recorderMutex);
    return m_recorder;
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
            // 启动期缓冲的兜底：如果远端 pty 没开回显（hook 回显永远不会来），
            // 静默若干轮后把已攒的内容放行，避免终端看起来被冻结。
            if (!m_bootstrapDone && !m_bootstrapBuf.isEmpty()) {
                if (++m_idlePolls >= 3) {
                    m_bootstrapDone = true;
                    QByteArray clean = dropMarkerLines(m_bootstrapBuf, markerList());
                    m_bootstrapBuf.clear();
                    clean = stripCubeIdPrefix(clean);
                    if (!clean.isEmpty())
                        emit dataReceived(clean);
                }
            }
            continue;
        }
        m_idlePolls = 0;
        if (data.isEmpty())
            break; // EOF / closed

        // 会话日志录制：在任何 UTF-8 拼接 / 内容过滤之前落盘，录到的才是与
        // Telnet/串口同语义的原始字节流。读线程在此持锁拷贝 shared_ptr，
        // recorder 内部再自带一把锁，UI 侧停止/销毁都不会造成悬垂。
        if (std::shared_ptr<SessionRecorder> rec = recorder())
            rec->writeRaw(data);

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

        // 启动期缓冲：hook 回显会被包边界切碎、且注入发生在 bash readline
        // 激活之后——回显是 readline 按 80 列把 349 字节折成多段、段间插
        // 「空格+\r」的编辑重绘，结尾是 \r 而非 \n。因此不能按行/按 \n 判断，
        // 必须折叠软换行后逐字节匹配完整 hook 文本再整段删。
        // 真实字节验证见 /tmp/cubeshell_boot.bin 的分析。
        if (!m_bootstrapDone) {
            m_bootstrapBuf += clean;
            const bool arrived = containsHookEcho(m_bootstrapBuf);
            if (arrived) {
                clean = stripHookEcho(m_bootstrapBuf);
                m_bootstrapBuf.clear();
                m_bootstrapDone = true;
            } else if (m_bootstrapBuf.size() >= 8192) {
                // 异常长度：远端没按预期回显，放弃缓冲以免吞掉正常输出。
                m_bootstrapDone = true;
                clean = m_bootstrapBuf;
                m_bootstrapBuf.clear();
            } else {
                continue;
            }
        }

        clean = dropMarkerLines(clean, markerList());

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

QByteArray SshBridge::shellHookCommand()
{
    // 前导空格：配置了 HISTCONTROL=ignorespace 的环境会直接跳过本行进历史。
    // 但不能依赖它（CentOS 默认只有 ignoredups），所以 bash 分支末尾还会主动
    // 删除本行历史。hook 若进了历史，按上下键会翻出含 __cs_osc7 的那行，
    // readline 行内重绘（\r + 提示符 + 历史 + \e[K）无法被显示层安全过滤
    // （整段丢会吞掉提示符和 \e[K，抹白会把本行变成一串空格）——必须不进历史。
    //
    // bash 删除自身：history 1 取最后一条；仅当它就是 hook 行（含 __cs_osc7）
    // 时才 history -d，若 ignorespace 已生效（最后一条是用户旧命令）则不动。
    // zsh 无对应机制，用 histignorespace 只影响后续行，hook 行本会话内仍在，
    // 属可接受折中。
    static const char hookCmd[] =
        " __cs_osc7(){ printf '\\e]7;file://%s%s\\e\\\\' \"$(hostname)\" \"$(pwd)\"; };"
        "if [ -n \"$ZSH_VERSION\" ];then setopt histignorespace;precmd(){ __cs_osc7; };"
        "elif [ -n \"$BASH_VERSION\" ];then HISTCONTROL=ignoreboth;"
        "PROMPT_COMMAND=\"${PROMPT_COMMAND:+$PROMPT_COMMAND;} __cs_osc7\";"
        "case \"$(history 1)\" in *__cs_osc7*)history -d $(history 1|awk '{print $1}');;esac;fi\n";
    return QByteArray(hookCmd);
}

void SshBridge::injectShellIntegration()
{
    if (!m_running || !m_client || !m_client->isChannelOpen())
        return;
    m_client->writeChannel(shellHookCommand());
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
