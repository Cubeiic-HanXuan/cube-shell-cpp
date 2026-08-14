// terminal_theme_util.h — 终端配色方案切换后的持久化与广播。
//
// 用户在终端右键菜单「切换终端主题」里选了新配色后，QTermWidget 只改了
// 自己的显示并发出 colorSchemeChanged(name)；本 helper 负责剩下的两件事：
//   1. 写入 GlobalState 并落盘 theme.json（否则下次启动被还原成旧主题）；
//   2. 把新配色应用到当前窗口所有已打开的终端，避免各标签页配色不一致。
//
// 对应Python: cube-shell.py::apply_theme
//   （util.THEME['terminal_theme'] = name + write_json + 全终端刷新）

#pragma once

#include <QWidget>

#include "config/GlobalState.h"
#include "qtermwidget.h"

namespace cubeshell {

// context 用于定位顶层窗口以枚举全部终端；传 nullptr 时只持久化不广播。
inline void applyTerminalThemeEverywhere(const QString &name, QWidget *context)
{
    GlobalState::instance().setTerminalTheme(name);
    // 立即落盘：用户切换即生效，重启后保持（与 SettingsDialog 的保存路径一致）。
    GlobalState::instance().saveTheme();

    QWidget *root = context ? context->window() : nullptr;
    if (!root)
        return;
    const QList<QTermWidget *> terms = root->findChildren<QTermWidget *>();
    for (QTermWidget *t : terms)
        t->setColorScheme(name);
}

} // namespace cubeshell
