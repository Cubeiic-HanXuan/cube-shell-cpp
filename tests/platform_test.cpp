// platform_test.cpp — 平台集成模块单元测试（纯逻辑，不写系统状态）。
//
// 覆盖 Phase 6 新增的 core/platform：
//   - FinderIntegration  : 平台支持判定 / workflow 路径推导
//   - WindowsIntegration : 平台支持判定（非 Windows 下 stub 语义）
//   - UrlSchemeRegistrar : 默认 scheme 集合 / isRegistered 不崩不写
//   - PlatformIntegration: 门面按平台分发与底层一致
// CUBESHELL_WITH_RDP=ON 时额外覆盖 RDP 命令行参数不含明文密码、RdpPanel 的
// 分辨率夹取/解析（"分辨率太低" 那个 bug 的回归闸门），以及剪贴板重定向里
// 那道不可信输入闸门（远端文件清单解析 + 文件名过滤）。
//
// 刻意**不**调用 install/register：那些会真实改动用户环境（~/Library/Services、
// HKCU 注册表、~/.local/share/applications），单测只做只读探测。

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QUrl>
#include <QtGlobal>

#include <cstdio>

#include "platform/FinderIntegration.h"
#include "platform/PlatformIntegration.h"
#include "platform/UrlSchemeRegistrar.h"
#include "platform/WindowsIntegration.h"

#ifdef CUBESHELL_WITH_RDP
#include "rdp/RdpClient.h"
#include "rdp/RdpClipboard.h"
#include "rdp/RdpPanel.h"
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
    std::printf("RdpClient (命令行密码不得出现在 argv)\n");

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

    if (backend == cubeshell::RdpClient::Backend::CommandLine) {
        cubeshell::RdpSettings settings;
        settings.host = QStringLiteral("10.0.0.5");
        settings.port = 3389;
        settings.username = QStringLiteral("Administrator");
        settings.password = QStringLiteral("Sup3rSecret!");
        settings.domain = QStringLiteral("CORP");

        cubeshell::RdpClient client;
        client.connectToHost(settings);   // 会真的尝试启动；失败也无妨，只取参数
        const QStringList argv = client.commandLineArgsForTest();
        client.disconnectFromHost();

        if (argv.isEmpty()) {
            // 本机没有 xfreerdp/mstsc：探测不崩即可，无法断言参数
            check(true, "命令行后端但无外部客户端，跳过参数断言");
        } else {
            const QString joined = argv.join(QLatin1Char(' '));
            check(!joined.contains(settings.password),
                  "命令行参数不含明文密码（/from-stdin 替代 /p:）");
            check(!joined.contains(QStringLiteral("/p:")),
                  "命令行参数不使用 /p: 形式");
            check(joined.contains(QStringLiteral("/from-stdin:force")),
                  "命令行参数声明 /from-stdin:force");
        }
    } else {
        // FreeRDP 库后端：进程内建连，密码根本不进命令行
        check(true, "FreeRDP 库后端：无命令行，密码天然不外泄");
    }

    // 本机可能没装 xfreerdp/mstsc，只要求探测不崩
    (void)cubeshell::RdpClient::commandLineProgram();
    check(true, "commandLineProgram() 探测不崩溃");
}

// 分辨率夹取/解析（RdpPanel 的静态部分，纯函数，不建控件）。
// 这是 "RDP 分辨率太低" 的回归闸门：旧实现把主屏物理像素 ÷2 后夹进
// [1280x800, 4096x2304]，1080p 普通屏一律连成 1280x800。现在只做协议对齐，
// 不再有任何"下限抬高"的动作。
void testRdpResolution()
{
    using cubeshell::RdpPanel;
    std::printf("RdpPanel 分辨率（夹取 + 解析）\n");

    // 主流尺寸原样通过：不再被砍半、也不被抬到 1280x800
    check(RdpPanel::alignResolution(QSize(1920, 1080)) == QSize(1920, 1080),
          "1920x1080 原样保留");
    check(RdpPanel::alignResolution(QSize(2560, 1440)) == QSize(2560, 1440),
          "2560x1440 原样保留");
    // 典型的"最大化窗口"尺寸：宽已是 4 的倍数，高奇数 → 向下对齐到偶数
    check(RdpPanel::alignResolution(QSize(1912, 997)) == QSize(1912, 996),
          "1912x997 → 高对齐到 996");
    check(RdpPanel::alignResolution(QSize(1366, 768)) == QSize(1364, 768),
          "1366x768 → 宽对齐到 4 的倍数");
    // 越界夹取：小窗口/巨屏都收进协议区间
    check(RdpPanel::alignResolution(QSize(100, 50)) == QSize(640, 480),
          "过小尺寸夹到 640x480");
    check(RdpPanel::alignResolution(QSize(8000, 5000)) == QSize(4096, 2304),
          "过大尺寸夹到 4096x2304");
    check(RdpPanel::alignResolution(QSize(0, 0)) == QSize(640, 480),
          "零尺寸夹到 640x480（画布还没布局时的兜底）");

    // 解析：大小写 x、多余空格都收
    check(RdpPanel::parseResolution(QStringLiteral("1920x1080")) == QSize(1920, 1080),
          "解析 1920x1080");
    check(RdpPanel::parseResolution(QStringLiteral("1920X1080")) == QSize(1920, 1080),
          "解析大写 X");
    check(RdpPanel::parseResolution(QStringLiteral(" 2560 x 1440 ")) == QSize(2560, 1440),
          "解析带空格");
    // 非法输入一律判无效（调用方退回"适应窗口"，绝不猜一个用户没要的尺寸）
    for (const char *bad : {"", "abc", "1920", "1920x", "x1080", "0x0",
                            "1920x1080x2", "1920*1080"})
        check(!RdpPanel::parseResolution(QString::fromLatin1(bad)).isValid(),
              "非法分辨率串判无效");
}

// 远端文件自动取回的体积闸门。
// 「必须点『取回 N 个文件』才能粘贴」是用户明确反馈的反人类交互，现在远端复制后
// 自动传回，只有超过上限才退回手动确认。这组断言守住那条界线：小的必须自动、
// 大的必须拦下，否则要么回到旧交互，要么变成静默拉几个 GB。
void testRdpAutoFetch()
{
    using cubeshell::RdpPanel;
    std::printf("RdpPanel 远端文件自动取回闸门\n");

    constexpr qint64 limit = RdpPanel::kAutoFetchLimitBytes;
    check(limit > 0, "上限是正数");

    // 日常场景（几个文档、一张图）必须自动，不能再让用户点按钮
    check(RdpPanel::shouldAutoFetch(0, false), "空内容自动取回");
    check(RdpPanel::shouldAutoFetch(1, false), "1 字节自动取回");
    check(RdpPanel::shouldAutoFetch(5 * 1024 * 1024, false), "5 MiB 自动取回");
    check(RdpPanel::shouldAutoFetch(limit, false), "正好等于上限仍自动取回");

    // 超限必须拦下：静默传几个 GB 是灾难
    check(!RdpPanel::shouldAutoFetch(limit + 1, false), "超上限 1 字节即拦下");
    check(!RdpPanel::shouldAutoFetch(8LL * 1024 * 1024 * 1024, false),
          "8 GiB 拦下，退回手动确认");

    // 大小未知（目录条目不带 FD_FILESIZE）不该阻止自动取回——否则复制文件夹
    // 永远退回手动，等于没修。传输中的累计闸门负责兜底。
    check(RdpPanel::shouldAutoFetch(0, true), "大小未知但总量小 → 仍自动取回");
    check(RdpPanel::shouldAutoFetch(1024, true), "含未知大小条目不影响判定");
    // 未知 + 已知部分就超限时，照样拦
    check(!RdpPanel::shouldAutoFetch(limit + 1, true),
          "已知部分已超限时即使有未知条目也拦下");
}

// --- 剪贴板重定向（RdpClipboard 的纯静态部分）---------------------------------
// 造一条 FILEDESCRIPTORW（[MS-RDPECLIP] 2.2.5.2.3.1，定长 592 字节）。
// 字段偏移与 RdpClipboard.cpp 顶部的常量表一一对应，这里手写一遍正是为了
// 让"解析器读的偏移"与"协议规定的偏移"能互相对账。
QByteArray makeFileDescriptor(const QString &name, quint64 size, bool haveSize,
                              bool isDirectory)
{
    QByteArray d(592, '\0');
    const auto put32 = [&d](int offset, quint32 v) {
        d[offset]     = char(v & 0xffu);
        d[offset + 1] = char((v >> 8) & 0xffu);
        d[offset + 2] = char((v >> 16) & 0xffu);
        d[offset + 3] = char((v >> 24) & 0xffu);
    };

    quint32 flags = 0;
    if (haveSize)
        flags |= 0x40u;   // FD_FILESIZE
    if (isDirectory)
        flags |= 0x04u;   // FD_ATTRIBUTES
    put32(0, flags);
    if (isDirectory)
        put32(36, 0x10u); // fileAttributes = FILE_ATTRIBUTE_DIRECTORY
    put32(64, quint32(size >> 32));
    put32(68, quint32(size & 0xffffffffu));

    // fileName：UTF-16LE，520 字节 = 最多 260 字符；写不满的部分留 NUL 填充。
    // 恰好写满 260 字符时字段里**没有**终止符——解析器必须靠长度停下，
    // 不能靠找 NUL（否则会读到下一条描述符里去）。
    for (int i = 0; i < name.size() && i < 260; ++i) {
        const ushort ch = name.at(i).unicode();
        d[72 + i * 2]     = char(ch & 0xffu);
        d[72 + i * 2 + 1] = char((ch >> 8) & 0xffu);
    }
    return d;
}

QByteArray makeFileList(const QList<QByteArray> &descriptors, quint32 declaredCount)
{
    QByteArray out(4, '\0');
    out[0] = char(declaredCount & 0xffu);
    out[1] = char((declaredCount >> 8) & 0xffu);
    out[2] = char((declaredCount >> 16) & 0xffu);
    out[3] = char((declaredCount >> 24) & 0xffu);
    for (const QByteArray &d : descriptors)
        out += d;
    return out;
}

// 远端剪贴板是**不可信输入**，而"取回文件"是本程序里唯一按远端给的名字往本地
// 磁盘写东西的路径。这一组断言就是那道闸门的回归测试。
void testRdpClipboard()
{
    using cubeshell::RdpClipboard;
    using cubeshell::RdpRemoteFile;
    std::printf("RdpClipboard（文件清单解析 + 文件名闸门 + uri-list）\n");

    // --- 正常多文件：名字/大小/目录标志/原始下标都要对 ---
    {
        const QByteArray blob = makeFileList(
            {makeFileDescriptor(QStringLiteral("a.txt"), 1234, true, false),
             makeFileDescriptor(QStringLiteral("sub\\b.bin"), 0x1'0000'0002ull, true, false),
             makeFileDescriptor(QStringLiteral("dir"), 0, false, true)},
            3);
        QString err;
        const QList<RdpRemoteFile> files = RdpClipboard::parseFileDescriptors(blob, &err);
        check(err.isEmpty(), "正常清单解析无错误");
        check(files.size() == 3, "解析出 3 项");
        if (files.size() == 3) {
            check(files.at(0).name == QStringLiteral("a.txt"), "第 1 项名字");
            check(files.at(0).size == 1234 && files.at(0).sizeKnown, "第 1 项大小 1234");
            check(!files.at(0).isDirectory, "第 1 项不是目录");
            // 反斜杠统一成 '/'：远端是 Windows，本机可能不是
            check(files.at(1).name == QStringLiteral("sub/b.bin"), "反斜杠转成 /");
            // 4GB 以上：high/low 两个 32 位字段必须拼对，不能只取 low
            check(files.at(1).size == 0x1'0000'0002ull, ">4GB 大小 high/low 拼接正确");
            check(files.at(2).isDirectory, "第 3 项识别为目录");
            check(!files.at(2).sizeKnown, "没带 FD_FILESIZE 时 sizeKnown=false");
            for (int i = 0; i < 3; ++i)
                check(files.at(i).listIndex == quint32(i), "listIndex 等于原始下标");
        }
    }

    // --- cItems 与实际字节数不符：整条拒掉，不去读不存在的内存 ---
    {
        QString err;
        const QByteArray blob = makeFileList(
            {makeFileDescriptor(QStringLiteral("only-one.txt"), 1, true, false)}, 3);
        check(RdpClipboard::parseFileDescriptors(blob, &err).isEmpty(),
              "声明 3 项只给 1 项 → 拒绝");
        check(!err.isEmpty(), "字节数不符时给出错误原因");

        err.clear();
        check(RdpClipboard::parseFileDescriptors(QByteArray("ab"), &err).isEmpty(),
              "不足 4 字节 → 拒绝");
        check(!err.isEmpty(), "过短时给出错误原因");

        // cItems=0 是合法的（远端清空了剪贴板），不算错误
        err.clear();
        check(RdpClipboard::parseFileDescriptors(makeFileList({}, 0), &err).isEmpty(),
              "cItems=0 → 空清单");
        check(err.isEmpty(), "cItems=0 不算错误");
    }

    // --- 文件名写满 260 字符（字段内无终止符）：按长度停住，不越读到下一条 ---
    {
        const QString longName(260, QLatin1Char('a'));
        const QByteArray blob = makeFileList(
            {makeFileDescriptor(longName, 7, true, false),
             makeFileDescriptor(QStringLiteral("next.txt"), 8, true, false)},
            2);
        const QList<RdpRemoteFile> files = RdpClipboard::parseFileDescriptors(blob);
        check(files.size() == 2, "满长度文件名不影响后续条目");
        if (files.size() == 2) {
            check(files.at(0).name.size() == 260, "满长度名字读满 260 字符即止");
            check(files.at(1).name == QStringLiteral("next.txt"),
                  "第 2 项未被越读污染");
        }
    }

    // --- 路径穿越等非法名：整条丢弃，且 listIndex 不错位 ---
    {
        const QByteArray blob = makeFileList(
            {makeFileDescriptor(QStringLiteral("..\\..\\etc\\passwd"), 1, true, false),
             makeFileDescriptor(QStringLiteral("good.txt"), 2, true, false)},
            2);
        const QList<RdpRemoteFile> files = RdpClipboard::parseFileDescriptors(blob);
        check(files.size() == 1, "穿越路径条目被丢弃");
        if (files.size() == 1) {
            check(files.at(0).name == QStringLiteral("good.txt"), "留下的是合法项");
            // 关键：丢了第 0 项后它在列表里是第 0 个，但向远端请求内容必须报 1。
            // 拿列表下标去请求会取到被丢弃的那个文件。
            check(files.at(0).listIndex == 1, "丢弃条目后 listIndex 仍是原始下标 1");
        }
    }

    // --- sanitizeRemoteName 逐条：这些必须全部被拒 ---
    for (const char *bad : {"../../etc/passwd", "..\\..\\Windows\\System32\\evil.dll",
                            "/etc/passwd", "C:\\Windows\\notepad.exe", "..",
                            "a/../../b", "a/./b", ".", "", "/", "sub/../../x",
                            "bad\nname.txt", "trailingdot.", "trailing space ",
                            "co:lon.txt", "star*.txt", "que?.txt", "pipe|.txt",
                            "lt<.txt", "gt>.txt", "quote\".txt"})
        check(RdpClipboard::sanitizeRemoteName(QString::fromUtf8(bad)).isEmpty(),
              "非法远端文件名被拒");

    // --- 合法名要放过，别把闸门做成一律拒绝 ---
    check(RdpClipboard::sanitizeRemoteName(QStringLiteral("a.txt"))
              == QStringLiteral("a.txt"),
          "普通文件名通过");
    check(RdpClipboard::sanitizeRemoteName(QStringLiteral("dir\\sub\\a.txt"))
              == QStringLiteral("dir/sub/a.txt"),
          "多级相对路径通过并统一分隔符");
    check(RdpClipboard::sanitizeRemoteName(QStringLiteral("中文 文件-名_1.txt"))
              == QStringLiteral("中文 文件-名_1.txt"),
          "中文/空格/连字符文件名通过");
    check(RdpClipboard::sanitizeRemoteName(QStringLiteral("a\\\\b.txt"))
              == QStringLiteral("a/b.txt"),
          "重复分隔符被折叠");

    // --- 本地路径 → text/uri-list（winpr 合成器的入口格式）---
    {
        const QByteArray uris = RdpClipboard::buildUriList(
            {QStringLiteral("/tmp/a b.txt"), QString(), QStringLiteral("/tmp/中文.txt")});
        const QList<QByteArray> lines = uris.split('\n');
        // 每行 CRLF 结尾；空路径被跳过，故只有 2 行 + 末尾空串
        check(uris.endsWith("\r\n"), "uri-list 以 CRLF 结尾");
        check(lines.size() == 3, "空路径被跳过，只产出 2 行");
        check(uris.contains("file:///tmp/a%20b.txt"), "空格 percent-encode");
        check(!uris.contains("file:///tmp/中文.txt"), "非 ASCII 已编码而非原样");
        // 往回解析要拿回原路径：这条链是"本机文件 → 远端"的第一环，错了远端拿到
        // 的就是错文件名
        check(QUrl::fromEncoded(lines.at(0).trimmed()).toLocalFile()
                  == QStringLiteral("/tmp/a b.txt"),
              "uri-list 往回解析得到原路径");
        check(RdpClipboard::buildUriList({}).isEmpty(), "空列表产出空 uri-list");
    }

    // --- packFileList：补上 winpr 漏掉的 4 字节 cItems 头 -------------------
    // 这一组是「本机复制文件粘不到远端主机」的回归测试。winpr 的
    // ClipboardGetData(FileGroupDescriptorW) 返回的是**裸描述符数组**
    //（FreeRDP 3.30.0 实测 2 个文件 = 1184 = 2×592 字节，不带头），而
    // [MS-RDPECLIP] 2.2.5.2.3 要求前面有 cItems。原样发出去，远端会把第一条
    // 记录的 flags 当成条目数去解析。
    {
        const QByteArray one = makeFileDescriptor(QStringLiteral("a.txt"), 12, true, false);
        const QByteArray two =
            makeFileDescriptor(QStringLiteral("deep\\one.txt"), 5, true, false);
        const QByteArray bare = one + two;   // winpr 给的就是这个形状
        check(bare.size() == 2 * 592, "裸数组就是 N×592，没有头");

        // 裸数组直接交给解析器（= 修复前发给远端的字节）必须解不出来：头 4 字节
        // 是第一条的 flags(0x40)，被读成 cItems=64，与 1184 字节对不上。
        QString err;
        check(RdpClipboard::parseFileDescriptors(bare, &err).isEmpty(),
              "裸数组当 packed list 解析会失败（这正是修复前发出去的东西）");

        QString packErr;
        const QByteArray packed = RdpClipboard::packFileList(bare, &packErr);
        check(packed.size() == 4 + 2 * 592, "补头后 = 4 + N×592");
        check(packErr.isEmpty(), "补头无错");
        check(quint8(packed.at(0)) == 2 && packed.at(1) == '\0', "cItems 写成了 2");

        // 补完头就该能被自己的解析器读回来——两个方向对账。
        const QList<RdpRemoteFile> back = RdpClipboard::parseFileDescriptors(packed, &err);
        check(back.size() == 2, "补头后解析出 2 条");
        check(back.at(0).name == QStringLiteral("a.txt") && back.at(0).size == 12,
              "第 1 条名字与大小往返一致");
        check(back.at(1).name == QStringLiteral("deep/one.txt") && back.at(1).size == 5,
              "第 2 条名字与大小往返一致");

        // 已经带头（size%592==4）就原样透传，不能重复加。
        const QByteArray headed = makeFileList({one, two}, 2);
        check(RdpClipboard::packFileList(headed) == headed, "已带头的原样透传");

        // 自带头但 cItems 与字节数不符 → 宁可回 FAIL 也不发畸形清单。
        QString badErr;
        check(RdpClipboard::packFileList(makeFileList({one, two}, 7), &badErr).isEmpty(),
              "自带头但 cItems 不自洽被拒");
        check(badErr.contains(QStringLiteral("不自洽")), "拒绝原因写明了");

        // 字节数凑不成整条记录 / 空输入 → 拒绝并写明原因。
        badErr.clear();
        check(RdpClipboard::packFileList(QByteArray(600, '\0'), &badErr).isEmpty(),
              "600 字节既非 N×592 也非 4+N×592，被拒");
        check(!badErr.isEmpty(), "非整数倍有原因");
        check(RdpClipboard::packFileList(QByteArray(), &badErr).isEmpty(), "空输入被拒");
    }

    // --- chunkWriteLen：取回时"绝不写超过声明大小" ---------------------------
    // 这组是真机上抓到的那次静默损坏的回归闸门。原来取长度取的是
    // common.dataLen（含 4 字节 streamId 的整个 PDU 长度）而不是 cbRequested，
    // 每块多写 4 字节、下一块又从多算过的偏移去要，结果每个 256 KiB 接缝上恰好
    // 4 个真字节被替换、文件比声明大 4 字节。13 MB 的 zip 里 410 个条目有 26 个
    // CRC 报错，且**全部**是跨接缝的条目——不跨接缝的一个都没坏，所以表面上
    // "文件能打开"，极难察觉。这里守住的是那条不变式：多给的必须截掉。
    {
        std::printf("RdpClipboard::chunkWriteLen（落盘长度闸门）\n");
        constexpr quint64 sz = 1000;

        // 正常情况原样放过
        check(RdpClipboard::chunkWriteLen(sz, 0, 256) == 256u, "块小于剩余量时原样落盘");
        check(RdpClipboard::chunkWriteLen(sz, 744, 256) == 256u, "最后一块正好补满");

        // 远端多给（含 dataLen 那 4 字节的情形）必须截掉，而不是写进文件
        check(RdpClipboard::chunkWriteLen(sz, 0, 1004) == 1000u,
              "单块多给 4 字节被截掉（正是 dataLen/cbRequested 那个坑）");
        check(RdpClipboard::chunkWriteLen(sz, 996, 8) == 4u, "尾块多给被截到剩余量");
        check(RdpClipboard::chunkWriteLen(sz, 0, 0xffffffffu) == 1000u, "超大响应被截到声明大小");

        // 已经写满/写溢出后一律 0：不能再往文件里追加
        check(RdpClipboard::chunkWriteLen(sz, 1000, 256) == 0u, "写满后不再落盘");
        check(RdpClipboard::chunkWriteLen(sz, 1200, 256) == 0u, "已溢出时也返回 0");

        // 声明大小为 0（空文件）不该落任何字节
        check(RdpClipboard::chunkWriteLen(0, 0, 256) == 0u, "空文件不落盘");

        // 4 GB 以上：quint64 算术不能被截成 32 位
        constexpr quint64 big = 5ull * 1024 * 1024 * 1024;
        check(RdpClipboard::chunkWriteLen(big, 0, 262144) == 262144u, ">4GB 文件首块正常");
        check(RdpClipboard::chunkWriteLen(big, big - 10, 262144) == 10u,
              ">4GB 文件尾块按剩余量截断");
    }
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
    testRdpResolution();
    testRdpAutoFetch();
    testRdpClipboard();
#endif

    if (g_failures == 0) {
        std::printf("platform_test: all checks passed\n");
        return 0;
    }
    std::printf("platform_test: %d check(s) failed\n", g_failures);
    return 1;
}
