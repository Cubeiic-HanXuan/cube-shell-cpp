#pragma once

// UrlSchemeRegistrar.h — jms:// / ssh:// / cubeshell:// URL Scheme 系统注册。
//
// C++ port of core/url_dispatch/url_scheme_register.py. 让浏览器点击
// jms://... 链接能唤起 CubeShell。各平台机制：
//   - macOS  : 首选打包期由 packaging/Info.plist.in 写入 CFBundleURLTypes
//              （系统扫描 .app 自动登记）。运行期作为开发构建的补救路径：
//              用 PlistBuddy 把缺失的 scheme 写入 bundle Info.plist，再用
//              lsregister 强制刷新 Launch Services 数据库。
//   - Windows: 写 HKCU\Software\Classes\<scheme> 协议键
//              （URL Protocol + DefaultIcon + shell\open\command）。
//   - Linux  : 生成 ~/.local/share/applications/cube-shell-url-handler.desktop
//              + xdg-mime default 注册 x-scheme-handler/<scheme>。
//
// 统一接口：registerSchemes() / unregisterSchemes() / isRegistered()。
// 默认 scheme 集合与 Python 版一致为 {jms, cubeshell}；ssh 可按需传入
//（conf/macos_url_scheme.plist 中 ssh 亦为注释保留状态）。

#include <QString>
#include <QStringList>

namespace cubeshell {

class UrlSchemeRegistrar {
public:
    // 对应Python: register/_register_windows 里写死的 ("jms", "cubeshell")
    static QStringList defaultSchemes();

    // 对应Python: core/url_dispatch/url_scheme_register.py::is_registered
    // 检测 schemes（空表 = defaultSchemes()）是否已全部注册。
    static bool isRegistered(const QStringList &schemes = QStringList());

    // 对应Python: core/url_dispatch/url_scheme_register.py::register
    // 注册 schemes 到当前用户；exePath 为空时自动取当前可执行文件路径
    //（对应 _get_exe_path）。全部成功返回 true。
    static bool registerSchemes(const QStringList &schemes = QStringList(),
                                const QString &exePath = QString());

    // 反注册（Python 版无对应，接口按任务约定补齐）：
    // Windows 删协议键；Linux 删 .desktop 文件并刷新数据库；macOS 用
    // PlistBuddy 移除 Info.plist 中对应 scheme 条目并 lsregister -f 刷新。
    static bool unregisterSchemes(const QStringList &schemes = QStringList());

    // 对应Python: core/url_dispatch/url_scheme_register.py::ensure_registered
    // 应用启动时调用：未注册则静默注册，绝不抛错、不影响启动。
    static void ensureRegistered();
};

} // namespace cubeshell
