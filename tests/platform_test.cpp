// platform_test.cpp — 平台集成模块单元测试（纯逻辑，不写系统状态）。
//
// 覆盖 Phase 6 新增的 core/platform：
//   - FinderIntegration  : 平台支持判定 / workflow 路径推导
//   - WindowsIntegration : 平台支持判定（非 Windows 下 stub 语义）
//   - UrlSchemeRegistrar : 默认 scheme 集合 / isRegistered 不崩不写
//   - PlatformIntegration: 门面按平台分发与底层一致
// CUBESHELL_WITH_RDP=ON 时额外覆盖 buildRdpUrl（对应 Python build_rdp_url）。
//
// 刻意**不**调用 install/register：那些会真实改动用户环境（~/Library/Services、
// HKCU 注册表、~/.local/share/applications），单测只做只读探测。

#include <QCoreApplication>
#include <QDir>
#include <QtGlobal>

#include <cstdio>

#include "platform/FinderIntegration.h"
#include "platform/PlatformIntegration.h"
#include "platform/UrlSchemeRegistrar.h"
#include "platform/WindowsIntegration.h"

#ifdef CUBESHELL_WITH_RDP
#include "rdp/RdpClient.h"
#endif

namespace {

int g_failures = 0;

void check(bool condition, const char *what)
{
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

void testFinderIntegration()
{
    std::printf("FinderIntegration\n");
#if defined(Q_OS_MACOS)
    check(cubeshell::FinderIntegration::isSupported(), "macOS 下 isSupported() == true");
    const QString path = cubeshell::FinderIntegration::workflowPath();
    check(path.startsWith(QDir::homePath() + QStringLiteral("/Library/Services/")),
          "workflowPath() 落在 ~/Library/Services/");
    check(path.endsWith(QStringLiteral("Open in CubeShell.workflow")),
          "workflowPath() 名称与 Python WORKFLOW_NAME 一致");
    // isInstalled 只读探测，两种结果都合法，只要不崩
    (void)cubeshell::FinderIntegration::isInstalled();
    check(true, "isInstalled() 只读探测不崩溃");
#else
    check(!cubeshell::FinderIntegration::isSupported(),
          "非 macOS 下 isSupported() == false");
    check(!cubeshell::FinderIntegration::isInstalled(),
          "非 macOS 下 isInstalled() == false");
    QString error;
    check(!cubeshell::FinderIntegration::installFinderExtension(&error),
          "非 macOS 下 install 返回 false");
    check(!error.isEmpty(), "非 macOS 下 install 填充错误信息");
#endif
}

void testWindowsIntegration()
{
    std::printf("WindowsIntegration\n");
#if defined(Q_OS_WIN)
    check(cubeshell::WindowsIntegration::isSupported(), "Windows 下 isSupported() == true");
    (void)cubeshell::WindowsIntegration::isInstalled();
    (void)cubeshell::WindowsIntegration::isAutoStartEnabled();
    check(true, "注册表只读探测不崩溃");
#else
    check(!cubeshell::WindowsIntegration::isSupported(),
          "非 Windows 下 isSupported() == false");
    check(!cubeshell::WindowsIntegration::isInstalled(),
          "非 Windows 下 isInstalled() == false");
    check(!cubeshell::WindowsIntegration::isAutoStartEnabled(),
          "非 Windows 下 isAutoStartEnabled() == false");
    QString error;
    check(!cubeshell::WindowsIntegration::install(&error),
          "非 Windows 下 install 返回 false");
    check(!error.isEmpty(), "非 Windows 下 install 填充错误信息");
#endif
}

void testUrlSchemeRegistrar()
{
    std::printf("UrlSchemeRegistrar\n");
    const QStringList schemes = cubeshell::UrlSchemeRegistrar::defaultSchemes();
    // 对应Python: _register_windows 里的 ("jms", "cubeshell")；telnet 是 C++ 侧
    // 新增（IANA 在案的标准 scheme，见 UrlSchemeRegistrar::defaultSchemes 注释）。
    // ssh 仍默认不启用，需调用方显式传入。
    check(schemes.size() == 3, "defaultSchemes() 为 3 项");
    check(schemes.contains(QStringLiteral("jms")), "defaultSchemes() 含 jms");
    check(schemes.contains(QStringLiteral("cubeshell")),
          "defaultSchemes() 含 cubeshell");
    check(schemes.contains(QStringLiteral("telnet")),
          "defaultSchemes() 含 telnet");
    check(!schemes.contains(QStringLiteral("ssh")),
          "defaultSchemes() 默认不含 ssh");

    // 只读探测：结果依赖本机环境，只要求不崩且对未知 scheme 返回 false
    (void)cubeshell::UrlSchemeRegistrar::isRegistered();
    check(!cubeshell::UrlSchemeRegistrar::isRegistered(
              {QStringLiteral("cubeshell-not-a-real-scheme")}),
          "未知 scheme 的 isRegistered() == false");
}

void testPlatformIntegrationFacade()
{
    std::printf("PlatformIntegration (facade)\n");
#if defined(Q_OS_MACOS)
    check(cubeshell::PlatformIntegration::isContextMenuSupported()
              == cubeshell::FinderIntegration::isSupported(),
          "macOS 下门面分发到 FinderIntegration");
    check(cubeshell::PlatformIntegration::isContextMenuInstalled()
              == cubeshell::FinderIntegration::isInstalled(),
          "macOS 下 isContextMenuInstalled 与底层一致");
#elif defined(Q_OS_WIN)
    check(cubeshell::PlatformIntegration::isContextMenuSupported()
              == cubeshell::WindowsIntegration::isSupported(),
          "Windows 下门面分发到 WindowsIntegration");
#else
    check(!cubeshell::PlatformIntegration::isContextMenuSupported(),
          "其他平台门面报告不支持");
    QString error;
    check(!cubeshell::PlatformIntegration::installContextMenu(&error),
          "其他平台 installContextMenu 返回 false");
    check(!error.isEmpty(), "其他平台 installContextMenu 填充错误信息");
#endif
}

#ifdef CUBESHELL_WITH_RDP
void testRdp()
{
    std::printf("RdpClient (buildRdpUrl)\n");
    cubeshell::RdpSettings settings;
    settings.host = QStringLiteral("10.0.0.5");
    settings.port = 3389;
    settings.username = QStringLiteral("Administrator");
    settings.password = QStringLiteral("p@ss word");
    settings.domain = QStringLiteral("CORP");

    // 对应Python: build_rdp_url —— ntlm 走 rdp+ntlm-password，DOMAIN\user
    // 保留反斜杠，密码完整 percent-encoding
    const QString url = cubeshell::buildRdpUrl(settings);
    check(url.startsWith(QStringLiteral("rdp+ntlm-password://")),
          "ntlm 认证使用 rdp+ntlm-password scheme");
    check(url.contains(QStringLiteral("CORP\\Administrator")),
          "DOMAIN\\user 形式保留反斜杠");
    check(url.contains(QStringLiteral("p%40ss%20word")), "密码做 percent-encoding");
    check(url.endsWith(QStringLiteral("@10.0.0.5:3389")), "host:port 结尾正确");

    const QString plain = cubeshell::buildRdpUrl(settings, QStringLiteral("plain"));
    check(plain.startsWith(QStringLiteral("rdp://")), "plain 认证使用 rdp scheme");

    // IPv6 裸地址加方括号
    cubeshell::RdpSettings v6;
    v6.host = QStringLiteral("fe80::1");
    v6.port = 3389;
    check(cubeshell::buildRdpUrl(v6).contains(QStringLiteral("[fe80::1]:3389")),
          "IPv6 主机自动加方括号");

    // 无凭据时不应出现 '@'
    cubeshell::RdpSettings bare;
    bare.host = QStringLiteral("host.local");
    check(!cubeshell::buildRdpUrl(bare).contains(QLatin1Char('@')),
          "无用户名时不产生 userinfo");

    // 后端解析：编入 FreeRDP 走库后端，否则命令行后备。
    //
    // 这里不能用 #ifdef CUBESHELL_HAVE_FREERDP 分支——那个宏是 cube_core 的
    // PRIVATE 编译定义（src/core/CMakeLists.txt），测试目标看不见它，于是无论
    // 实际编没编入 FreeRDP，测试都会走 #else 去断言 CommandLine 而误报失败。
    // backend() 是运行时可查的，改为只断言取值合法 + 后备路径探测不崩。
    const auto backend = cubeshell::RdpClient::backend();
    check(backend == cubeshell::RdpClient::Backend::FreeRdp
              || backend == cubeshell::RdpClient::Backend::CommandLine,
          "backend() 返回合法后端取值");
    // 本机可能没装 xfreerdp/mstsc，只要求探测不崩
    (void)cubeshell::RdpClient::commandLineProgram();
    check(true, "commandLineProgram() 探测不崩溃");
}
#endif // CUBESHELL_WITH_RDP

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testFinderIntegration();
    testWindowsIntegration();
    testUrlSchemeRegistrar();
    testPlatformIntegrationFacade();
#ifdef CUBESHELL_WITH_RDP
    testRdp();
#endif

    if (g_failures == 0) {
        std::printf("platform_test: all checks passed\n");
        return 0;
    }
    std::printf("platform_test: %d check(s) failed\n", g_failures);
    return 1;
}
