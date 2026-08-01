// FRP 模块单元测试：平台/架构包名映射、FrpInstaller 的纯函数（下载地址、
// 归档目录名、远端解压命令）、frps/frpc/proxy 配置与 Python 侧
// function/traversal.py 的逐字对比、frpc.toml 写入→ConfigUtil 解析回读往返。
// 全部离线，不发起任何网络请求、不建立 SSH 连接。
// Style mirrors tests/util_test.cpp (no gtest, main + CHECK macro).
//
// 对应Python: core/frp_manager.py + function/traversal.py + cube-shell.py::nat_lod

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include "config/ConfigUtil.h"
#include "forwarder/FrpConnectWorker.h"
#include "forwarder/FrpInstaller.h"
#include "forwarder/FrpManager.h"
#include "util/FileUtil.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 对应Python: core/frp_manager.py::SERVER_ARCH_MAP / FRP_DOWNLOADS / get_platform_key
static void testPackageNames()
{
    const QString v = FrpManager::kFrpVersion;
    CHECK(v == QStringLiteral("0.61.1")); // 对应Python: FRP_VERSION = "0.61.1"

    // SERVER_ARCH_MAP 的五个键
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("x86_64"))
          == QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(v));
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("amd64"))
          == QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(v));
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("aarch64"))
          == QStringLiteral("frp_%1_linux_arm64.tar.gz").arg(v));
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("arm64"))
          == QStringLiteral("frp_%1_linux_arm64.tar.gz").arg(v));
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("armv7l"))
          == QStringLiteral("frp_%1_linux_arm.tar.gz").arg(v));
    // `arch` 输出带换行/空格时也要命中（Python 侧 server_arch.strip()）
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral(" x86_64\n"))
          == QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(v));
    // 未知架构 -> 空（Python: SERVER_ARCH_MAP.get(...) is None）
    CHECK(FrpManager::serverPackageNameForArch(QStringLiteral("riscv64")).isEmpty());
    CHECK(FrpManager::serverPackageNameForArch(QString()).isEmpty());

    // 当前平台包名：本地支持的三大平台都应有值，且带版本号前缀
    const QString pkg = FrpManager::packageNameForCurrentPlatform();
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    CHECK(!pkg.isEmpty());
    CHECK(pkg.startsWith(QStringLiteral("frp_%1_").arg(v)));
    CHECK(pkg.endsWith(QStringLiteral(".tar.gz")) || pkg.endsWith(QStringLiteral(".zip")));
#endif
#ifdef Q_OS_DARWIN
    CHECK(pkg.contains(QStringLiteral("darwin")));
    CHECK(pkg.endsWith(QStringLiteral(".tar.gz")));
#endif
#ifdef Q_OS_WIN
    CHECK(pkg.contains(QStringLiteral("windows")));
    CHECK(pkg.endsWith(QStringLiteral(".zip"))); // Windows 包是 zip
#endif
}

// 对应Python: core/frp_manager.py::FRP_GITHUB_BASE / download_frps_for_server 的 cmd
static void testInstallerPureFunctions()
{
    const QString v = FrpManager::kFrpVersion;
    CHECK(FrpInstaller::githubBase()
          == QStringLiteral("https://github.com/fatedier/frp/releases/download/v%1").arg(v));

    const QString pkg = QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(v);
    CHECK(FrpInstaller::downloadUrl(pkg)
          == FrpInstaller::githubBase() + QStringLiteral("/") + pkg);

    // 对应Python: package_name.replace(".tar.gz", "").replace(".zip", "")
    CHECK(FrpInstaller::archiveBaseName(pkg)
          == QStringLiteral("frp_%1_linux_amd64").arg(v));
    CHECK(FrpInstaller::archiveBaseName(QStringLiteral("frp_%1_windows_amd64.zip").arg(v))
          == QStringLiteral("frp_%1_windows_amd64").arg(v));

    // 远端解压命令必须与 Python 逐字一致（两版行为可互换）
    const QString expected =
        QStringLiteral("cd /root && tar -xzf %1 && rm -rf frp && mv %2 frp && rm -f %1")
            .arg(pkg, FrpInstaller::archiveBaseName(pkg));
    CHECK(FrpInstaller::remoteExtractCommand(QStringLiteral("/root"), pkg) == expected);
}

// 对应Python: function/traversal.py::frps
static void testFrpsConfig()
{
    // Python: frps("abc123") ->
    //   'bindPort = 7000\nauth.token = "abc123"\n'
    CHECK(FileUtil::frpsConfig(QStringLiteral("abc123"), QStringLiteral("tcp"), 0)
          == QStringLiteral("bindPort = 7000\nauth.token = \"abc123\"\n"));

    // 非 HTTP 类型即使给了端口也不写 vhostHTTPPort
    CHECK(FileUtil::frpsConfig(QStringLiteral("t"), QStringLiteral("TCP"), 8088)
          == QStringLiteral("bindPort = 7000\nauth.token = \"t\"\n"));
    CHECK(FileUtil::frpsConfig(QStringLiteral("t"), QStringLiteral("UDP"), 8088)
          == QStringLiteral("bindPort = 7000\nauth.token = \"t\"\n"));

    // HTTP + 端口 -> 追加 vhostHTTPPort（Python: ant_type.lower() == "http" and http_port）
    CHECK(FileUtil::frpsConfig(QStringLiteral("t"), QStringLiteral("HTTP"), 8088)
          == QStringLiteral("bindPort = 7000\nauth.token = \"t\"\nvhostHTTPPort = 8088\n"));
    // HTTP 但端口为 0/空 -> 不追加（Python 的 http_port 为 None/0 时 falsy）
    CHECK(FileUtil::frpsConfig(QStringLiteral("t"), QStringLiteral("http"), 0)
          == QStringLiteral("bindPort = 7000\nauth.token = \"t\"\n"));

    // FrpManager::buildFrpsConfig 只是转发，输出必须一致
    CHECK(FrpManager::buildFrpsConfig(QStringLiteral("t"), QStringLiteral("HTTP"), 8088)
          == FileUtil::frpsConfig(QStringLiteral("t"), QStringLiteral("HTTP"), 8088));
}

// 对应Python: function/traversal.py::proxy_config（7 种协议下拉项）
static void testProxyConfigAllProtocols()
{
    const QString http = QStringLiteral(
        "[[proxies]]\n"
        "name = \"http_proxy\"\n"
        "type = \"http\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 8080\n"
        "customDomains = [\"1.2.3.4\"]\n");
    const QString udp = QStringLiteral(
        "[[proxies]]\n"
        "name = \"udp_proxy\"\n"
        "type = \"udp\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 8080\n"
        "remotePort = 8088\n");
    const QString tcp = QStringLiteral(
        "[[proxies]]\n"
        "name = \"tcp_proxy\"\n"
        "type = \"tcp\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 8080\n"
        "remotePort = 8088\n");

    const QString addr = QStringLiteral("1.2.3.4");
    CHECK(FileUtil::proxyConfig(QStringLiteral("TCP"), 8080, 8088, addr) == tcp);
    CHECK(FileUtil::proxyConfig(QStringLiteral("UDP"), 8080, 8088, addr) == udp);
    CHECK(FileUtil::proxyConfig(QStringLiteral("HTTP"), 8080, 8088, addr) == http);
    // HTTPS / STCP / SUDP / XTCP 在 Python 侧都落 else 分支（tcp 模板）
    CHECK(FileUtil::proxyConfig(QStringLiteral("HTTPS"), 8080, 8088, addr) == tcp);
    CHECK(FileUtil::proxyConfig(QStringLiteral("STCP"), 8080, 8088, addr) == tcp);
    CHECK(FileUtil::proxyConfig(QStringLiteral("SUDP"), 8080, 8088, addr) == tcp);
    CHECK(FileUtil::proxyConfig(QStringLiteral("XTCP"), 8080, 8088, addr) == tcp);

    // frpc 整份配置：Python f-string 的换行/空行位置逐字对齐
    CHECK(FileUtil::frpcConfig(addr, QStringLiteral("tok"), QStringLiteral("TCP"), 8080, 8088)
          == QStringLiteral("serverAddr = \"1.2.3.4\"\n"
                            "serverPort = 7000\n"
                            "auth.token = \"tok\"\n"
                            "\n")
                 + tcp + QStringLiteral("\n"));
}

// frpc.toml 写入 → ConfigUtil::readToml 回读（对应Python: nat_lod 的 toml.load）
static void testFrpcConfigRoundTrip()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("frpc.toml"));

    QString err;
    CHECK(FrpManager::writeFrpcConfigFile(path, QStringLiteral("10.0.0.7"),
                                          QStringLiteral("s3cr3t"), QStringLiteral("TCP"),
                                          8080, 8088, &err));
    CHECK(err.isEmpty());
    CHECK(QFile::exists(path));

    const QVariantMap cfg = ConfigUtil::readToml(path, &err);
    CHECK(err.isEmpty());
    // 对应Python: config['serverAddr'] / config['auth']['token'] / config['proxies']
    CHECK(cfg.value(QStringLiteral("serverAddr")).toString() == QStringLiteral("10.0.0.7"));
    CHECK(cfg.value(QStringLiteral("serverPort")).toInt() == 7000);
    CHECK(cfg.contains(QStringLiteral("auth")));
    CHECK(cfg.value(QStringLiteral("auth")).toMap().value(QStringLiteral("token")).toString()
          == QStringLiteral("s3cr3t"));

    const QVariantList proxies = cfg.value(QStringLiteral("proxies")).toList();
    CHECK(proxies.size() == 1);
    if (!proxies.isEmpty()) {
        const QVariantMap p = proxies.first().toMap();
        // nat_lod 用的四个字段
        CHECK(p.value(QStringLiteral("type")).toString().toUpper() == QStringLiteral("TCP"));
        CHECK(p.value(QStringLiteral("localPort")).toString() == QStringLiteral("8080"));
        CHECK(p.contains(QStringLiteral("remotePort")));
        CHECK(p.value(QStringLiteral("remotePort")).toString() == QStringLiteral("8088"));
        CHECK(p.value(QStringLiteral("name")).toString() == QStringLiteral("tcp_proxy"));
    }

    // HTTP 类型没有 remotePort（nat_lod 的 if 'remotePort' in proxy 分支）
    const QString httpPath = QDir(tmp.path()).filePath(QStringLiteral("frpc_http.toml"));
    CHECK(FrpManager::writeFrpcConfigFile(httpPath, QStringLiteral("10.0.0.7"),
                                          QStringLiteral("t"), QStringLiteral("HTTP"),
                                          8080, 8088, &err));
    const QVariantList httpProxies =
        ConfigUtil::readToml(httpPath).value(QStringLiteral("proxies")).toList();
    CHECK(httpProxies.size() == 1);
    if (!httpProxies.isEmpty()) {
        const QVariantMap p = httpProxies.first().toMap();
        CHECK(p.value(QStringLiteral("type")).toString() == QStringLiteral("http"));
        CHECK(!p.contains(QStringLiteral("remotePort")));
    }
}

// frpc.toml 落盘位置必须叫 frpc.toml 且位于某个 conf/ 或用户配置目录下 ——
// 与 Python 的 abspath('frpc.toml') 语义对齐（不做网络/SSH）。
static void testFrpcConfigPath()
{
    const QString path = FrpConnectWorker::frpcConfigPath();
    CHECK(!path.isEmpty());
    CHECK(path.endsWith(QStringLiteral("frpc.toml")));
    CHECK(QDir::isAbsolutePath(path));
}

// 可达性探测的边界：空主机/保留地址上的关闭端口应快速返回 false（无外网依赖）。
// 对应Python: function/util.py::check_server_accessibility
static void testServerAccessibility()
{
    CHECK(!FrpConnectWorker::checkServerAccessibility(QString(), 22));
    // 127.0.0.1 的高位端口通常无监听；即便偶然有，也不影响其他断言。
    CHECK(!FrpConnectWorker::checkServerAccessibility(QStringLiteral("127.0.0.1"), 1, 300));
}

// 端口文本校验（C++ 特有安全修复）：Python 的 int() 对非法输入抛异常并被
// FRPConnectThread.run 的 except 兜成错误提示；C++ 的 toInt() 只会静默给 0，
// 因此必须显式校验后才允许进入启动流程。
static void testPortValidation()
{
    int port = -1;
    CHECK(FrpConnectWorker::isValidPort(QStringLiteral("8088"), &port));
    CHECK(port == 8088);

    // 边界：1 与 65535 合法，0 与 65536 非法
    CHECK(FrpConnectWorker::isValidPort(QStringLiteral("1")));
    CHECK(FrpConnectWorker::isValidPort(QStringLiteral("65535")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("0")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("65536")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("-1")));

    // 非数字/空白/空串：toInt() 会静默返回 0，这里必须判为非法
    CHECK(!FrpConnectWorker::isValidPort(QString()));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("abc")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("80a")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("8.5")));
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("  ")));

    // 首尾空白允许（用户从输入框粘贴常见）
    CHECK(FrpConnectWorker::isValidPort(QStringLiteral(" 22 "), &port));
    CHECK(port == 22);

    // 校验失败时不得改写出参（避免调用方误用残留值）
    int untouched = 12345;
    CHECK(!FrpConnectWorker::isValidPort(QStringLiteral("bad"), &untouched));
    CHECK(untouched == 12345);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testPackageNames();
    testInstallerPureFunctions();
    testFrpsConfig();
    testProxyConfigAllProtocols();
    testFrpcConfigRoundTrip();
    testFrpcConfigPath();
    testServerAccessibility();
    testPortValidation();

    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "frp_installer_test: all checks passed";
    return 0;
}
