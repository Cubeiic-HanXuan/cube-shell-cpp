#pragma once

// PlatformIntegration.h — 平台集成跨平台门面（facade）。
//
// 统一封装"文件管理器右键菜单集成"的按平台分发：
//   - macOS   → FinderIntegration（Finder 快速操作 workflow）
//   - Windows → WindowsIntegration（资源管理器右键菜单注册表项）
//   - 其他    → 不支持（返回 false）
// URL Scheme 注册见 UrlSchemeRegistrar（本身即跨平台统一接口）。
//
// 对应Python: ui/设置页对 core/finder_integration.py 与
// core/windows_integration.py 的按 platform.system() 调用分发。

#include <QString>

#include "FinderIntegration.h"
#include "WindowsIntegration.h"

namespace cubeshell {

class PlatformIntegration {
public:
    PlatformIntegration() = delete;

    // 当前平台是否支持文件管理器右键菜单集成。
    // 对应Python: finder_integration.is_supported / windows_integration.is_supported
    static bool isContextMenuSupported()
    {
#if defined(__APPLE__)
        return FinderIntegration::isSupported();
#elif defined(_WIN32)
        return WindowsIntegration::isSupported();
#else
        return false;
#endif
    }

    // 右键菜单是否已安装。
    // 对应Python: finder_integration.is_installed / windows_integration.is_installed
    static bool isContextMenuInstalled()
    {
#if defined(__APPLE__)
        return FinderIntegration::isInstalled();
#elif defined(_WIN32)
        return WindowsIntegration::isInstalled();
#else
        return false;
#endif
    }

    // 安装右键菜单（"在 CubeShell 中打开终端"）。
    // 对应Python: finder_integration.install / windows_integration.install
    static bool installContextMenu(QString *errorMessage = nullptr)
    {
#if defined(__APPLE__)
        return FinderIntegration::installFinderExtension(errorMessage);
#elif defined(_WIN32)
        return WindowsIntegration::install(errorMessage);
#else
        if (errorMessage)
            *errorMessage = QStringLiteral("当前平台不支持右键菜单集成");
        return false;
#endif
    }

    // 卸载右键菜单。
    // 对应Python: finder_integration.uninstall / windows_integration.uninstall
    static bool uninstallContextMenu(QString *errorMessage = nullptr)
    {
#if defined(__APPLE__)
        return FinderIntegration::uninstallFinderExtension(errorMessage);
#elif defined(_WIN32)
        return WindowsIntegration::uninstall(errorMessage);
#else
        if (errorMessage)
            *errorMessage = QStringLiteral("当前平台不支持右键菜单集成");
        return false;
#endif
    }
};

} // namespace cubeshell
