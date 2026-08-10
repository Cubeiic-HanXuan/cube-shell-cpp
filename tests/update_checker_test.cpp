// update_checker_test.cpp — 更新检查的纯逻辑单元测试（不发起任何网络请求）。
//
// 覆盖：
//   (a) 仓库地址指向 C++ 版 cube-shell-cpp，而非 Python 版 cube-shell
//   (b) 版本号解析/比较：V 前缀、位数缺省、预发布排序
//   (c) 同版本不误报更新（本机 3.2.0 对上游 V3.2.0 → 无更新）
//   (d) parseReleaseJson：tag/assets 提取与 html_url 兜底
//   (e) selectAsset：按当前平台从真实发布物命名中挑出唯一 asset，
//       且绝不跨架构错配（宁可返回空走 Release 页兜底）

#include <QCoreApplication>
#include <QDebug>

#include "update/UpdateChecker.h"

using namespace cubeshell;

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

// --- (a) 仓库地址 -----------------------------------------------------------
//
// 老地址 .../repos/Cubeiic-HanXuan/cube-shell/... 是 Python 版仓库，最新 tag
// 停在 V2.8.0。指过去会让 3.x 用户被推去下 2.8.0 的包，正是这次要修的问题。
static void testRepoUrlsPointToCppRepo()
{
    const QString api = QString::fromLatin1(UpdateChecker::kGitHubApi);
    const QString page = QString::fromLatin1(UpdateChecker::kReleasePage);

    TEST_ASSERT(api.contains(QLatin1String("/Cubeiic-HanXuan/cube-shell-cpp/")),
                "API 地址指向 cube-shell-cpp");
    TEST_ASSERT(page.contains(QLatin1String("/Cubeiic-HanXuan/cube-shell-cpp/")),
                "Release 页地址指向 cube-shell-cpp");
    // 不能再以 Python 仓库路径结尾（cube-shell 是 cube-shell-cpp 的前缀，
    // 只能用完整分段判断，不能用 contains("cube-shell")）。
    TEST_ASSERT(!api.contains(QLatin1String("/cube-shell/releases")),
                "API 地址不再指向 Python 版仓库");
    TEST_ASSERT(!page.contains(QLatin1String("/cube-shell/releases")),
                "Release 页地址不再指向 Python 版仓库");
    TEST_ASSERT(api.startsWith(QLatin1String("https://")), "API 走 https");
}

// --- (b) 版本号解析与比较 ---------------------------------------------------
static void testVersionParsing()
{
    SemVer v = UpdateChecker::parseVersion(QStringLiteral("V3.2.0"));
    TEST_ASSERT(v.valid, "大写 V 前缀可解析");
    TEST_ASSERT_EQ(v.major, 3, "major");
    TEST_ASSERT_EQ(v.minor, 2, "minor");
    TEST_ASSERT_EQ(v.patch, 0, "patch");

    v = UpdateChecker::parseVersion(QStringLiteral("v3.2"));
    TEST_ASSERT(v.valid, "省略 patch 可解析");
    TEST_ASSERT_EQ(v.patch, 0, "缺省 patch 补 0");

    v = UpdateChecker::parseVersion(QStringLiteral("  3.2.1  "));
    TEST_ASSERT(v.valid, "首尾空白被裁掉");
    TEST_ASSERT_EQ(v.patch, 1, "patch");

    TEST_ASSERT(!UpdateChecker::parseVersion(QStringLiteral("")).valid, "空串不可解析");
    TEST_ASSERT(!UpdateChecker::parseVersion(QStringLiteral("latest")).valid,
                "非版本号文本不可解析");
}

static void testVersionCompare()
{
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("3.2.1"),
                                                  QStringLiteral("3.2.0")), 1, "patch 更大");
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("3.10.0"),
                                                  QStringLiteral("3.9.0")), 1,
                   "minor 按数值比较而非字典序");
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("V3.2.0"),
                                                  QStringLiteral("3.2.0")), 0,
                   "V 前缀不影响相等判定");
    // 正式版 > 预发布
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("3.2.0"),
                                                  QStringLiteral("3.2.0-rc1")), 1,
                   "正式版大于 rc");
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("3.2.0-beta1"),
                                                  QStringLiteral("3.2.0-alpha9")), 1,
                   "beta 大于 alpha");
    // 无法解析时保守返回 0（不升级）
    TEST_ASSERT_EQ(UpdateChecker::compareVersions(QStringLiteral("latest"),
                                                  QStringLiteral("3.2.0")), 0,
                   "不可解析保守视为相等");
}

// 核心回归：本机版本必须来自编译期 PROJECT_VERSION。
// 曾经读 theme.json 的 "version"，老配置里没有这个键 → 退化成 "0" →
// 任何上游 tag 都「更新」，这就是更新功能不可用的直接原因。
static void testNoFalsePositiveOnSameVersion()
{
    const QString local = QStringLiteral(CUBESHELL_VERSION);
    TEST_ASSERT(!local.isEmpty(), "编译期版本号非空");
    TEST_ASSERT(UpdateChecker::parseVersion(local).valid, "编译期版本号可解析");

    // 上游 tag 与本机同版本（带 V 前缀）→ 不应提示更新。
    TEST_ASSERT(!UpdateChecker::isNewer(QStringLiteral("V") + local, local),
                "同版本不提示更新");
    TEST_ASSERT(!UpdateChecker::isNewer(local, local), "同版本(无前缀)不提示更新");

    // 退化成 "0" 的老行为会让一切都算新版本——用它反证修复有效。
    TEST_ASSERT(UpdateChecker::isNewer(QStringLiteral("V2.8.0"), QStringLiteral("0")),
                "本机版本为 0 时旧版也算「新」（即误报的成因）");
    // 修好之后：Python 版的 2.8.0 不该覆盖 3.x 本机版本。
    TEST_ASSERT(!UpdateChecker::isNewer(QStringLiteral("V2.8.0"), local),
                "上游更旧的 tag 不提示更新");
}

// --- (d) Release JSON 解析 --------------------------------------------------
static QByteArray sampleReleaseJson()
{
    return QByteArrayLiteral(R"({
      "tag_name": "V3.2.0",
      "name": "V3.2.0",
      "body": "新增 Telnet/TCP 协议功能",
      "html_url": "https://github.com/Cubeiic-HanXuan/cube-shell-cpp/releases/tag/V3.2.0",
      "assets": [
        {"name":"cube-shell-3.2.0-linux-arm64.tar.gz","browser_download_url":"https://x/1","size":1,"content_type":"application/gzip"},
        {"name":"cube-shell-3.2.0-linux-x86_64.tar.gz","browser_download_url":"https://x/2","size":2,"content_type":"application/gzip"},
        {"name":"cube-shell-3.2.0-macos-arm64.dmg","browser_download_url":"https://x/3","size":3,"content_type":"application/octet-stream"},
        {"name":"cube-shell-3.2.0-macos-x86_64.dmg","browser_download_url":"https://x/4","size":4,"content_type":"application/octet-stream"},
        {"name":"cube-shell-3.2.0-windows-arm64.zip","browser_download_url":"https://x/5","size":5,"content_type":"application/zip"},
        {"name":"cube-shell-3.2.0-windows-x86_64.zip","browser_download_url":"https://x/6","size":6,"content_type":"application/zip"}
      ]
    })");
}

static void testParseReleaseJson()
{
    QString err;
    const UpdateReleaseInfo rel = UpdateChecker::parseReleaseJson(sampleReleaseJson(), &err);
    TEST_ASSERT(err.isEmpty(), "合法 JSON 无错误");
    TEST_ASSERT_EQ(rel.tagName, QStringLiteral("V3.2.0"), "tag_name");
    TEST_ASSERT_EQ(rel.assets.size(), 6, "asset 数量");
    TEST_ASSERT(rel.htmlUrl.contains(QLatin1String("cube-shell-cpp")), "html_url");
    TEST_ASSERT_EQ(rel.assets.at(0).size, qint64(1), "asset size 解析为整数");

    // 非法 JSON → 报错且 tagName 为空，调用方据此走 checkFailed。
    UpdateChecker::parseReleaseJson(QByteArrayLiteral("{not json"), &err);
    TEST_ASSERT(!err.isEmpty(), "非法 JSON 产生错误信息");

    // 缺 html_url 时用 kReleasePage 兜底。
    err.clear();
    const UpdateReleaseInfo bare = UpdateChecker::parseReleaseJson(
        QByteArrayLiteral(R"({"tag_name":"V3.2.0","assets":[]})"), &err);
    TEST_ASSERT_EQ(bare.htmlUrl, QString::fromLatin1(UpdateChecker::kReleasePage),
                   "html_url 缺失时兜底到 Release 页");
}

// --- (e) 平台 asset 匹配 ----------------------------------------------------
//
// selectAsset 依赖编译期平台宏，只能验证「当前平台」的结果。各平台各断言一次，
// 合起来由 CI 的多平台构建覆盖全部分支。
static void testSelectAssetForCurrentPlatform()
{
    QString err;
    const UpdateReleaseInfo rel = UpdateChecker::parseReleaseJson(sampleReleaseJson(), &err);
    const UpdateAssetInfo picked = UpdateChecker::selectAsset(rel.assets);

    const QString arch = QSysInfo::currentCpuArchitecture();
    QString expectOs;
#if defined(Q_OS_MACOS)
    expectOs = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    expectOs = QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    expectOs = QStringLiteral("linux");
#endif

    if (expectOs.isEmpty()) {
        // 其它平台（如鸿蒙）没有发布物，返回空是正确行为。
        TEST_ASSERT(picked.name.isEmpty(), "无发布物的平台返回空");
        return;
    }

    TEST_ASSERT(!picked.name.isEmpty(),
                "当前平台应能唯一匹配到 asset（返回空说明平台规则缺分支）");
    TEST_ASSERT(picked.name.contains(expectOs), "匹配到的 asset 属于当前操作系统");
    TEST_ASSERT(!picked.browserDownloadUrl.isEmpty(), "下载直链非空");

    // 绝不跨架构：arm64 机器不能拿到 x86_64 包，反之亦然。
    if (arch == QLatin1String("arm64")) {
        TEST_ASSERT(picked.name.contains(QLatin1String("arm64")), "arm64 机器取 arm64 包");
        TEST_ASSERT(!picked.name.contains(QLatin1String("x86_64")), "不能错拿 x86_64 包");
    } else if (arch == QLatin1String("x86_64")) {
        TEST_ASSERT(picked.name.contains(QLatin1String("x86_64")), "x86_64 机器取 x86_64 包");
        TEST_ASSERT(!picked.name.contains(QLatin1String("arm64")), "不能错拿 arm64 包");
    }
}

// 只有异架构发布物时必须返回空（走 Release 页兜底），而不是硬塞一个错包。
static void testSelectAssetRejectsForeignArch()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch != QLatin1String("arm64") && arch != QLatin1String("x86_64")) {
        ++g_passed;   // 该场景在当前架构上不适用
        return;
    }
    const QString foreign = arch == QLatin1String("arm64")
                                ? QStringLiteral("x86_64") : QStringLiteral("arm64");

    QList<UpdateAssetInfo> assets;
    for (const QString &os : {QStringLiteral("macos"), QStringLiteral("windows"),
                              QStringLiteral("linux")}) {
        const QString ext = os == QStringLiteral("macos")   ? QStringLiteral(".dmg")
                          : os == QStringLiteral("windows") ? QStringLiteral(".zip")
                                                            : QStringLiteral(".tar.gz");
        UpdateAssetInfo a;
        a.name = QStringLiteral("cube-shell-3.2.0-%1-%2%3").arg(os, foreign, ext);
        a.browserDownloadUrl = QStringLiteral("https://x/") + a.name;
        assets.append(a);
    }
    const UpdateAssetInfo picked = UpdateChecker::selectAsset(assets);
    TEST_ASSERT(picked.name.isEmpty(), "只有异架构包时返回空，交给 Release 页兜底");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testRepoUrlsPointToCppRepo();
    testVersionParsing();
    testVersionCompare();
    testNoFalsePositiveOnSameVersion();
    testParseReleaseJson();
    testSelectAssetForCurrentPlatform();
    testSelectAssetRejectsForeignArch();

    qInfo() << "passed:" << g_passed << "failed:" << g_failed;
    return g_failed == 0 ? 0 : 1;
}
