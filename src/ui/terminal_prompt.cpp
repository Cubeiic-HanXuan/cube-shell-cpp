#include "terminal_prompt.h"

#include "Emulation.h"
#include "Session.h"
#include "qtermwidget.h"

namespace cubeshell {

TerminalPrompt::TerminalPrompt(QTermWidget *term, QObject *parent)
    : QObject(parent)
    , m_term(term)
{
}

void TerminalPrompt::ask(const QString &prompt, bool echo, Callback cb)
{
    // 已有提示在进行：先按取消收尾（回调以 ok=false 触发），再开新的一轮。
    if (m_active)
        cancel();

    Konsole::Session *session = m_term ? m_term->session() : nullptr;
    Konsole::Emulation *emulation = session ? session->emulation() : nullptr;
    if (!emulation) {
        // 终端还没建好/已析构：当作放弃，别把上层的回调吊死。
        if (cb)
            cb(false, QString());
        return;
    }

    // 断开 emulation→本地 Pty。会话是 startnow=0 建的（没有本地 shell），
    // 按键喂给未启动的 Pty 本就无处可去；更要紧的是别让行规程把字符回显
    // 回来——密码提示不回显才是对的。SshBridge::start() 之后还会调一次，
    // 重复调用无副作用（disconnect 幂等，started() 无人监听）。
    session->runEmptyPTY();

    m_buffer.clear();
    m_echo = echo;
    m_callback = std::move(cb);
    m_active = true;
    m_keyConn = connect(emulation, &Konsole::Emulation::sendData,
                        this, &TerminalPrompt::onKeyBytes);

    write(prompt);
    // 提示出来了就得能敲字：SSH 标签页并不保证终端已拿到键盘焦点
    // （QTermWidget 的 focusProxy 指向 TerminalDisplay，setFocus 会落到它身上）。
    if (m_term)
        m_term->setFocus();
}

void TerminalPrompt::write(const QString &text)
{
    Konsole::Session *session = m_term ? m_term->session() : nullptr;
    if (!session || text.isEmpty())
        return;

    // '\n' 补成 "\r\n"：这里绕过了 tty 的 ONLCR，只发 \n 的话光标只换行
    // 不回首列，多行提示会写成阶梯状。
    const QByteArray utf8 = text.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() + 8);
    char prev = 0;
    for (const char c : utf8) {
        if (c == '\n' && prev != '\r')
            out.append('\r');
        out.append(c);
        prev = c;
    }
    session->onReceiveBlock(out.constData(), int(out.size()));
}

void TerminalPrompt::cancel()
{
    if (!m_active)
        return;
    finish(false, QString());
}

void TerminalPrompt::onKeyBytes(const char *data, int length)
{
    if (!m_active || length <= 0)
        return;

    QByteArray payload;
    const KeyAction action = classifyKeys(QByteArray(data, length),
                                          m_buffer.isEmpty(), &payload);
    switch (action) {
    case KeyAction::Submit: {
        // 粘贴带换行时 payload 是换行前的那截，先并进来再提交。
        m_buffer.append(payload);
        const QString text = QString::fromUtf8(m_buffer);
        finish(true, text);
        break;
    }
    case KeyAction::Cancel:
        // 照终端惯例把 ^C 打在屏幕上，用户才知道这一下按到了。
        write(QStringLiteral("^C"));
        finish(false, QString());
        break;
    case KeyAction::Backspace:
        if (!m_buffer.isEmpty()) {
            chopUtf8Char(&m_buffer);
            if (m_echo)
                write(QStringLiteral("\b \b"));
        }
        break;
    case KeyAction::ClearLine:
        if (m_echo) {
            // 尽力而为：按字符数退格擦除（宽字符占两列，这里不追求精确，
            // 当前唯一用法是 echo=false 的密码提示）。
            const int chars = int(QString::fromUtf8(m_buffer).size());
            if (chars > 0) {
                write(QString(chars, QLatin1Char('\b'))
                      + QString(chars, QLatin1Char(' '))
                      + QString(chars, QLatin1Char('\b')));
            }
        }
        m_buffer.fill('\0');
        m_buffer.clear();
        break;
    case KeyAction::Append:
        m_buffer.append(payload);
        if (m_echo)
            write(QString::fromUtf8(payload));
        break;
    case KeyAction::Ignore:
        break;
    }
}

void TerminalPrompt::finish(bool ok, const QString &text)
{
    detach();
    // 提交/取消都换行：后续输出（"正在连接…"、远端 banner）从行首开始。
    write(QStringLiteral("\n"));

    // 缓冲里躺着刚输入的密码，先抹掉再走。回调可能立刻发起下一轮 ask()，
    // 所以 callback 必须先摘出来（ask 会覆盖 m_callback）。
    m_buffer.fill('\0');
    m_buffer.clear();
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    if (cb)
        cb(ok, text);
}

void TerminalPrompt::detach()
{
    if (m_keyConn)
        QObject::disconnect(m_keyConn);
    m_keyConn = QMetaObject::Connection();
    m_active = false;
}

TerminalPrompt::KeyAction TerminalPrompt::classifyKeys(const QByteArray &chunk,
                                                       bool bufferEmpty,
                                                       QByteArray *payload)
{
    if (payload)
        payload->clear();
    if (chunk.isEmpty())
        return KeyAction::Ignore;

    const unsigned char first = static_cast<unsigned char>(chunk.at(0));

    // ESC 打头 = 方向键/功能键/鼠标上报的控制序列，整块丢弃：既不该进缓冲，
    // 也不该被当成一次输入（否则 ↑ 会在密码里塞进 "[A"）。
    if (first == 0x1b)
        return KeyAction::Ignore;

    switch (first) {
    case 0x7f:   // DEL — 多数 keytab 把 Backspace 翻成这个
    case 0x08:   // BS
        return KeyAction::Backspace;
    case 0x03:   // Ctrl+C
        return KeyAction::Cancel;
    case 0x04:   // Ctrl+D：空行上是"放弃"，行内有内容时无意义（同 shell）
        return bufferEmpty ? KeyAction::Cancel : KeyAction::Ignore;
    case 0x15:   // Ctrl+U
        return KeyAction::ClearLine;
    default:
        break;
    }

    // 一般输入：整块扫一遍。遇到换行即提交，换行之前的字节照收——
    // 粘贴一段以换行结尾的密码（Cmd+V）就是这条路径。
    QByteArray text;
    text.reserve(chunk.size());
    for (const char c : chunk) {
        const unsigned char b = static_cast<unsigned char>(c);
        if (b == '\r' || b == '\n') {
            if (payload)
                *payload = text;
            return KeyAction::Submit;
        }
        // 其余 C0 控制字节与 DEL 丢弃；>= 0x80 是 UTF-8 续接字节，必须留。
        if (b < 0x20 || b == 0x7f)
            continue;
        text.append(c);
    }

    if (text.isEmpty())
        return KeyAction::Ignore;
    if (payload)
        *payload = text;
    return KeyAction::Append;
}

void TerminalPrompt::chopUtf8Char(QByteArray *buffer)
{
    if (!buffer || buffer->isEmpty())
        return;
    // UTF-8 续接字节形如 10xxxxxx：一路退到首字节，整个字符一起删，
    // 免得缓冲里留半截字节让 fromUtf8 变出 U+FFFD。
    int i = buffer->size() - 1;
    while (i > 0 && (static_cast<unsigned char>(buffer->at(i)) & 0xC0) == 0x80)
        --i;
    buffer->truncate(i);
}

} // namespace cubeshell
