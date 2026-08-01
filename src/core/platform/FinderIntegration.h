#pragma once

// FinderIntegration.h — macOS Finder 右键菜单集成（"快速操作" Quick Action）。
//
// C++ port of core/finder_integration.py. 安装/卸载
// ~/Library/Services/Open in CubeShell.workflow，使用户可在 Finder 中右键
// 点击文件夹选择"在 CubeShell 中打开终端"——workflow 内的 shell 脚本通过
// `open "cubeshell://open-local?path=<encoded>"` 启动应用并传递路径参数。
//
// 平台约定：
//   - 真实实现位于 FinderIntegration_mac.mm（Objective-C++，仅 APPLE 编译），
//     额外提供 Launch Services 注册与桌面快捷方式创建（Python 版没有）。
//   - 非 macOS 平台编译 FinderIntegration.cpp 空 stub（返回 false）。

#include <QString>

namespace cubeshell {
namespace FinderIntegration {

// 对应Python: core/finder_integration.py::WORKFLOW_DIR
// ~/Library/Services/Open in CubeShell.workflow 的绝对路径。
QString workflowPath();

// 对应Python: core/finder_integration.py::is_supported
// 是否为支持的平台（仅 macOS 返回 true）。
bool isSupported();

// 对应Python: core/finder_integration.py::is_installed
// 检查 Finder 快速操作是否已安装（workflow 目录存在）。
bool isInstalled();

// 对应Python: core/finder_integration.py::install
// 安装 Finder 快速操作 workflow（写入 Info.plist + document.wflow），并把
// 当前应用 bundle 注册到 Launch Services（保证 cubeshell:// 能唤起本应用）。
// 失败时返回 false，*errorMessage（可为 nullptr）填充原因。
// 对应 Python 返回 tuple (success, error_message)。
bool installFinderExtension(QString *errorMessage = nullptr);

// 对应Python: core/finder_integration.py::uninstall
// 卸载 Finder 快速操作 workflow（删除 workflow 目录，不存在视为成功）。
bool uninstallFinderExtension(QString *errorMessage = nullptr);

// macOS 专属扩展（Python 版无对应）：在 ~/Desktop 创建指向当前应用
// bundle 的快捷方式（符号链接）。非 bundle 运行（裸可执行文件）时链接
// 到可执行文件本身。
bool createDesktopShortcut(QString *errorMessage = nullptr);

} // namespace FinderIntegration
} // namespace cubeshell
