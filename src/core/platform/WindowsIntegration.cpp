// WindowsIntegration.cpp — Windows 右键菜单/开机自启（注册表实现 + 非 Win stub）。
// 对应Python: core/windows_integration.py

#include "WindowsIntegration.h"

#ifdef _WIN32
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#endif

namespace cubeshell {
namespace WindowsIntegration {

#ifdef _WIN32

namespace {

// 对应Python: install() 里的 display_name
const QString kDisplayName = QStringLiteral("在 CubeShell 中打开终端");
// 注册表路径。QSettings NativeFormat 下 '/' 为层级分隔符，"Default" 表示默认值。
const QString kClassesRoot = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes");
const QString kMenuKey = QStringLiteral("Directory/shell/OpenInCubeShell");
const QString kBackgroundMenuKey =
    QStringLiteral("Directory/Background/shell/OpenInCubeShell");
const QString kRunKey =
    QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kRunValueName = QStringLiteral("CubeShell");

// 对应Python: _get_exe_path（C++ 下可执行文件即 exe 本身，无 frozen 分支）
QString exePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

// 对应Python: _get_command_template（打包分支）'"{exe_path}" "{path_var}"'
QString commandTemplate(const QString &pathVar)
{
    return QStringLiteral("\"%1\" \"%2\"").arg(exePath(), pathVar);
}

// 对应Python: _get_icon_path（打包分支）'"{exe_path}",0'
QString iconValue()
{
    return QStringLiteral("\"%1\",0").arg(exePath());
}

// 对应Python: _create_context_menu_entry
void createContextMenuEntry(QSettings &classes, const QString &basePath,
                            const QString &command)
{
    classes.setValue(basePath + QStringLiteral("/Default"), kDisplayName);
    classes.setValue(basePath + QStringLiteral("/Icon"), iconValue());
    classes.setValue(basePath + QStringLiteral("/command/Default"), command);
}

} // namespace

bool isSupported()
{
    // 对应Python: platform.system() == 'Windows'
    return true;
}

bool isInstalled()
{
    // 对应Python: is_installed（winreg.OpenKey 探测键是否存在）
    QSettings classes(kClassesRoot, QSettings::NativeFormat);
    return classes.contains(kMenuKey + QStringLiteral("/Default"))
        || classes.contains(kMenuKey + QStringLiteral("/command/Default"));
}

bool install(QString *errorMessage)
{
    // 对应Python: install()（Directory\shell 用 %1，Background 用 %V）
    QSettings classes(kClassesRoot, QSettings::NativeFormat);
    createContextMenuEntry(classes, kMenuKey, commandTemplate(QStringLiteral("%1")));
    createContextMenuEntry(classes, kBackgroundMenuKey,
                           commandTemplate(QStringLiteral("%V")));
    classes.sync();
    if (classes.status() != QSettings::NoError) {
        if (errorMessage)
            *errorMessage = QStringLiteral("写入注册表失败（HKCU\\Software\\Classes）");
        return false;
    }
    return true;
}

bool uninstall(QString *errorMessage)
{
    // 对应Python: uninstall() / _delete_registry_tree（不存在视为成功）
    QSettings classes(kClassesRoot, QSettings::NativeFormat);
    classes.remove(kMenuKey);
    classes.remove(kBackgroundMenuKey);
    classes.sync();
    if (classes.status() != QSettings::NoError) {
        if (errorMessage)
            *errorMessage = QStringLiteral("删除注册表键失败（HKCU\\Software\\Classes）");
        return false;
    }
    return true;
}

bool isAutoStartEnabled()
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    const QString value = run.value(kRunValueName).toString();
    // 与 url_scheme_register.py 的校验思路一致：值里须包含当前 exe 路径
    return !value.isEmpty() && value.contains(exePath(), Qt::CaseInsensitive);
}

bool setAutoStart(bool enabled, QString *errorMessage)
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    if (enabled)
        run.setValue(kRunValueName, QStringLiteral("\"%1\"").arg(exePath()));
    else
        run.remove(kRunValueName);
    run.sync();
    if (run.status() != QSettings::NoError) {
        if (errorMessage)
            *errorMessage = QStringLiteral("写入注册表失败（HKCU Run 键）");
        return false;
    }
    return true;
}

#else // !_WIN32 — 非 Windows stub（空实现/返回 false）

namespace {

bool notSupported(QString *errorMessage)
{
    // 对应Python: "winreg 模块不可用（仅 Windows 支持）"
    if (errorMessage)
        *errorMessage = QStringLiteral("Windows 集成仅支持 Windows");
    return false;
}

} // namespace

bool isSupported()
{
    return false;
}

bool isInstalled()
{
    return false;
}

bool install(QString *errorMessage)
{
    return notSupported(errorMessage);
}

bool uninstall(QString *errorMessage)
{
    return notSupported(errorMessage);
}

bool isAutoStartEnabled()
{
    return false;
}

bool setAutoStart(bool enabled, QString *errorMessage)
{
    Q_UNUSED(enabled);
    return notSupported(errorMessage);
}

#endif // _WIN32

} // namespace WindowsIntegration
} // namespace cubeshell
