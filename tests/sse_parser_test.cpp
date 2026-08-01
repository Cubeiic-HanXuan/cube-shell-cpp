// SSE parser unit test: line buffering / CRLF / escaped payloads / empty
// blocks / [DONE] sentinel / multiple consecutive events / incomplete
// trailing-line buffering / finish() flush. Pure logic — no network.
//
// 对应Python: openai SDK 内部 SSE 解析的行为对照（worker.py 迭代协议层）

#include <QByteArray>
#include <QDebug>
#include <QList>
#include <QString>

#include "ai/SseClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 单事件基础解析：data 行 + 空行分发
static void testBasicEvent()
{
    SseEventParser parser;
    const QList<SseEvent> events =
        parser.feed("data: {\"x\":1}\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().type == QStringLiteral("message"));
    CHECK(events.first().data == QStringLiteral("{\"x\":1}"));
    CHECK(!parser.isDone());
}

// event:/id: 字段 + 多 data 行 join('\n')
static void testFieldsAndMultiData()
{
    SseEventParser parser;
    const QList<SseEvent> events = parser.feed(
        "event: delta\nid: 42\ndata: line1\ndata: line2\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().type == QStringLiteral("delta"));
    CHECK(events.first().id == QStringLiteral("42"));
    CHECK(events.first().data == QStringLiteral("line1\nline2"));
}

// 不完整行缓冲：跨 feed 的字节切分不得破坏事件
static void testIncompleteLineBuffering()
{
    SseEventParser parser;
    QList<SseEvent> events;
    events += parser.feed("da");
    CHECK(events.isEmpty());
    events += parser.feed("ta: hel");
    CHECK(events.isEmpty());
    events += parser.feed("lo\n");
    CHECK(events.isEmpty());        // 尚无空行，不分发
    events += parser.feed("\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("hello"));
}

// CRLF 换行容忍
static void testCrlf()
{
    SseEventParser parser;
    const QList<SseEvent> events =
        parser.feed("data: a\r\n\r\ndata: b\r\n\r\n");
    CHECK(events.size() == 2);
    CHECK(events.at(0).data == QStringLiteral("a"));
    CHECK(events.at(1).data == QStringLiteral("b"));
}

// 转义/含冒号的 JSON 载荷不被字段切分破坏（只按第一个 ':' 切）
static void testEscapedJsonPayload()
{
    SseEventParser parser;
    const QByteArray payload =
        "{\"content\":\"a:b \\\"quoted\\\" \\n newline 中文\"}";
    const QList<SseEvent> events =
        parser.feed("data: " + payload + "\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data == QString::fromUtf8(payload));
}

// 空块：连续空行 / 无 data 的 event 行不产生事件
static void testEmptyBlocks()
{
    SseEventParser parser;
    QList<SseEvent> events = parser.feed("\n\n\n");
    CHECK(events.isEmpty());
    // 只有 event: 字段没有 data → 空行时不分发（无待分发数据）
    events = parser.feed("event: ping\n\n");
    CHECK(events.isEmpty());
    // 注释行忽略
    events = parser.feed(": keep-alive comment\n\n");
    CHECK(events.isEmpty());
    // 空 data 行（"data:"）也算有效数据
    events = parser.feed("data:\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data.isEmpty());
}

// [DONE] 终止标记：吞掉不分发，isDone 置位
static void testDoneSentinel()
{
    SseEventParser parser;
    QList<SseEvent> events = parser.feed(
        "data: {\"a\":1}\n\ndata: [DONE]\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("{\"a\":1}"));
    CHECK(parser.isDone());
}

// 多事件连续到达（单次 feed 含多个完整事件）
static void testMultipleConsecutiveEvents()
{
    SseEventParser parser;
    QByteArray chunk;
    for (int i = 0; i < 5; ++i)
        chunk += "data: {\"n\":" + QByteArray::number(i) + "}\n\n";
    const QList<SseEvent> events = parser.feed(chunk);
    CHECK(events.size() == 5);
    CHECK(events.at(0).data == QStringLiteral("{\"n\":0}"));
    CHECK(events.at(4).data == QStringLiteral("{\"n\":4}"));
}

// finish() 宽容冲刷：末尾缺空行/缺换行的事件也要分发
static void testFinishFlush()
{
    // 缺最后空行
    SseEventParser p1;
    QList<SseEvent> events = p1.feed("data: tail\n");
    CHECK(events.isEmpty());
    events = p1.finish();
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("tail"));

    // 连换行都没有的裸尾行
    SseEventParser p2;
    events = p2.feed("data: bare");
    CHECK(events.isEmpty());
    events = p2.finish();
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("bare"));

    // finish 时的 [DONE] 同样吞掉
    SseEventParser p3;
    p3.feed("data: [DONE]");
    events = p3.finish();
    CHECK(events.isEmpty());
    CHECK(p3.isDone());
}

// reset() 后可复用
static void testReset()
{
    SseEventParser parser;
    parser.feed("data: [DONE]\n\n");
    CHECK(parser.isDone());
    parser.reset();
    CHECK(!parser.isDone());
    const QList<SseEvent> events = parser.feed("data: again\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("again"));
}

// 字段名后无空格（"data:x"）也要接受（规范允许 0/1 个前导空格）
static void testNoSpaceAfterColon()
{
    SseEventParser parser;
    const QList<SseEvent> events = parser.feed("data:x\n\n");
    CHECK(events.size() == 1);
    CHECK(events.first().data == QStringLiteral("x"));
}

int main()
{
    testBasicEvent();
    testFieldsAndMultiData();
    testIncompleteLineBuffering();
    testCrlf();
    testEscapedJsonPayload();
    testEmptyBlocks();
    testDoneSentinel();
    testMultipleConsecutiveEvents();
    testFinishFlush();
    testReset();
    testNoSpaceAfterColon();

    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "sse_parser_test: all checks passed";
    return 0;
}
