// SFTP 面板「代理端不提供文件浏览」判定的单测（纯逻辑，无网络、无显示）。
//
// 回归的是这条真实故障：jms:// 连上 JumpServer 后，终端跑在资产上、OSC 7 报
// cwd = /config，而 SFTP 子系统由 koko 自己提供（一套与资产无关的虚拟命名空间，
// 用 connection token 登录时连根目录都是空的）。面板照着终端 cwd 去 opendir
// 必然失败，此前把 libssh2 的原始串 "opendir failed: /config: Failed opening
// remote file" 直接糊在了 UI 上。
//
// 这里钉住判定的真值表，防止把条件放宽成"根目录空就报不可用"——普通主机上
// 一个真的空目录会被误判，或者反过来收紧到永不命中。
// 对应Python: cube-shell.py 的 is_jumpserver_proxy + _file_tree_unavailable

#include <QCoreApplication>
#include <QDebug>
#include <QPair>
#include <QString>
#include <QStringList>

#include "sftp_browser_widget.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 真值表：bastionProxied && path == "/" && entryCount == 0
static void testUnavailableTruthTable()
{
    const QString root = QStringLiteral("/");

    // 唯一命中：经代理 + 根目录 + 空。这就是 koko 的实测表现。
    CHECK(SftpBrowserWidget::sftpLooksUnavailable(true, root, 0));

    // 三个维度各自否定，都不该命中。
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(false, root, 0));  // 普通主机的空根目录
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(true, root, 1));   // 代理端列出了东西
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(true, QStringLiteral("/config"), 0));
}

// 非根路径列不出来不足以推断整条通道不可用——可能只是权限不足或路径不存在。
// 若把这条放宽，普通主机上进一个空目录/无权目录就会被判成"SFTP 不可用"，
// 用户从此浏览不了任何文件。
static void testNonRootNeverTriggers()
{
    for (const QString &path : {QStringLiteral("/config"), QStringLiteral("/tmp"),
                                QStringLiteral("/home/testuser"), QStringLiteral("")})
        CHECK(!SftpBrowserWidget::sftpLooksUnavailable(true, path, 0));

    // 尾部斜杠形态也不算根：loadPath 收到的路径已由 setCurrentPath 去过尾部
    // 斜杠，这里顺带钉住"只认恰好等于 /"的严格比较。
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(true, QStringLiteral("//"), 0));
}

// 未经代理的连接永不进入不可用态：普通主机的 SFTP 是真的可用，
// 空目录只是空目录。这条是防止改动把正常 SFTP 浏览弄坏的闸门。
static void testPlainHostUnaffected()
{
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(false, QStringLiteral("/"), 0));
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(false, QStringLiteral("/"), 42));
    CHECK(!SftpBrowserWidget::sftpLooksUnavailable(false, QStringLiteral("/config"), 0));
}

// 批量下载的选中项拆分：文件进下载队列，目录单独收集（由 UI 统一提示跳过）。
static void testPartitionDownloadSelection()
{
    QStringList files, dirs;

    // 空选
    SftpBrowserWidget::partitionDownloadSelection({}, files, dirs);
    CHECK(files.isEmpty() && dirs.isEmpty());

    // 纯文件
    SftpBrowserWidget::partitionDownloadSelection(
        {{QStringLiteral("/a.log"), false}, {QStringLiteral("/b.log"), false}}, files, dirs);
    CHECK(files.size() == 2 && dirs.isEmpty());

    // 纯目录
    SftpBrowserWidget::partitionDownloadSelection(
        {{QStringLiteral("/etc"), true}}, files, dirs);
    CHECK(files.isEmpty() && dirs.size() == 1);

    // 混合：文件与目录各自归组，顺序保持
    SftpBrowserWidget::partitionDownloadSelection(
        {{QStringLiteral("/etc"), true},
         {QStringLiteral("/a.log"), false},
         {QStringLiteral("/var"), true},
         {QStringLiteral("/b.log"), false}}, files, dirs);
    CHECK(files == QStringList({QStringLiteral("/a.log"), QStringLiteral("/b.log")}));
    CHECK(dirs == QStringList({QStringLiteral("/etc"), QStringLiteral("/var")}));

    // 空路径条目（约定上游已剔除 ".."，这里钉住空串不进组）
    SftpBrowserWidget::partitionDownloadSelection(
        {{QString(), false}, {QStringLiteral("/a.log"), false}}, files, dirs);
    CHECK(files.size() == 1 && dirs.isEmpty());
}

// 批量下载的目标本地路径生成：保存目录 + 远端文件名。
static void testDownloadTargetPath()
{
    using W = SftpBrowserWidget;
    CHECK(W::downloadTargetPath(QStringLiteral("/tmp"), QStringLiteral("/var/log/a.log"))
          == QStringLiteral("/tmp/a.log"));
    // 目标目录带尾斜杠不双斜杠
    CHECK(W::downloadTargetPath(QStringLiteral("/tmp/"), QStringLiteral("/var/log/a.log"))
          == QStringLiteral("/tmp/a.log"));
    // 远端深层路径只取文件名
    CHECK(W::downloadTargetPath(QStringLiteral("/tmp"), QStringLiteral("/a/b/c.txt"))
          == QStringLiteral("/tmp/c.txt"));
    // 根目录目标
    CHECK(W::downloadTargetPath(QStringLiteral("/"), QStringLiteral("/a.log"))
          == QStringLiteral("/a.log"));
}

int main(int argc, char *argv[])
{
    // 判定函数是静态纯函数，不建任何 widget，因此用 QCoreApplication 就够
    // （与 command_suggest_test 同款：链 Qt6::Widgets 但不需要显示环境，
    // 无头 CI 上也能跑）。
    QCoreApplication app(argc, argv);
    testUnavailableTruthTable();
    testNonRootNeverTriggers();
    testPlainHostUnaffected();
    testPartitionDownloadSelection();
    testDownloadTargetPath();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
