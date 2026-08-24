// terminal_prompt_test.cpp — TerminalPrompt 的纯逻辑单测。
//
// 被测的是"一段按键字节 → 一个动作"的归类（classifyKeys）和 UTF-8 退格
// （chopUtf8Char）。这两块正是连接时在终端里输密码的手感所在：方向键不能
// 混进密码、粘贴带换行要当提交、退格得整个汉字一起删。
//
// 不需要显示环境：两个函数都是 static 纯函数，不碰 QTermWidget。

#include <QCoreApplication>
#include <QDebug>

#include "terminal_prompt.h"

using namespace cubeshell;
using KeyAction = TerminalPrompt::KeyAction;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 便捷包装：非空缓冲（Ctrl+D 的语义依赖这个）默认按"缓冲里有内容"。
static KeyAction classify(const QByteArray &chunk, QByteArray *payload,
                          bool bufferEmpty = false)
{
    return TerminalPrompt::classifyKeys(chunk, bufferEmpty, payload);
}

static void testPlainCharacters()
{
    qInfo() << "--- testPlainCharacters ---";
    QByteArray payload;
    CHECK(classify(QByteArray("a"), &payload) == KeyAction::Append);
    CHECK(payload == QByteArray("a"));

    // 一次送多个字节（快速输入/粘贴）：整块进缓冲。
    CHECK(classify(QByteArray("s3cret"), &payload) == KeyAction::Append);
    CHECK(payload == QByteArray("s3cret"));

    // 空格是可打印字符，密码里合法。
    CHECK(classify(QByteArray("a b"), &payload) == KeyAction::Append);
    CHECK(payload == QByteArray("a b"));
}

static void testSubmit()
{
    qInfo() << "--- testSubmit ---";
    QByteArray payload;
    CHECK(classify(QByteArray("\r"), &payload) == KeyAction::Submit);
    CHECK(payload.isEmpty());
    CHECK(classify(QByteArray("\n"), &payload) == KeyAction::Submit);
    CHECK(payload.isEmpty());

    // 粘贴一段以换行结尾的密码：换行前的字节要一起提交，不能丢。
    CHECK(classify(QByteArray("pw123\r"), &payload) == KeyAction::Submit);
    CHECK(payload == QByteArray("pw123"));
    CHECK(classify(QByteArray("pw123\r\n"), &payload) == KeyAction::Submit);
    CHECK(payload == QByteArray("pw123"));

    // 换行后面还有内容（粘贴了两行）：第一行提交，余下丢弃——多行粘贴到
    // 单行提示里本就没有正确语义，取第一行是最不容易出事的做法。
    CHECK(classify(QByteArray("first\nsecond"), &payload) == KeyAction::Submit);
    CHECK(payload == QByteArray("first"));
}

static void testEditingKeys()
{
    qInfo() << "--- testEditingKeys ---";
    QByteArray payload;
    // Backspace：多数 keytab 发 DEL(0x7f)，少数发 BS(0x08)，都要认。
    CHECK(classify(QByteArray("\x7f"), &payload) == KeyAction::Backspace);
    CHECK(classify(QByteArray("\b"), &payload) == KeyAction::Backspace);
    // Ctrl+U 清行
    CHECK(classify(QByteArray("\x15"), &payload) == KeyAction::ClearLine);
}

static void testCancelKeys()
{
    qInfo() << "--- testCancelKeys ---";
    QByteArray payload;
    // Ctrl+C 一律取消，不论缓冲里有没有东西。
    CHECK(classify(QByteArray("\x03"), &payload, /*bufferEmpty=*/true) == KeyAction::Cancel);
    CHECK(classify(QByteArray("\x03"), &payload, /*bufferEmpty=*/false) == KeyAction::Cancel);

    // Ctrl+D 只在空行上是"放弃"；行内已有输入时无意义（同 shell 的 EOF 语义）。
    CHECK(classify(QByteArray("\x04"), &payload, /*bufferEmpty=*/true) == KeyAction::Cancel);
    CHECK(classify(QByteArray("\x04"), &payload, /*bufferEmpty=*/false) == KeyAction::Ignore);
}

static void testControlSequencesIgnored()
{
    qInfo() << "--- testControlSequencesIgnored ---";
    QByteArray payload;
    // 方向键/Home/End/F1 等都是 ESC 打头的序列：整块丢掉。
    // 不这么做，按一下 ↑ 就会往密码里塞进 "[A"。
    CHECK(classify(QByteArray("\x1b[A"), &payload) == KeyAction::Ignore);
    CHECK(classify(QByteArray("\x1b[B"), &payload) == KeyAction::Ignore);
    CHECK(classify(QByteArray("\x1b[3~"), &payload) == KeyAction::Ignore);
    CHECK(classify(QByteArray("\x1bOP"), &payload) == KeyAction::Ignore);
    // 裸 ESC 同理。
    CHECK(classify(QByteArray("\x1b"), &payload) == KeyAction::Ignore);

    // 其它落单的 C0 控制字节（这里是 Ctrl+A / Tab）：不进缓冲，也不作数。
    CHECK(classify(QByteArray("\x01"), &payload) == KeyAction::Ignore);
    CHECK(classify(QByteArray("\t"), &payload) == KeyAction::Ignore);

    // 空块：什么也没发生。
    CHECK(classify(QByteArray(), &payload) == KeyAction::Ignore);
}

static void testUtf8Multibyte()
{
    qInfo() << "--- testUtf8Multibyte ---";
    QByteArray payload;
    // 中文密码：一个汉字 3 字节，续接字节 >= 0x80 必须原样留着。
    const QByteArray hanzi = QStringLiteral("密").toUtf8();
    CHECK(hanzi.size() == 3);
    CHECK(classify(hanzi, &payload) == KeyAction::Append);
    CHECK(payload == hanzi);

    // emoji（4 字节）同理。
    const QByteArray emoji = QStringLiteral("🔑").toUtf8();
    CHECK(emoji.size() == 4);
    CHECK(classify(emoji, &payload) == KeyAction::Append);
    CHECK(payload == emoji);

    // 混合 ASCII + 多字节，且以换行结尾。
    const QByteArray mixed = QStringLiteral("a密b").toUtf8() + "\r";
    CHECK(classify(mixed, &payload) == KeyAction::Submit);
    CHECK(payload == QStringLiteral("a密b").toUtf8());
    CHECK(QString::fromUtf8(payload) == QStringLiteral("a密b"));
}

static void testChopUtf8Char()
{
    qInfo() << "--- testChopUtf8Char ---";
    // ASCII：删一个字节。
    QByteArray buf("abc");
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(buf == QByteArray("ab"));

    // 汉字：3 个字节整体删掉，不能留半截（否则 fromUtf8 变出 U+FFFD）。
    buf = QStringLiteral("a密").toUtf8();
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(buf == QByteArray("a"));
    CHECK(QString::fromUtf8(buf) == QStringLiteral("a"));

    // emoji：4 字节整体删掉。
    buf = QStringLiteral("x🔑").toUtf8();
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(buf == QByteArray("x"));

    // 连续删到空。
    buf = QStringLiteral("密码").toUtf8();
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(QString::fromUtf8(buf) == QStringLiteral("密"));
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(buf.isEmpty());

    // 空缓冲/空指针：不崩、不越界。
    TerminalPrompt::chopUtf8Char(&buf);
    CHECK(buf.isEmpty());
    TerminalPrompt::chopUtf8Char(nullptr);
}

static void testPayloadClearedOnNonAppend()
{
    qInfo() << "--- testPayloadClearedOnNonAppend ---";
    // payload 是复用的出参：上一轮的残留绝不能被当成这一轮的输入
    // （否则一次 Backspace 之后可能把上次的字节重新追加进密码）。
    QByteArray payload("stale");
    CHECK(classify(QByteArray("\x7f"), &payload) == KeyAction::Backspace);
    CHECK(payload.isEmpty());

    payload = QByteArray("stale");
    CHECK(classify(QByteArray("\x1b[A"), &payload) == KeyAction::Ignore);
    CHECK(payload.isEmpty());

    payload = QByteArray("stale");
    CHECK(classify(QByteArray("\x03"), &payload) == KeyAction::Cancel);
    CHECK(payload.isEmpty());
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testPlainCharacters();
    testSubmit();
    testEditingKeys();
    testCancelKeys();
    testControlSequencesIgnored();
    testUtf8Multibyte();
    testChopUtf8Char();
    testPayloadClearedOnNonAppend();

    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
