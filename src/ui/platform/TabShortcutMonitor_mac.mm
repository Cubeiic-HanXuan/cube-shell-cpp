// TabShortcutMonitor_mac.mm — 在系统菜单导航截走 ⌃+Tab 之前，用 NSEvent 本地监视器
// 捕获 https://developer.apple.com/documentation/appkit/nsevent/1534751-addlocalmonitorforevents 原始事件。
// 仅在 APPLE 下编译（由 src/ui/CMakeLists.txt 的 if(APPLE) 分支加入源列表）。

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h> // kVK_Tab 虚拟键码

#include "TabShortcutMonitor.h"

#include <functional>

namespace cubeshell {
namespace {

struct TabShortcutState {
    id monitor = nil;                 // 本地监视器句柄
    std::function<void(bool)> onCycle; // 切换回调
};

TabShortcutState &state()
{
    static TabShortcutState s;
    return s;
}

} // namespace

void removeMacTabShortcutMonitor()
{
    if (state().monitor) {
        [NSEvent removeMonitor:state().monitor];
        state().monitor = nil;
    }
    state().onCycle = nullptr;
}

void installMacTabShortcutMonitor(const std::function<void(bool)> &onCycle)
{
    removeMacTabShortcutMonitor();
    state().onCycle = onCycle;

    // 传指针到函数级静态回调，处理器在主线程执行；remove() 后 onCycle 变空，
    // handler 里调用前做空检查即可。局部监视器只对当前进程主线程事件流生效，
    // 不会像全局监视器那样要求“辅助功能”权限。
    const auto *cb = &state().onCycle;
    state().monitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                              handler:^NSEvent *(NSEvent *event) {
                                                    if ([event type] != NSEventTypeKeyDown)
                                                        return event;
                                                    const NSUInteger mods =
                                                        [event modifierFlags] &
                                                        NSEventModifierFlagDeviceIndependentFlagsMask;
                                                    if ((mods & NSEventModifierFlagControl) == 0 ||
                                                        [event keyCode] != kVK_Tab)
                                                        return event;
                                                    const BOOL backward =
                                                        (mods & NSEventModifierFlagShift) != 0;
                                                    // 长按不重复切换（isRepeat=true 时仅吞掉，
                                                    // 不触发回调，避免卡死循环切换）。
                                                    if (![event isARepeat] && cb && *cb)
                                                        (*cb)(backward);
                                                    // 返回 nil：彻底吞掉该按键，阻止菜单栏
                                                    // 键盘导航截走 ⌃+Tab（也就是之前“菜单闪一下”的来源）。
                                                    return nil;
                                                }];
}

} // namespace cubeshell

#endif // __APPLE__