// FinderIntegration.cpp — 非 macOS 平台的空 stub。
// 对应Python: core/finder_integration.py（is_supported 在非 Darwin 返回 False）
//
// 真实实现见 FinderIntegration_mac.mm；本文件只在非 APPLE 平台加入源列表
//（cpp/src/core/CMakeLists.txt），保证跨平台链接不缺符号。

#ifndef __APPLE__

#include "FinderIntegration.h"

namespace cubeshell {
namespace FinderIntegration {

namespace {

// 统一的"平台不支持"错误填充。
bool notSupported(QString *errorMessage)
{
    if (errorMessage)
        *errorMessage = QStringLiteral("Finder 集成仅支持 macOS");
    return false;
}

} // namespace

QString workflowPath()
{
    return QString();
}

bool isSupported()
{
    // 对应Python: platform.system() == 'Darwin' → False
    return false;
}

bool isInstalled()
{
    return false;
}

bool installFinderExtension(QString *errorMessage)
{
    return notSupported(errorMessage);
}

bool uninstallFinderExtension(QString *errorMessage)
{
    return notSupported(errorMessage);
}

bool createDesktopShortcut(QString *errorMessage)
{
    return notSupported(errorMessage);
}

} // namespace FinderIntegration
} // namespace cubeshell

#endif // !__APPLE__
