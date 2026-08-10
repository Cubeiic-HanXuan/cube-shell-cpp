// terminal_search_test.cpp — 终端内容搜索的定位逻辑单元测试。
//
// 覆盖：
//   (a) lineIndexForOffset：偏移落在行首/行中/行尾/末行的换算
//   (b) lineIndexForOffset：空表与单行表的边界
//   (c) isAtOrBefore：跨行、同行、同点的序号判定
//   (d) 搜索正则构造：普通文本转义、大小写选项、非法正则
//   (e) 计数扫描：跳过空匹配（形如 "a*" 的正则不应把每个位置都算一次）

#include <QCoreApplication>
#include <QDebug>
#include <QList>
#include <QRegularExpression>

#include "qtermwidget.h"

using namespace cubeshell_search;

static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            qCritical() << "FAIL:" << msg << "at" << __FILE__ << ":" << __LINE__; \
            ++g_failed; \
            return; \
        } \
        ++g_passed; \
    } while (0)

#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            qCritical() << "FAIL:" << msg << " expected:" << (b) << "got:" << (a) \
                        << "at" << __FILE__ << ":" << __LINE__; \
            ++g_failed; \
            return; \
        } \
        ++g_passed; \
    } while (0)

// 模拟 PlainTextDecoder 的输出：三行文本，各行起始偏移。
//   行0: "error one\n"   偏移 0..9
//   行1: "warn two\n"    偏移 10..18
//   行2: "error three"   偏移 19..29
static QList<int> sampleLinePositions()
{
    return QList<int>{0, 10, 19};
}

static void testLineIndexBasic()
{
    const QList<int> lp = sampleLinePositions();

    TEST_ASSERT_EQ(lineIndexForOffset(lp, 0), 0, "行0行首");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 5), 0, "行0行中");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 9), 0, "行0行尾");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 10), 1, "行1行首");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 18), 1, "行1行尾");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 19), 2, "行2行首");
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 29), 2, "行2行中");
    // 超出末行长度也应钳在末行，不能越界。
    TEST_ASSERT_EQ(lineIndexForOffset(lp, 9999), 2, "偏移超界钳到末行");
}

static void testLineIndexEdgeCases()
{
    // 空表：没有任何行信息时返回 0，调用方用 value(,0) 取列 → 列为原偏移。
    TEST_ASSERT_EQ(lineIndexForOffset(QList<int>{}, 0), 0, "空表返回0");
    TEST_ASSERT_EQ(lineIndexForOffset(QList<int>{}, 42), 0, "空表任意偏移返回0");

    // 单行表。
    const QList<int> one{0};
    TEST_ASSERT_EQ(lineIndexForOffset(one, 0), 0, "单行行首");
    TEST_ASSERT_EQ(lineIndexForOffset(one, 100), 0, "单行大偏移");

    // 连续空行（相邻起始偏移相同）：应落在最后一个同偏移的行上。
    const QList<int> blanks{0, 5, 5, 5, 8};
    TEST_ASSERT_EQ(lineIndexForOffset(blanks, 5), 3, "连续空行取最后一行");
    TEST_ASSERT_EQ(lineIndexForOffset(blanks, 4), 0, "空行前仍在行0");
    TEST_ASSERT_EQ(lineIndexForOffset(blanks, 8), 4, "空行后进入下一行");
}

// 列换算：与生产代码同样的写法，验证组合结果。
static void testColumnComputation()
{
    const QList<int> lp = sampleLinePositions();

    // "error three" 里的 error 起始偏移 19 → 行2 列0。
    int idx = lineIndexForOffset(lp, 19);
    TEST_ASSERT_EQ(idx, 2, "行号");
    TEST_ASSERT_EQ(19 - lp.value(idx, 0), 0, "列号为0");

    // "warn two" 里的 two 起始偏移 15 → 行1 列5。
    idx = lineIndexForOffset(lp, 15);
    TEST_ASSERT_EQ(idx, 1, "行号");
    TEST_ASSERT_EQ(15 - lp.value(idx, 0), 5, "列号为5");
}

static void testIsAtOrBefore()
{
    // 参考点：行 5 列 10。
    TEST_ASSERT(isAtOrBefore(3, 0, 5, 10), "前面的行");
    TEST_ASSERT(isAtOrBefore(4, 999, 5, 10), "前一行的很后面仍算之前");
    TEST_ASSERT(isAtOrBefore(5, 9, 5, 10), "同行更靠前的列");
    TEST_ASSERT(isAtOrBefore(5, 10, 5, 10), "同点算「在此处」");
    TEST_ASSERT(!isAtOrBefore(5, 11, 5, 10), "同行更靠后的列");
    TEST_ASSERT(!isAtOrBefore(6, 0, 5, 10), "后面的行");

    // 行首参考点（列 0）。
    TEST_ASSERT(isAtOrBefore(0, 0, 0, 0), "原点自身");
    TEST_ASSERT(!isAtOrBefore(0, 1, 0, 0), "原点之后");
}

// 复刻 searchRegExp() 的构造规则（该方法依赖 SearchBar 实例，此处验证规则本身）。
static QRegularExpression buildRegExp(const QString &text, bool useRegex, bool matchCase)
{
    if (text.isEmpty())
        return QRegularExpression();
    QRegularExpression re(useRegex ? text : QRegularExpression::escape(text));
    re.setPatternOptions(matchCase ? QRegularExpression::NoPatternOption
                                   : QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid())
        return QRegularExpression();
    return re;
}

static void testRegExpConstruction()
{
    // 普通文本模式：正则元字符必须被转义，按字面量匹配。
    QRegularExpression re = buildRegExp("a.c", /*useRegex=*/false, /*matchCase=*/true);
    TEST_ASSERT(re.match("a.c").hasMatch(), "字面量匹配自身");
    TEST_ASSERT(!re.match("abc").hasMatch(), "点号不应通配");

    // 正则模式：元字符生效。
    re = buildRegExp("a.c", true, true);
    TEST_ASSERT(re.match("abc").hasMatch(), "正则模式下点号通配");

    // 大小写不敏感（默认）。
    re = buildRegExp("ERROR", false, /*matchCase=*/false);
    TEST_ASSERT(re.match("error").hasMatch(), "不区分大小写命中");
    re = buildRegExp("ERROR", false, /*matchCase=*/true);
    TEST_ASSERT(!re.match("error").hasMatch(), "区分大小写不命中");

    // 空关键字 → 无效/空 pattern，调用方据此跳过搜索。
    re = buildRegExp("", true, true);
    TEST_ASSERT(re.pattern().isEmpty(), "空关键字得到空 pattern");

    // 非法正则（未闭合括号）→ 空 pattern，不能塞进过滤器链。
    re = buildRegExp("(unclosed", /*useRegex=*/true, true);
    TEST_ASSERT(re.pattern().isEmpty(), "非法正则得到空 pattern");
}

// 计数扫描的核心不变式：空匹配不计数，否则 "a*" 会在每个位置命中一次。
static void testEmptyMatchesSkipped()
{
    const QString text = "error one\nwarn two\nerror three";

    QRegularExpression re = buildRegExp("error", false, false);
    int total = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        if (it.next().capturedLength() == 0)
            continue;
        ++total;
    }
    TEST_ASSERT_EQ(total, 2, "error 命中两次");

    // "o*" 在每个位置都能空匹配一次（此文本共 30 字符 → 31 个位置）；
    // 跳过空匹配后只剩真正含 o 的 4 处：err(o)r、(o)ne、tw(o)、err(o)r。
    re = buildRegExp("o*", /*useRegex=*/true, false);
    total = 0;
    it = re.globalMatch(text);
    while (it.hasNext()) {
        if (it.next().capturedLength() == 0)
            continue;
        ++total;
    }
    TEST_ASSERT_EQ(total, 4, "o* 仅计非空命中");
}

// 端到端串起来：在一段模拟日志里数出命中总数与当前序号。
static void testCountingWithCurrentIndex()
{
    // 三行，行起始偏移 {0, 10, 19}。error 出现在行0列0 与 行2列0。
    const QString text = "error one\nwarn two\nerror three";
    const QList<int> lp = sampleLinePositions();
    const QRegularExpression re = buildRegExp("error", false, false);

    // 当前命中定位在行2列0（第二个 error）→ 序号应为 2/2。
    const int refLine = 2, refColumn = 0;
    int total = 0, currentIndex = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength() == 0)
            continue;
        ++total;
        const int line = lineIndexForOffset(lp, m.capturedStart());
        const int column = m.capturedStart() - lp.value(line, 0);
        if (isAtOrBefore(line, column, refLine, refColumn))
            currentIndex = total;
    }
    TEST_ASSERT_EQ(total, 2, "总数为2");
    TEST_ASSERT_EQ(currentIndex, 2, "当前为第2个");

    // 当前命中回到行0列0 → 序号应为 1/2。
    total = 0;
    currentIndex = 0;
    it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength() == 0)
            continue;
        ++total;
        const int line = lineIndexForOffset(lp, m.capturedStart());
        const int column = m.capturedStart() - lp.value(line, 0);
        if (isAtOrBefore(line, column, /*refLine=*/0, /*refColumn=*/0))
            currentIndex = total;
    }
    TEST_ASSERT_EQ(total, 2, "总数仍为2");
    TEST_ASSERT_EQ(currentIndex, 1, "当前为第1个");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testLineIndexBasic();
    testLineIndexEdgeCases();
    testColumnComputation();
    testIsAtOrBefore();
    testRegExpConstruction();
    testEmptyMatchesSkipped();
    testCountingWithCurrentIndex();

    qInfo() << "passed:" << g_passed << "failed:" << g_failed;
    return g_failed == 0 ? 0 : 1;
}
