// 文件树筛选/排序共用逻辑（file_browser_common.h）的单测：纯函数，无显示环境。
//
// 钉住两条用户可感知的行为：
//  1) 筛选：子串、忽略大小写，空筛选词恒真——写错一处用户就会"找不到明明
//     存在的文件"。
//  2) 排序：目录恒在文件前（升降序都保持）、数值列（大小/日期）按裸值比而
//     非格式化文本、列值平局以名称裁决、降序只翻转列比较不翻转目录分组。
//     其中"降序保持目录在前"是最容易写反的一条（直接对 less 取反就会把
//     目录甩到文件后面，还顺带破坏严格弱序）。

#include <QApplication>
#include <QDebug>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "file_browser_common.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static FileSortKey key(const QString &name, bool isDir, qint64 size = 0,
                       qint64 mtime = 0, const QString &perm = QString(),
                       const QString &owner = QString())
{
    FileSortKey k;
    k.name = name;
    k.isDir = isDir;
    k.size = size;
    k.mtime = mtime;
    k.perm = perm;
    k.owner = owner;
    return k;
}

static void testMatchesFilter()
{
    // 空筛选词恒真（收起检索框时列表必须完整还原）。
    CHECK(fileMatchesFilter(QStringLiteral("a.log"), QString()));
    CHECK(fileMatchesFilter(QStringLiteral(".hidden"), QString()));

    // 子串匹配，忽略大小写。
    CHECK(fileMatchesFilter(QStringLiteral("nginx.conf"), QStringLiteral("conf")));
    CHECK(fileMatchesFilter(QStringLiteral("README.md"), QStringLiteral("readme")));
    CHECK(fileMatchesFilter(QStringLiteral("App.log"), QStringLiteral("app")));

    // 非子串不中；只命中部分字符也不中（必须是连续子串）。
    CHECK(!fileMatchesFilter(QStringLiteral("a.log"), QStringLiteral("logg")));
    CHECK(!fileMatchesFilter(QStringLiteral("ab.txt"), QStringLiteral("ac")));
}

static void testNameSortColumn()
{
    using S = Qt::SortOrder;
    // 忽略大小写。
    CHECK(fileTreeSortLess(0, S::AscendingOrder,
                           key(QStringLiteral("abc"), false), key(QStringLiteral("Def"), false)));
    CHECK(!fileTreeSortLess(0, S::AscendingOrder,
                            key(QStringLiteral("Def"), false), key(QStringLiteral("abc"), false)));
    // 隐藏文件前导点不参与排序：".env" 按 "env" 排。
    CHECK(fileTreeSortLess(0, S::AscendingOrder,
                           key(QStringLiteral(".env"), false), key(QStringLiteral("zeta"), false)));
    // 降序翻转。
    CHECK(fileTreeSortLess(0, S::DescendingOrder,
                           key(QStringLiteral("Def"), false), key(QStringLiteral("abc"), false)));
    // 相等元素 comp(a,a) 必须为 false（严格弱序，std::sort 的 UB 防线）。
    CHECK(!fileTreeSortLess(0, S::AscendingOrder,
                            key(QStringLiteral("a"), false), key(QStringLiteral("a"), false)));
    CHECK(!fileTreeSortLess(0, S::DescendingOrder,
                            key(QStringLiteral("a"), false), key(QStringLiteral("a"), false)));
}

// 目录分组只适用于名称列：升降序都保持目录在前。
static void testDirsFirstOnNameColumn()
{
    using S = Qt::SortOrder;
    const FileSortKey dir = key(QStringLiteral("zdir"), true);
    const FileSortKey file = key(QStringLiteral("afile"), false);
    CHECK(fileTreeSortLess(0, S::AscendingOrder, dir, file));
    CHECK(!fileTreeSortLess(0, S::AscendingOrder, file, dir));
    CHECK(fileTreeSortLess(0, S::DescendingOrder, dir, file));
    CHECK(!fileTreeSortLess(0, S::DescendingOrder, file, dir));
}

// 非名称列全列单调混排：目录不提前，只按列值比。钉住这条是因为此前
// "目录恒在前"让日期列在目录/文件分界处倒退，用户看到的就是"排序是乱的"。
static void testNonNameColumnsAreGloballyMonotonic()
{
    using S = Qt::SortOrder;
    // 目录的 mtime 更新，升序时也排在新文件之后（不按目录身份提前）。
    const FileSortKey newerDir = key(QStringLiteral("zdir"), true, 0, 3000);
    const FileSortKey olderFile = key(QStringLiteral("afile"), false, 0, 1000);
    CHECK(fileTreeSortLess(2, S::AscendingOrder, olderFile, newerDir));
    CHECK(fileTreeSortLess(2, S::DescendingOrder, newerDir, olderFile));
    // 大小列同理：小文件排在大目录之前。
    const FileSortKey bigDir = key(QStringLiteral("zdir"), true, 4096);
    const FileSortKey tinyFile = key(QStringLiteral("afile"), false, 10);
    CHECK(fileTreeSortLess(1, S::AscendingOrder, tinyFile, bigDir));
}

static void testNumericColumns()
{
    using S = Qt::SortOrder;
    // 大小按裸字节数比：1 GB > 4 MB（格式化文本直接比字符串会比错）。
    const FileSortKey small = key(QStringLiteral("small"), false, 4 * 1024 * 1024);
    const FileSortKey big = key(QStringLiteral("big"), false, 1024LL * 1024 * 1024);
    CHECK(fileTreeSortLess(1, S::AscendingOrder, small, big));
    CHECK(fileTreeSortLess(1, S::DescendingOrder, big, small));

    // 日期按裸 epoch 秒比。
    const FileSortKey older = key(QStringLiteral("older"), false, 0, 1000);
    const FileSortKey newer = key(QStringLiteral("newer"), false, 0, 2000);
    CHECK(fileTreeSortLess(2, S::AscendingOrder, older, newer));
    CHECK(fileTreeSortLess(2, S::DescendingOrder, newer, older));

    // 大小平局以名称裁决（结果稳定，不依赖插入顺序）。
    const FileSortKey a = key(QStringLiteral("a"), false, 100);
    const FileSortKey b = key(QStringLiteral("b"), false, 100);
    CHECK(fileTreeSortLess(1, S::AscendingOrder, a, b));
    CHECK(!fileTreeSortLess(1, S::AscendingOrder, b, a));
}

static void testTextColumns()
{
    using S = Qt::SortOrder;
    const FileSortKey x = key(QStringLiteral("x"), false, 0, 0,
                              QStringLiteral("-rw-r--r--"), QStringLiteral("alice"));
    const FileSortKey y = key(QStringLiteral("y"), false, 0, 0,
                              QStringLiteral("-rwxr-xr-x"), QStringLiteral("bob"));
    CHECK(fileTreeSortLess(3, S::AscendingOrder, x, y));
    CHECK(fileTreeSortLess(4, S::AscendingOrder, x, y));
    CHECK(fileTreeSortLess(4, S::DescendingOrder, y, x));
}

// 端到端：模拟 populate 的条目创建（含裸值 role），验证 sortFileTree 对
// 大小/日期列的真实重排——钉住"role 没存上 → 按这两列排了等于没排"这类
// 隐蔽回归（全部 0 值平局后退化为名称序，用户看到的就是"点了没反应"）。
static void testSortFileTreeEndToEnd()
{
    QTreeWidget tree;
    tree.setColumnCount(5);
    const auto add = [&tree](const QString &name, bool isDir, qint64 size, qint64 mtime) {
        auto *item = new QTreeWidgetItem(&tree);
        item->setText(0, name);
        item->setData(0, kIsDirRole, isDir);
        item->setData(0, kSizeRole, size);
        item->setData(0, kMtimeRole, mtime);
        return item;
    };
    add(QStringLiteral(".."), true, 0, 0)->setData(0, kIsUpRole, true);
    add(QStringLiteral("big.bin"), false, 1 << 30, 100);
    add(QStringLiteral("mid.bin"), false, 1 << 20, 300);
    add(QStringLiteral("small.bin"), false, 100, 200);
    // 一个目录混在里面：大小/日期列应全列单调混排（不提前），名称列才提前。
    add(QStringLiteral("zdir"), true, 50, 250);

    // 大小升序：".." 置顶，其后 zdir(50) → small(100) → mid → big，全列单调
    sortFileTree(&tree, 1, Qt::AscendingOrder);
    CHECK(tree.topLevelItem(0)->text(0) == QStringLiteral(".."));
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("zdir"));
    CHECK(tree.topLevelItem(2)->text(0) == QStringLiteral("small.bin"));
    CHECK(tree.topLevelItem(3)->text(0) == QStringLiteral("mid.bin"));
    CHECK(tree.topLevelItem(4)->text(0) == QStringLiteral("big.bin"));

    // 大小降序：".." 仍置顶，big 排最前
    sortFileTree(&tree, 1, Qt::DescendingOrder);
    CHECK(tree.topLevelItem(0)->text(0) == QStringLiteral(".."));
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("big.bin"));
    CHECK(tree.topLevelItem(4)->text(0) == QStringLiteral("zdir"));

    // 日期升序：100(big) → 200(small) → 250(zdir) → 300(mid)，目录不提前
    sortFileTree(&tree, 2, Qt::AscendingOrder);
    CHECK(tree.topLevelItem(0)->text(0) == QStringLiteral(".."));
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("big.bin"));
    CHECK(tree.topLevelItem(2)->text(0) == QStringLiteral("small.bin"));
    CHECK(tree.topLevelItem(3)->text(0) == QStringLiteral("zdir"));
    CHECK(tree.topLevelItem(4)->text(0) == QStringLiteral("mid.bin"));

    // 日期降序
    sortFileTree(&tree, 2, Qt::DescendingOrder);
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("mid.bin"));
    CHECK(tree.topLevelItem(4)->text(0) == QStringLiteral("big.bin"));

    // 名称升序：目录提前（zdir 排在所有文件之前）
    sortFileTree(&tree, 0, Qt::AscendingOrder);
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("zdir"));
    CHECK(tree.topLevelItem(2)->text(0) == QStringLiteral("big.bin"));
}

// 端到端：applyFileFilter 隐藏不匹配项，".." 在有筛选词时隐藏，清空后还原。
static void testApplyFileFilterEndToEnd()
{
    QTreeWidget tree;
    tree.setColumnCount(5);
    const auto add = [&tree](const QString &name, bool isUp = false) {
        auto *item = new QTreeWidgetItem(&tree);
        item->setText(0, name);
        item->setData(0, kIsUpRole, isUp);
        return item;
    };
    add(QStringLiteral(".."), true);
    add(QStringLiteral("nginx.conf"));
    add(QStringLiteral("app.log"));
    add(QStringLiteral("readme.md"));

    int visible = applyFileFilter(&tree, QStringLiteral("log"));
    CHECK(visible == 1);
    CHECK(tree.topLevelItem(0)->isHidden());               // ".." 随筛选隐藏
    CHECK(tree.topLevelItem(1)->isHidden());               // nginx.conf
    CHECK(!tree.topLevelItem(2)->isHidden());              // app.log
    CHECK(tree.topLevelItem(3)->isHidden());               // readme.md

    visible = applyFileFilter(&tree, QString());
    CHECK(visible == 3);
    for (int i = 0; i < tree.topLevelItemCount(); ++i)
        CHECK(!tree.topLevelItem(i)->isHidden());
}

int main(int argc, char *argv[])
{
    // sortFileTree/applyFileFilter 要建 QTreeWidget，需要 QApplication；
    // offscreen 平台保证无头 CI 可跑（与其他 widget 相关单测同款）。
    QApplication app(argc, argv);
    testMatchesFilter();
    testNameSortColumn();
    testDirsFirstOnNameColumn();
    testNonNameColumnsAreGloballyMonotonic();
    testNumericColumns();
    testTextColumns();
    testSortFileTreeEndToEnd();
    testApplyFileFilterEndToEnd();
    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "file_browser_test: all checks passed";
    return 0;
}
