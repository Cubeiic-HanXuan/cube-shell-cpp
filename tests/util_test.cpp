// Util module unit test: StringUtil formatting/parsing, FileUtil traversal +
// compress/decompress round-trip (QTemporaryDir), DataParser sample outputs,
// ThemeManager loading the real conf/theme.json and producing non-empty QSS.
// Style mirrors tests/config_store_test.cpp (no gtest, main + CHECK macro).

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "util/DataParser.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"
#include "util/ThemeManager.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testStringUtil()
{
    // format_file_size: 字节 / KB / MB / GB(3位小数)
    CHECK(StringUtil::formatFileSize(512) == QStringLiteral("512 字节"));
    CHECK(StringUtil::formatFileSize(2048) == QStringLiteral("2.00 KB"));
    CHECK(StringUtil::formatFileSize(5 * 1024 * 1024) == QStringLiteral("5.00 MB"));
    CHECK(StringUtil::formatFileSize(3LL * 1024 * 1024 * 1024) == QStringLiteral("3.000 GB"));

    // format_speed
    CHECK(StringUtil::formatSpeed(500) == QStringLiteral("500 B/s"));
    CHECK(StringUtil::formatSpeed(2048) == QStringLiteral("2.00 KB/s"));
    CHECK(StringUtil::formatSpeed(3.5 * 1024 * 1024) == QStringLiteral("3.50 MB/s"));

    // has_valid_suffix
    CHECK(StringUtil::hasValidSuffix(QStringLiteral("backup.tar.gz")));
    CHECK(StringUtil::hasValidSuffix(QStringLiteral("app.jar")));
    CHECK(!StringUtil::hasValidSuffix(QStringLiteral("notes.txt")));

    // remove_special_lines: 去掉纯波浪号行与空行，保留正常行
    const QString filtered = StringUtil::removeSpecialLines(
        QStringLiteral("hello\n~\n~~~ \n  \nworld ~"));
    CHECK(filtered == QStringLiteral("hello\nworld ~"));

    // is_ipv6_address
    CHECK(StringUtil::isIpv6Address(QStringLiteral("fdb2:2c26::bc9c")));
    CHECK(StringUtil::isIpv6Address(QStringLiteral("[fd00::1]")));
    CHECK(!StringUtil::isIpv6Address(QStringLiteral("192.168.1.1")));
    CHECK(!StringUtil::isIpv6Address(QStringLiteral("example.com")));

    // symbolic_to_octal
    CHECK(StringUtil::symbolicToOctal(QStringLiteral("rwxr-xr-x")) == 755);
    CHECK(StringUtil::symbolicToOctal(QStringLiteral("rw-r--r--")) == 644);
    CHECK(StringUtil::symbolicToOctal(QStringLiteral("---------")) == 0);

    // joinPath / baseName
    CHECK(StringUtil::joinPath(QStringLiteral("/tmp/dir"), QStringLiteral("a.txt"))
          == QStringLiteral("/tmp/dir/a.txt"));
    CHECK(StringUtil::joinPath(QStringLiteral("/tmp/dir"), QStringLiteral("/abs/b.txt"))
          == QStringLiteral("/abs/b.txt"));
    CHECK(StringUtil::baseName(QStringLiteral("/etc/conf/theme.json"))
          == QStringLiteral("theme.json"));

    // banner / appName 常量
    CHECK(StringUtil::appName() == QStringLiteral("cube-shell"));
    CHECK(StringUtil::banner().contains(QStringLiteral("cube-shell")));
}

// 在 dir 下创建 files 中的相对路径文件（自动补齐父目录）
static void makeTree(const QString &dir, const QStringList &files)
{
    for (const QString &rel : files) {
        const QString abs = dir + QLatin1Char('/') + rel;
        QDir().mkpath(QFileInfo(abs).absolutePath());
        QFile f(abs);
        f.open(QIODevice::WriteOnly);
        f.write(rel.toUtf8()); // 内容用相对路径本身，便于校验
        f.close();
    }
}

static void testFileUtil()
{
    // shlex.quote 语义
    CHECK(FileUtil::shellQuote(QStringLiteral("plain-name.txt")) == QStringLiteral("plain-name.txt"));
    CHECK(FileUtil::shellQuote(QStringLiteral("a b")) == QStringLiteral("'a b'"));
    CHECK(FileUtil::shellQuote(QString()) == QStringLiteral("''"));
    CHECK(FileUtil::shellQuote(QStringLiteral("it's")) == QStringLiteral("'it'\"'\"'s'"));

    // isPathWithinBase（zip-slip 防护）
    CHECK(FileUtil::isPathWithinBase(QStringLiteral("/tmp/dest"), QStringLiteral("sub/a.txt")));
    CHECK(!FileUtil::isPathWithinBase(QStringLiteral("/tmp/dest"), QStringLiteral("../evil.txt")));
    CHECK(!FileUtil::isPathWithinBase(QStringLiteral("/tmp/dest"), QStringLiteral("/etc/passwd")));

    // frp 配置模板
    const QString frpc = FileUtil::frpcConfig(QStringLiteral("1.2.3.4"),
                                              QStringLiteral("tok"), QStringLiteral("TCP"),
                                              8080, 9090);
    CHECK(frpc.contains(QStringLiteral("serverAddr = \"1.2.3.4\"")));
    CHECK(frpc.contains(QStringLiteral("auth.token = \"tok\"")));
    CHECK(frpc.contains(QStringLiteral("type = \"tcp\"")));
    CHECK(frpc.contains(QStringLiteral("remotePort = 9090")));
    const QString httpProxy = FileUtil::proxyConfig(QStringLiteral("http"), 80, 0,
                                                    QStringLiteral("example.com"));
    CHECK(httpProxy.contains(QStringLiteral("customDomains = [\"example.com\"]")));
    const QString frps = FileUtil::frpsConfig(QStringLiteral("tok"), QStringLiteral("http"), 8080);
    CHECK(frps.contains(QStringLiteral("bindPort = 7000")));
    CHECK(frps.contains(QStringLiteral("vhostHTTPPort = 8080")));
    CHECK(!FileUtil::frpsConfig(QStringLiteral("tok")).contains(QStringLiteral("vhostHTTPPort")));

    // 目录遍历 + tar.gz 压缩/解压往返
    QTemporaryDir srcDir;
    CHECK(srcDir.isValid());
    const QStringList tree = {
        QStringLiteral("a.txt"),
        QStringLiteral("sub/b.txt"),
        QStringLiteral("sub/deep/c.log"),
    };
    makeTree(srcDir.path(), tree);

    QStringList listed = FileUtil::listFilesRecursive(srcDir.path());
    CHECK(listed.size() == 3);
    CHECK(listed.contains(QStringLiteral("a.txt")));
    CHECK(listed.contains(QStringLiteral("sub/deep/c.log")));

    const FileUtil::ArchiveResult packed = FileUtil::compress(
        srcDir.path(), {QStringLiteral("a.txt"), QStringLiteral("sub")},
        QStringLiteral("bundle.tar.gz"), FileUtil::ArchiveFormat::TarGz);
    CHECK(packed.success);
    CHECK(QFile::exists(srcDir.path() + QStringLiteral("/bundle.tar.gz")));

    QTemporaryDir destDir;
    CHECK(destDir.isValid());
    const FileUtil::ArchiveResult unpacked = FileUtil::decompress(
        srcDir.path() + QStringLiteral("/bundle.tar.gz"), destDir.path());
    CHECK(unpacked.success);
    for (const QString &rel : tree) {
        QFile f(destDir.path() + QLatin1Char('/') + rel);
        CHECK(f.exists());
        if (f.open(QIODevice::ReadOnly))
            CHECK(f.readAll() == rel.toUtf8()); // 内容往返一致
    }

    // 不支持的格式报错（与 Python 消息语义一致）
    const FileUtil::ArchiveResult bad = FileUtil::decompress(
        srcDir.path() + QStringLiteral("/a.txt"), destDir.path());
    CHECK(!bad.success);
    CHECK(bad.message.contains(QStringLiteral("Unsupported archive format")));
}

static void testDataParser()
{
    // /proc/net/dev 样例（lo 应被跳过）
    const QString netOut = QStringLiteral(
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
        "  eth0: 5000 50 1 2 0 0 0 0 3000 30 4 5 0 0 0 0\n");
    const auto net = DataParser::parseNetworkData(netOut);
    CHECK(net.size() == 1);
    CHECK(net.contains(QStringLiteral("eth0")));
    CHECK(net[QStringLiteral("eth0")].rxBytes == 5000);
    CHECK(net[QStringLiteral("eth0")].txBytes == 3000);
    CHECK(net[QStringLiteral("eth0")].txErrors == 4);

    // 两次快照 → 速率
    auto prev = net;
    auto curr = net;
    curr[QStringLiteral("eth0")].rxBytes += 2048;
    curr[QStringLiteral("eth0")].txBytes += 1024;
    const auto speeds = DataParser::calculateNetworkSpeed(prev, curr, 2.0);
    CHECK(speeds.size() == 1);
    CHECK(qFuzzyCompare(speeds[0].rxSpeed, 1024.0));
    CHECK(qFuzzyCompare(speeds[0].txSpeed, 512.0));
    DataParser::NetworkInterfaceStat main;
    CHECK(DataParser::getMainInterface(speeds, &main));
    CHECK(main.name == QStringLiteral("eth0"));

    // /proc/stat 样例 + CPU 使用率
    const QString statT0 = QStringLiteral(
        "cpu  100 0 100 800 0 0 0 0\n"
        "cpu0 50 0 50 400 0 0 0 0\n"
        "cpu1 50 0 50 400 0 0 0 0\n");
    const QString statT1 = QStringLiteral(
        "cpu  200 0 200 1600 0 0 0 0\n"
        "cpu0 100 0 100 800 0 0 0 0\n"
        "cpu1 100 0 100 800 0 0 0 0\n");
    const auto cpu0 = DataParser::parseCpuData(statT0);
    const auto cpu1 = DataParser::parseCpuData(statT1);
    CHECK(cpu0.isValid() && cpu0.cores.size() == 2);
    const auto usage = DataParser::calculateCpuUsage(cpu0, cpu1);
    // delta: total=1000 idle=800 → 20% 使用率；user=10%；system=10%
    CHECK(qAbs(usage.totalUsage - 20.0) < 0.01);
    CHECK(qAbs(usage.userUsage - 10.0) < 0.01);
    CHECK(qAbs(usage.systemUsage - 10.0) < 0.01);
    CHECK(usage.coresUsage.size() == 2);

    // parse_size_value（返回 MB）
    CHECK(qFuzzyCompare(DataParser::parseSizeValue(QStringLiteral("512")), 512.0));
    CHECK(qFuzzyCompare(DataParser::parseSizeValue(QStringLiteral("2G")), 2048.0));
    CHECK(qFuzzyCompare(DataParser::parseSizeValue(QStringLiteral("1024K")), 1.0));
    CHECK(qFuzzyCompare(DataParser::parseSizeValue(QStringLiteral("1T")), 1024.0 * 1024.0));
    CHECK(qFuzzyCompare(DataParser::parseSizeValue(QStringLiteral("550M")), 550.0));
    CHECK(DataParser::parseSizeValue(QStringLiteral("garbage")) == 0.0);

    // df 样例（tmpfs 应被过滤）
    const QString dfOut = QStringLiteral(
        "Filesystem      Size  Used Avail Use% Mounted on\n"
        "/dev/vda1        50G   20G   28G  42% /\n"
        "tmpfs           7.8G     0  7.8G   0% /dev/shm\n");
    const auto disks = DataParser::parseDiskData(dfOut);
    CHECK(disks.size() == 1);
    CHECK(disks[0].filesystem == QStringLiteral("/dev/vda1"));
    CHECK(qFuzzyCompare(disks[0].usagePercent, 42.0));
    CHECK(disks[0].mountPoint == QStringLiteral("/"));

    // uptime 负载
    const auto load = DataParser::parseLoadAverage(
        QStringLiteral("10:00 up 3 days, 2 users, load average: 0.52, 0.58, 0.59"));
    CHECK(load.size() == 3);
    CHECK(qFuzzyCompare(load[0], 0.52) && qFuzzyCompare(load[2], 0.59));
    CHECK(DataParser::parseLoadAverage(QStringLiteral("no match")) == QList<double>({0.0, 0.0, 0.0}));

    // free -m 样例
    const QString freeOut = QStringLiteral(
        "              total        used        free      shared  buff/cache   available\n"
        "Mem:           8000        4000         500         300        3500        3600\n"
        "Swap:          2048           0        2048\n");
    const auto mem = DataParser::parseMemoryData(freeOut);
    CHECK(qFuzzyCompare(mem.total, 8000.0));
    CHECK(qFuzzyCompare(mem.available, 3600.0));
    CHECK(qAbs(mem.usagePercent - 55.0) < 0.01); // (8000-3600)/8000*100

    // hostnamectl 样例
    const auto info = DataParser::parseHostnamectlOutput(QStringLiteral(
        " Static hostname: web-01\n Operating System: Ubuntu 22.04.3 LTS\n"));
    CHECK(info.value(QStringLiteral("Static hostname")) == QStringLiteral("web-01"));
    CHECK(info.value(QStringLiteral("Operating System")).startsWith(QStringLiteral("Ubuntu")));
}

static void testThemeManager()
{
    // 加载仓库里的真实 conf/theme.json。用 __FILE__ 定位源码树
    // （本文件在 tests/ 下，theme.json 在 ../conf/theme.json），
    // 与测试可执行文件的输出目录无关。
    const QString themePath = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                                  .filePath(QStringLiteral("../conf/theme.json"));
    CHECK(QFile::exists(themePath));
    ThemeManager mgr;
    QString err;
    CHECK(mgr.load(themePath, &err));
    if (!err.isEmpty())
        qWarning() << "theme load error:" << err;
    CHECK(mgr.fontFamily() == QStringLiteral("Monaco"));
    CHECK(mgr.fontSize() == 14);
    CHECK(mgr.appearance() == ThemeManager::Appearance::Dark);
    CHECK(mgr.terminalTheme() == QStringLiteral("Tango"));
    CHECK(mgr.language() == QStringLiteral("zh_CN"));

    // 枚举/查询接口
    CHECK(ThemeManager::availableThemes().contains(QStringLiteral("dark")));
    CHECK(ThemeManager::appearanceFromString(QStringLiteral("LIGHT")) == ThemeManager::Appearance::Light);
    CHECK(ThemeManager::appearanceFromString(QStringLiteral("anything")) == ThemeManager::Appearance::Dark);
    CHECK(ThemeManager::appearanceToString(ThemeManager::Appearance::Light) == QStringLiteral("light"));

    // 生成非空 QSS，且 primary 色与 Python custom_colors 一致。
    // qdarktheme 导出的 qss 里颜色统一为 rgba() 形式：
    // #00A1FF -> rgba(0, 161, 255, 1.000)，#E05B00 -> rgba(224, 91, 0, 1.000)
    const QString darkQss = ThemeManager::styleSheet(ThemeManager::Appearance::Dark);
    CHECK(!darkQss.isEmpty());
    CHECK(darkQss.contains(QStringLiteral("rgba(0, 161, 255, 1.000)")));
    const QString lightQss = ThemeManager::styleSheet(ThemeManager::Appearance::Light);
    CHECK(!lightQss.isEmpty());
    CHECK(lightQss.contains(QStringLiteral("rgba(224, 91, 0, 1.000)")));
    CHECK(darkQss != lightQss);
    // url 已全部改写为 Qt 资源路径，不应残留导出机器上的绝对缓存路径
    CHECK(darkQss.contains(QStringLiteral("url(\":/qdarktheme/svg/")));
    CHECK(!darkQss.contains(QStringLiteral("/.cache/qdarktheme")));

    // palette 主色
    CHECK(ThemeManager::palette(ThemeManager::Appearance::Dark)
              .color(QPalette::Highlight) == QColor(QStringLiteral("#00A1FF")));

    // save 往返（写入临时副本，不动仓库文件）
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString copyPath = tmp.filePath(QStringLiteral("theme.json"));
    CHECK(QFile::copy(themePath, copyPath));
    ThemeManager rw;
    CHECK(rw.load(copyPath));
    rw.setAppearance(ThemeManager::Appearance::Light);
    rw.setFont(QStringLiteral("Menlo"), 16);
    CHECK(rw.save());
    ThemeManager reloaded;
    CHECK(reloaded.load(copyPath));
    CHECK(reloaded.appearance() == ThemeManager::Appearance::Light);
    CHECK(reloaded.fontFamily() == QStringLiteral("Menlo"));
    CHECK(reloaded.fontSize() == 16);
    // 原有键不丢失（格式保持不变）
    CHECK(reloaded.terminalTheme() == QStringLiteral("Tango"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testStringUtil();
    testFileUtil();
    testDataParser();
    testThemeManager();
    qInfo() << (failures == 0 ? "ALL PASS" : "FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
