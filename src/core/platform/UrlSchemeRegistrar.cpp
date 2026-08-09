// UrlSchemeRegistrar.cpp — URL Scheme 跨平台注册实现。
// 对应Python: core/url_dispatch/url_scheme_register.py

#include "UrlSchemeRegistrar.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#ifndef CUBESHELL_PLATFORM_OHOS
#include <QProcess>
#endif

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace cubeshell {

namespace {

#ifndef CUBESHELL_PLATFORM_OHOS
// 对应Python: _get_exe_path（C++ 下无 frozen 分支，可执行文件即自身）
// 鸿蒙：无任何使用方（公开接口直接短路返回），不编译。
QString exePath()
{
    return QCoreApplication::applicationFilePath();
}

// 注册表/Info.plist 里的协议描述文案。
// 对应Python: _register_windows 里的 "CubeShell JMS Protocol" 等
QString schemeDescription(const QString &scheme)
{
    if (scheme == QLatin1String("jms"))
        return QStringLiteral("CubeShell JMS Protocol");
    if (scheme == QLatin1String("cubeshell"))
        return QStringLiteral("CubeShell Local Terminal");
    if (scheme == QLatin1String("ssh"))
        return QStringLiteral("CubeShell SSH Protocol");
    if (scheme == QLatin1String("telnet"))
        return QStringLiteral("CubeShell Telnet Protocol");
    return QStringLiteral("CubeShell %1 Protocol").arg(scheme);
}
#endif // !CUBESHELL_PLATFORM_OHOS

// macOS 分支在鸿蒙上不编译：鸿蒙无 bundle/PlistBuddy/lsregister，
// 且 Q_OS_MACOS 与 CUBESHELL_PLATFORM_OHOS 不会同时出现在真实工具链下
//（此处防御的是 OHOS 宏开在 macOS 宿主机上的交叉配置检查场景）。
#if defined(Q_OS_MACOS) && !defined(CUBESHELL_PLATFORM_OHOS)

// 当前 .app bundle 的 Info.plist 路径；非 bundle 运行（裸可执行文件）返回空。
QString bundleInfoPlistPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.endsWith(QLatin1String("/Contents/MacOS")))
        return QString();
    return QFileInfo(appDir + QStringLiteral("/../Info.plist")).canonicalFilePath();
}

QString bundlePath()
{
    const QString plist = bundleInfoPlistPath();
    if (plist.isEmpty())
        return QString();
    // .../CubeShell.app/Contents/Info.plist → .../CubeShell.app
    return QFileInfo(QFileInfo(plist).path() + QStringLiteral("/..")).canonicalFilePath();
}

// 运行 /usr/libexec/PlistBuddy -c "<command>" <plist>，可选取回 stdout。
bool runPlistBuddy(const QString &command, const QString &plist, QString *output = nullptr)
{
    QProcess proc;
    proc.start(QStringLiteral("/usr/libexec/PlistBuddy"),
               {QStringLiteral("-c"), command, plist});
    if (!proc.waitForFinished(10000))
        return false;
    if (output)
        *output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

// 强制把 bundle 重新登记进 Launch Services 数据库（Info.plist 变更后必须刷新）。
void refreshLaunchServices()
{
    const QString bundle = bundlePath();
    if (bundle.isEmpty())
        return;
    const QString lsregister = QStringLiteral(
        "/System/Library/Frameworks/CoreServices.framework"
        "/Frameworks/LaunchServices.framework/Support/lsregister");
    QProcess::execute(lsregister, {QStringLiteral("-f"), bundle});
}

// Info.plist 的 CFBundleURLTypes 里是否已声明 scheme。XML plist 直接做文本
// 探测（CMake/PlistBuddy 均产出 XML 格式）。
bool plistContainsScheme(const QString &plist, const QString &scheme)
{
    QFile file(plist);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    return content.contains(QLatin1String("CFBundleURLTypes"))
        && content.contains(QStringLiteral("<string>%1</string>").arg(scheme));
}

// 对应Python: is_registered 的 Darwin 分支（Python 恒 True——由打包期
// Info.plist 保证；C++ 侧真实探测 bundle Info.plist，开发裸二进制返回 false）
bool isRegisteredMac(const QStringList &schemes)
{
    const QString plist = bundleInfoPlistPath();
    if (plist.isEmpty())
        return false;
    for (const QString &scheme : schemes) {
        if (!plistContainsScheme(plist, scheme))
            return false;
    }
    return true;
}

// 开发构建补救路径：把缺失的 scheme 追加进 bundle Info.plist（PlistBuddy），
// 再 lsregister 刷新。打包构建由 packaging/Info.plist.in 在配置期写好。
bool registerMac(const QStringList &schemes)
{
    const QString plist = bundleInfoPlistPath();
    if (plist.isEmpty())
        return false; // 裸可执行文件无法承载 CFBundleURLTypes

    bool changed = false;
    for (const QString &scheme : schemes) {
        if (plistContainsScheme(plist, scheme))
            continue;
        // 数组不存在则创建（已存在时 Add 失败，忽略）
        runPlistBuddy(QStringLiteral("Add :CFBundleURLTypes array"), plist);
        // 统一头插（index 0），避免解析既有条目数
        if (!runPlistBuddy(QStringLiteral("Add :CFBundleURLTypes:0 dict"), plist))
            return false;
        runPlistBuddy(QStringLiteral("Add :CFBundleURLTypes:0:CFBundleURLName string %1")
                          .arg(schemeDescription(scheme)), plist);
        runPlistBuddy(QStringLiteral("Add :CFBundleURLTypes:0:CFBundleURLSchemes array"),
                      plist);
        if (!runPlistBuddy(
                QStringLiteral("Add :CFBundleURLTypes:0:CFBundleURLSchemes:0 string %1")
                    .arg(scheme), plist))
            return false;
        changed = true;
    }
    if (changed)
        refreshLaunchServices();
    return isRegisteredMac(schemes);
}

bool unregisterMac(const QStringList &schemes)
{
    const QString plist = bundleInfoPlistPath();
    if (plist.isEmpty())
        return true; // 无 bundle 即无注册，视为成功

    // 逐个扫描 CFBundleURLTypes 条目，命中 scheme 则整条删除后重扫
    //（删除会使后续索引前移）。
    bool changed = false;
    for (const QString &scheme : schemes) {
        bool removedOne = true;
        while (removedOne) {
            removedOne = false;
            for (int i = 0; i < 32; ++i) {
                QString value;
                if (!runPlistBuddy(
                        QStringLiteral("Print :CFBundleURLTypes:%1:CFBundleURLSchemes:0")
                            .arg(i), plist, &value))
                    break; // 越界：条目扫完
                if (value == scheme) {
                    runPlistBuddy(QStringLiteral("Delete :CFBundleURLTypes:%1").arg(i),
                                  plist);
                    changed = true;
                    removedOne = true;
                    break;
                }
            }
        }
    }
    if (changed)
        refreshLaunchServices();
    return true;
}

#elif defined(Q_OS_WIN)

// 对应Python: _is_registered_windows
bool isRegisteredWin(const QStringList &schemes)
{
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    const QString exe = exePath();
    for (const QString &scheme : schemes) {
        const QString value = classes
            .value(scheme + QStringLiteral("/shell/open/command/Default"))
            .toString();
        // 注册表中的命令格式为 "exe_path" "%1"
        if (!value.contains(exe, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

// 对应Python: _register_windows_scheme / _register_windows
bool registerWin(const QStringList &schemes, const QString &exe)
{
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    const QString nativeExe = QDir::toNativeSeparators(exe);
    for (const QString &scheme : schemes) {
        // 协议根键（URL Protocol 空值声明这是 URL 协议）
        classes.setValue(scheme + QStringLiteral("/Default"),
                         QStringLiteral("URL:%1").arg(schemeDescription(scheme)));
        classes.setValue(scheme + QStringLiteral("/URL Protocol"), QString());
        // DefaultIcon
        classes.setValue(scheme + QStringLiteral("/DefaultIcon/Default"),
                         QStringLiteral("\"%1\",0").arg(nativeExe));
        // shell\open\command
        classes.setValue(scheme + QStringLiteral("/shell/open/command/Default"),
                         QStringLiteral("\"%1\" \"%2\"").arg(nativeExe,
                                                             QStringLiteral("%1")));
    }
    classes.sync();
    return classes.status() == QSettings::NoError;
}

bool unregisterWin(const QStringList &schemes)
{
    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                      QSettings::NativeFormat);
    for (const QString &scheme : schemes)
        classes.remove(scheme);
    classes.sync();
    return classes.status() == QSettings::NoError;
}

#elif defined(CUBESHELL_PLATFORM_OHOS)
// 鸿蒙：无 .desktop/xdg-mime 概念（系统交互走 Want/Uri），Linux 分支的
// 辅助函数与 QProcess 调用整体不编译；公开接口在下方直接返回 true。

#else // Linux / other Unix

// 对应Python: _register_linux 里的 desktop 文件路径
QString desktopFilePath()
{
    return QDir::homePath()
        + QStringLiteral("/.local/share/applications/cube-shell-url-handler.desktop");
}

// 对应Python: _is_registered_linux
bool isRegisteredLinux(const QStringList &schemes)
{
    QFile file(desktopFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    if (!content.contains(exePath()))
        return false;
    for (const QString &scheme : schemes) {
        if (!content.contains(QStringLiteral("x-scheme-handler/%1").arg(scheme)))
            return false;
    }
    return true;
}

// 对应Python: _register_linux（.desktop 文件 + xdg-mime + update-desktop-database）
bool registerLinux(const QStringList &schemes, const QString &exe)
{
    const QString applicationsDir = QFileInfo(desktopFilePath()).path();
    if (!QDir().mkpath(applicationsDir))
        return false;

    QStringList mimeTypes;
    for (const QString &scheme : schemes)
        mimeTypes << QStringLiteral("x-scheme-handler/%1").arg(scheme);

    // 对应Python: desktop_content（MimeType 由 scheme 列表拼出）
    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Name=CubeShell URL Handler\n"
        "Comment=Handle jms:// and cubeshell:// URLs for CubeShell\n"
        "Exec=%1 %u\n"
        "Terminal=false\n"
        "Type=Application\n"
        "NoDisplay=true\n"
        "MimeType=%2;\n"
        "Categories=Network;\n").arg(exe, mimeTypes.join(QLatin1Char(';')));

    QFile file(desktopFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(content.toUtf8());
    file.close();

    // 注册为各协议的默认处理程序（失败静默，与 Python check=False 一致）
    for (const QString &mime : mimeTypes) {
        QProcess::execute(QStringLiteral("xdg-mime"),
                          {QStringLiteral("default"),
                           QStringLiteral("cube-shell-url-handler.desktop"), mime});
    }
    // 更新桌面数据库
    QProcess::execute(QStringLiteral("update-desktop-database"), {applicationsDir});
    return true;
}

bool unregisterLinux(const QStringList &schemes)
{
    Q_UNUSED(schemes);
    QFile file(desktopFilePath());
    if (file.exists() && !file.remove())
        return false;
    QProcess::execute(QStringLiteral("update-desktop-database"),
                      {QFileInfo(desktopFilePath()).path()});
    return true;
}

#endif

} // namespace

QStringList UrlSchemeRegistrar::defaultSchemes()
{
    // 对应Python: ("jms", "cubeshell")；ssh 按 conf/macos_url_scheme.plist
    // 的注释状态默认不启用，调用方可显式传入。
    // telnet 默认启用：它是 IANA 在案的标准 scheme，网页/文档里的 telnet://
    // 链接本来就期望被终端工具接管，与 ssh:// 的暧昧状态不同。
    return {QStringLiteral("jms"), QStringLiteral("cubeshell"),
            QStringLiteral("telnet")};
}

bool UrlSchemeRegistrar::isRegistered(const QStringList &schemes)
{
#if defined(CUBESHELL_PLATFORM_OHOS)
    // 鸿蒙：URL/深度链接经应用市场 HAP 声明，系统侧无桌面式 scheme 注册概念；
    // 按「已注册」处理，避免启动期反复尝试写桌面文件。
    Q_UNUSED(schemes);
    return true;
#else
    const QStringList effective = schemes.isEmpty() ? defaultSchemes() : schemes;
#if defined(Q_OS_MACOS)
    return isRegisteredMac(effective);
#elif defined(Q_OS_WIN)
    return isRegisteredWin(effective);
#else
    return isRegisteredLinux(effective);
#endif
#endif
}

bool UrlSchemeRegistrar::registerSchemes(const QStringList &schemes,
                                         const QString &exePathOverride)
{
#if defined(CUBESHELL_PLATFORM_OHOS)
    Q_UNUSED(schemes);
    Q_UNUSED(exePathOverride);
    return true;
#else
    const QStringList effective = schemes.isEmpty() ? defaultSchemes() : schemes;
    const QString exe = exePathOverride.isEmpty() ? exePath() : exePathOverride;
#if defined(Q_OS_MACOS)
    Q_UNUSED(exe); // macOS 由 bundle Info.plist 承载，无需 exe 路径
    return registerMac(effective);
#elif defined(Q_OS_WIN)
    return registerWin(effective, exe);
#else
    return registerLinux(effective, exe);
#endif
#endif
}

bool UrlSchemeRegistrar::unregisterSchemes(const QStringList &schemes)
{
#if defined(CUBESHELL_PLATFORM_OHOS)
    Q_UNUSED(schemes);
    return true;
#else
    const QStringList effective = schemes.isEmpty() ? defaultSchemes() : schemes;
#if defined(Q_OS_MACOS)
    return unregisterMac(effective);
#elif defined(Q_OS_WIN)
    return unregisterWin(effective);
#else
    return unregisterLinux(effective);
#endif
#endif
}

void UrlSchemeRegistrar::ensureRegistered()
{
    // 对应Python: ensure_registered（静默执行，绝不影响应用正常启动）
    if (isRegistered())
        return;
    if (registerSchemes())
        qInfo("[CubeShell] URL Schemes (jms://, cubeshell://) registered successfully.");
    else
        qWarning("[CubeShell] Warning: Failed to register URL Schemes.");
}

} // namespace cubeshell
