#pragma once

// terminal_prompt.h — 在终端画面里就地读一行输入（可关回显），不弹任何对话框。
//
// 用途：SSH 连接前把密码问出来。终端工具的惯例是像 ssh 那样在终端里提示
//   陈德兵@192.168.1.10 的密码：
// 而不是弹一个模态对话框——后者打断的是"我在终端里连机器"这件事本身。
//
// 复用既有的两条通路，不碰 TerminalDisplay 的事件链：
//   写屏：Konsole::Session::onReceiveBlock()（同 TcpBridge::writeToTerminal）
//   收键：Konsole::Emulation::sendData()    （同 SshBridge::onEmulationSendData）
// 之所以能这样接：QTermWidget(startnow=0) 建出来的会话没有本地 shell，
// ask() 里先 runEmptyPTY() 断开 emulation→本地 Pty，键盘就只喂给本类了。
//
// 异步、不阻塞事件循环：ask() 立即返回，结果经回调送出。这一点与
// QInputDialog::getText 的嵌套事件循环相反——上层（连接 worker）不会被卡住。

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QTermWidget;

namespace cubeshell {

class TerminalPrompt : public QObject {
    Q_OBJECT
public:
    // ok=false 表示用户放弃（Ctrl+C / 空行上的 Ctrl+D / cancel()）。
    using Callback = std::function<void(bool ok, const QString &text)>;

    explicit TerminalPrompt(QTermWidget *term, QObject *parent = nullptr);

    // 显示 prompt 并开始收键。echo=false 时一个字符都不回显（同 ssh 的密码提示）。
    // 正在进行的提示会先被 cancel()。cb 在提示结束后调用一次，可在其中再次 ask()。
    void ask(const QString &prompt, bool echo, Callback cb);

    // 往终端画面写文本。'\n' 自动补成 "\r\n"——终端不做 ONLCR 转换，
    // 只发 \n 的话下一行会从上一行的列位置开始（阶梯状输出）。
    void write(const QString &text);

    bool active() const { return m_active; }

    // 外部中止（组件析构 / 关标签页）。回调以 ok=false 触发。
    void cancel();

    // --- 纯逻辑（静态，便于单测；见 tests/terminal_prompt_test.cpp）---
    //
    // 按键字节的语义。终端把按键翻译成字节后经 Emulation::sendData 送出，
    // 一次可能带多个字节（粘贴、多字节 UTF-8 字符、方向键的 ESC 序列）。
    enum class KeyAction {
        Append,     // payload 追加到输入缓冲
        Submit,     // 回车
        Backspace,  // 退格
        ClearLine,  // Ctrl+U
        Cancel,     // Ctrl+C，或空缓冲上的 Ctrl+D
        Ignore,     // 方向键等控制序列：不进缓冲也不作数
    };

    // chunk 归类为一个动作；Append 时把可用字节写进 payload。
    // bufferEmpty 只影响 Ctrl+D 的语义（空行上取消，否则忽略），同 shell。
    static KeyAction classifyKeys(const QByteArray &chunk, bool bufferEmpty,
                                  QByteArray *payload);

    // 从 UTF-8 缓冲尾部删掉一个完整字符（多字节字符整体删掉，不留半截）。
    static void chopUtf8Char(QByteArray *buffer);

private:
    void onKeyBytes(const char *data, int length);
    void finish(bool ok, const QString &text);
    void detach();

    QPointer<QTermWidget> m_term;
    QByteArray m_buffer;
    Callback m_callback;
    // emulation→本类的按键连接。只在提示进行期间存在：提示结束就断开，
    // 键盘立刻回到 SshBridge（或本地 Pty）该有的归属，不留旁听者。
    QMetaObject::Connection m_keyConn;
    bool m_active = false;
    bool m_echo = false;
};

} // namespace cubeshell
