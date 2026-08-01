#pragma once

// WindowsIntegration.h — Windows 资源管理器右键菜单 + 开机自启集成。
//
// C++ port of core/windows_integration.py. 通过 HKCU 注册表：
//   - Software\Classes\Directory\shell\OpenInCubeShell（右键点击文件夹）
//   - Software\Classes\Directory\Background\shell\OpenInCubeShell（空白处右键）
//     各含 (Default)=显示名、Icon、command\(Default)="exe" "%1"/"%V"
//   - Software\Microsoft\Windows\CurrentVersion\Run（开机自启，Python 版无）
//
// 平台约定：实现里用 #ifdef _WIN32 条件编译；非 Windows 下所有函数为 stub
//（返回 false / 填充"仅 Windows 支持"错误），保证跨平台链接。
//
// 注：Python 版的 _is_frozen/_get_exe_path 用于区分 Nuitka 打包与源码运行；
// C++ 可执行文件天然就是 exe 本身，直接用 QCoreApplication::applicationFilePath()。

#include <QString>

namespace cubeshell {
namespace WindowsIntegration {

// 对应Python: core/windows_integration.py::is_supported
// 是否为支持的平台（仅 Windows 返回 true）。
bool isSupported();

// 对应Python: core/windows_integration.py::is_installed
// 检查右键菜单项是否已注册（Directory\shell\OpenInCubeShell 键存在）。
bool isInstalled();

// 对应Python: core/windows_integration.py::install
// 注册右键菜单（Directory\shell + Directory\Background\shell 两处）。
// 失败时返回 false，*errorMessage（可为 nullptr）填充原因。
bool install(QString *errorMessage = nullptr);

// 对应Python: core/windows_integration.py::uninstall
// 删除两处右键菜单注册（不存在视为成功）。
bool uninstall(QString *errorMessage = nullptr);

// Windows 专属扩展（Python 版无对应）：开机自启（HKCU Run 键）。
bool isAutoStartEnabled();
bool setAutoStart(bool enabled, QString *errorMessage = nullptr);

} // namespace WindowsIntegration
} // namespace cubeshell
