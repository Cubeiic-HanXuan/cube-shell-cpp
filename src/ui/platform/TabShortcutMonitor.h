// TabShortcutMonitor.h — macOS ⌃+Tab / ⌃⇧+Tab 原生捕获（Objective-C++ 实现）。
// 理由：macOS 菜单栏“键盘导航”会在系统层截走 ⌃+Tab 做“移动焦点”，并把原始
// ⌃ 修饰符剥掉（到达应用的是一个 mods=0 的裸 Tab），导致 Qt 的 QShortcut/
// QAction/事件过滤一律无法匹配。故与 SecureCRT 相同，用一个 NSEvent 本地监视器
// 在系统菜单导航之前拿到带 ⌃ 修饰符的原始事件并切换标签。
#pragma once

#include <functional>

#ifdef Q_OS_MACOS

namespace cubeshell {

// 安装本地监视器：拦截 ⌃+Tab / ⌃⇧+Tab。
//   backward=false → 下一个标签；backward=true → 上一个标签。
// 回调在主线程执行（本地事件监视器天然跑在主线程）。
// 重复调用会先移除之前的监视器。
void installMacTabShortcutMonitor(const std::function<void(bool backward)> &onCycle);

// 移除监视器（内部自幂等；MainWindow 析构时调用）。
void removeMacTabShortcutMonitor();

} // namespace cubeshell

#endif // Q_OS_MACOS
