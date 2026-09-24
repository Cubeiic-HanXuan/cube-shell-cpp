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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

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

// 拖放上传的目标目录：落在目录条目上进该目录（".." 条目的路径即上级目录），
// 其余落点（空白/文件条目）进当前目录。
static void testDropTargetDir()
{
    using W = SftpBrowserWidget;
    // 目录条目 → 该目录
    CHECK(W::dropTargetDir(QStringLiteral("/var/log"), true, QStringLiteral("/var"))
          == QStringLiteral("/var/log"));
    // ".." 条目：kPathRole 已存上级路径、isDir=true，天然覆盖
    CHECK(W::dropTargetDir(QStringLiteral("/"), true, QStringLiteral("/var"))
          == QStringLiteral("/"));
    // 文件条目 → 当前目录
    CHECK(W::dropTargetDir(QStringLiteral("/var/a.log"), false, QStringLiteral("/var"))
          == QStringLiteral("/var"));
    // 空白处（无条目）→ 当前目录
    CHECK(W::dropTargetDir(QString(), false, QStringLiteral("/data"))
          == QStringLiteral("/data"));
}

// 拖入本地路径的任务展开：文件直接映射；文件夹递归且远端路径保留目录结构。
static void testCollectUploadTasks()
{
    using W = SftpBrowserWidget;
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();

    // 造结构：root/a.txt、root/dir/b.txt、root/dir/sub/c.txt、root/empty/
    auto writeFile = [&root](const QString &rel) {
        const QString path = root + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write("x") > 0;
    };
    CHECK(writeFile(QStringLiteral("a.txt")));
    CHECK(writeFile(QStringLiteral("dir/b.txt")));
    CHECK(writeFile(QStringLiteral("dir/sub/c.txt")));
    CHECK(QDir().mkpath(root + QStringLiteral("/empty")));

    // 单个文件
    QList<QPair<QString, QString>> tasks =
        W::collectUploadTasks(QStringLiteral("/data"), {root + QStringLiteral("/a.txt")});
    CHECK(tasks.size() == 1);
    CHECK(tasks.at(0).second == QStringLiteral("/data/a.txt"));

    // 目标目录带尾斜杠不双斜杠；根目标不三斜杠
    tasks = W::collectUploadTasks(QStringLiteral("/data/"), {root + QStringLiteral("/a.txt")});
    CHECK(tasks.at(0).second == QStringLiteral("/data/a.txt"));
    tasks = W::collectUploadTasks(QStringLiteral("/"), {root + QStringLiteral("/a.txt")});
    CHECK(tasks.at(0).second == QStringLiteral("/a.txt"));

    // 文件夹递归：远端 = 目标目录 + 顶层文件夹名 + 相对路径
    tasks = W::collectUploadTasks(QStringLiteral("/data"), {root + QStringLiteral("/dir")});
    CHECK(tasks.size() == 2);
    QStringList remotes;
    for (const auto &t : tasks)
        remotes.append(t.second);
    CHECK(remotes.contains(QStringLiteral("/data/dir/b.txt")));
    CHECK(remotes.contains(QStringLiteral("/data/dir/sub/c.txt")));

    // 空文件夹不产生任务
    tasks = W::collectUploadTasks(QStringLiteral("/data"), {root + QStringLiteral("/empty")});
    CHECK(tasks.isEmpty());

    // 不存在的路径不产生任务
    tasks = W::collectUploadTasks(QStringLiteral("/data"),
                                  {root + QStringLiteral("/no-such")});
    CHECK(tasks.isEmpty());

    // 混合：文件 + 文件夹
    tasks = W::collectUploadTasks(QStringLiteral("/data"),
                                  {root + QStringLiteral("/a.txt"),
                                   root + QStringLiteral("/dir")});
    CHECK(tasks.size() == 3);
}

// 「执行脚本」菜单项的显示判定：只认扩展名白名单。
// 这条真值表就是那一项的安全边界——放宽成"有可执行位就算脚本"，用户右键
// /usr/bin 下任何二进制都会看到"执行脚本"，误点即在远端把它跑起来。
static void testScriptDetection()
{
    using W = SftpBrowserWidget;

    // 白名单命中（扩展名大小写不敏感）
    CHECK(W::looksLikeScript(QStringLiteral("deploy.sh"), false));
    CHECK(W::looksLikeScript(QStringLiteral("backup.py"), false));
    CHECK(W::looksLikeScript(QStringLiteral("run.BASH"), false));
    CHECK(W::looksLikeScript(QStringLiteral("x.ZSH"), false));
    CHECK(W::looksLikeScript(QStringLiteral("hook.pl"), false));
    CHECK(W::looksLikeScript(QStringLiteral("app.js"), false));
    // 多点文件名取最后一段扩展名
    CHECK(W::looksLikeScript(QStringLiteral("release.v2.sh"), false));

    // 目录永不命中，哪怕名字带脚本后缀（scripts.sh/ 这种目录真实存在）
    CHECK(!W::looksLikeScript(QStringLiteral("scripts.sh"), true));

    // 无扩展名不命中：可执行位不参与判定，编译出来的二进制（deploy、ls）
    // 不该冒出"执行脚本"。
    CHECK(!W::looksLikeScript(QStringLiteral("deploy"), false));
    CHECK(!W::looksLikeScript(QStringLiteral("ls"), false));

    // 白名单外的扩展名
    CHECK(!W::looksLikeScript(QStringLiteral("notes.txt"), false));
    CHECK(!W::looksLikeScript(QStringLiteral("libfoo.so"), false));
    CHECK(!W::looksLikeScript(QStringLiteral("app.exe"), false));
    CHECK(!W::looksLikeScript(QStringLiteral("a.sh.bak"), false));

    // 隐藏文件的前导点不是扩展名分隔符：".sh" 是个没有扩展名的文件
    CHECK(!W::looksLikeScript(QStringLiteral(".sh"), false));
    CHECK(!W::looksLikeScript(QStringLiteral(".bashrc"), false));
    // 以点结尾 / 空名
    CHECK(!W::looksLikeScript(QStringLiteral("run."), false));
    CHECK(!W::looksLikeScript(QString(), false));
}

// 脚本 → 终端命令行：有可执行位直跑（走 shebang），否则按扩展名补解释器。
static void testScriptRunCommand()
{
    using W = SftpBrowserWidget;

    // 置了可执行位 → 只有路径，解释器交给脚本自己的 shebang
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/app/deploy.sh"), 0755, false)
          == QStringLiteral("/opt/app/deploy.sh"));
    // 只有 others 的 x 位也算（登录用户可能正是 other）
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/a.py"), 0001, false)
          == QStringLiteral("/opt/a.py"));

    // 没有可执行位 → 补解释器，避免 "Permission denied"
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/app/deploy.sh"), 0644, false)
          == QStringLiteral("bash /opt/app/deploy.sh"));
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/app/backup.py"), 0644, false)
          == QStringLiteral("python3 /opt/app/backup.py"));
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/x.rb"), 0400, false)
          == QStringLiteral("ruby /opt/x.rb"));

    // 符号链接不直跑：链接自身权限位恒为 rwxrwxrwx，说明不了目标能不能执行
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/link.sh"), 0777, true)
          == QStringLiteral("bash /opt/link.sh"));

    // 路径转义（FileUtil::shellQuote / shlex.quote 语义：安全字符原样，
    // 含空格或单引号才整体包裹）
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/my app/a b.sh"), 0644, false)
          == QStringLiteral("bash '/opt/my app/a b.sh'"));
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/my app/run.sh"), 0755, false)
          == QStringLiteral("'/opt/my app/run.sh'"));
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/it's/a.sh"), 0644, false)
          == QStringLiteral("bash '/opt/it'\"'\"'s/a.sh'"));
    // 分号/反引号等注入字符也被包进单引号，不会被远端 shell 当成命令分隔
    CHECK(W::scriptRunCommand(QStringLiteral("/opt/a;rm -rf x.sh"), 0644, false)
          == QStringLiteral("bash '/opt/a;rm -rf x.sh'"));
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
    testDropTargetDir();
    testCollectUploadTasks();
    testScriptDetection();
    testScriptRunCommand();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
