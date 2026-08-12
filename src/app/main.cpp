// main.cpp — 应用入口：完整启动流程。
// 对应Python: cube-shell.py 末尾 `if __name__ == "__main__":` 启动段
//
// 流程：安装 URL 事件过滤器 → GlobalState/theme.json 加载 → 主题应用 → 语言
// 加载 → 单实例检查 (QLockFile) → 解析命令行 jms:// URL → 创建主窗口 → 处理
// URL → 进入事件循环。
//
// 过滤器排在最前面不是随手写的：macOS 冷启动时 QFileOpenEvent 到得很早，装晚了
// 就丢，症状是"应用起来了但没连上资源"。详见 UrlOpenFilter 的注释。

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#ifndef CUBESHELL_PLATFORM_OHOS
#include <QFileOpenEvent>
#endif
#include <QIcon>
#ifndef CUBESHELL_PLATFORM_OHOS
#include <QLockFile>
#endif
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

#ifndef Q_OS_WIN
#include <csignal>
#endif

#include "main_window.h"
#include "LanguageManager.h"

#include "config/GlobalState.h"
#include "platform/UrlSchemeRegistrar.h"
#include "url_dispatch/UrlHandler.h"
#include "util/ThemeManager.h"

namespace {

#ifndef CUBESHELL_PLATFORM_OHOS
// macOS URL Scheme 事件（jms:// 由系统经 QFileOpenEvent 投递给运行中的应用）。
// 对应Python: core/url_dispatch/bastion_client.py::UrlEventFilter
// 鸿蒙：无 QFileOpenEvent 机制（系统交互走 Want/Uri），不编译。
//
// 两个要点，缺一都会导致"点了只启动、不连接"：
//  1. 【本次 bug 的真正原因】过滤器必须在 MainWindow 之前安装，且窗口未就绪时
//     要把 URL 存下来。macOS 冷启动（浏览器里点"打开 cube-shell.app"）时
//     QFileOpenEvent 在启动早期就投递过来，而旧代码是建完窗口才 install，
//     事件根本没人接 —— 应用起来了、URL 没了。m_pendingUrl 负责接住这一发。
//  2. 取值必须 file() 优先。jms:// 的 Base64 载荷落在 hostname 位置，而 QUrl 按
//     DNS 规则校验主机名：单个 label 上限 63 字符。JumpServer 的载荷实测 120~680
//     字符（本机联调的一条是 676），必然超限；Base64 里的 '+' '=' 同样非法。
//     结果是 isValid() 为 false 且 toString()/toEncoded() 全返回空串。
//     旧代码写成 url().isValid() ? url().toString() : file() —— 靠 isValid() 为
//     假而恰好落到 file()，能用但纯属巧合；这里改成无条件 file() 优先，把意图
//     写死，免得后人"优化"成 url().toString() 又把 URL 变成空串。
//     回归闸门见 tests/url_dispatch_test.cpp::testJmsUrlSurvivesWhereQUrlFails。
class UrlOpenFilter : public QObject {
public:
    explicit UrlOpenFilter(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    // 主窗口就绪后调用；在此之前到达的 URL 都存在 m_pendingUrl 里。
    void setWindow(cubeshell::MainWindow *window) { m_window = window; }

    // 取走并清空缓存的 URL（没有则返回空串）。
    QString takePendingUrl()
    {
        const QString url = m_pendingUrl;
        m_pendingUrl.clear();
        return url;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *openEvent = static_cast<QFileOpenEvent *>(event);
            // file() 优先——见上面第 1 点。后两者仅作兜底。
            QString url = openEvent->file();
            if (url.isEmpty())
                url = openEvent->url().toString();
            if (url.isEmpty())
                url = QString::fromUtf8(openEvent->url().toEncoded());
            // 只接管自己的 scheme。file() 对"把文件拖到 Dock 图标"返回的是
            // 普通路径，那类事件要放行给下游，不能在这里吞掉。
            if (cubeshell::isSupportedUrlScheme(url)) {
                if (m_window)
                    m_window->handleUrl(url);
                else
                    m_pendingUrl = url;   // 窗口还没好，先存着
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    cubeshell::MainWindow *m_window = nullptr;
    QString m_pendingUrl;
};
#endif // CUBESHELL_PLATFORM_OHOS

// theme.json 查找：用户配置目录优先，其次工程 conf/（与 Python 侧共用一份）。
// 对应Python: function/util.py 里 THEME 的加载路径
QString locateThemeJson()
{
    const QStringList candidates = {
        cubeshell::GlobalState::configFilePath(QStringLiteral("theme.json")),
        QDir::currentPath() + QStringLiteral("/conf/theme.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/conf/theme.json"),
        // cpp/build/bin/xx.app/Contents/MacOS → 工程根/conf
        QCoreApplication::applicationDirPath()
            + QStringLiteral("/../../../../../conf/theme.json"),
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    // 不存在时仍返回配置目录路径（saveTheme 落在这里）。
    return candidates.first();
}

// 命令行里找 jms:// / ssh:// / cubeshell:// URL（含 -url 标志形式）。
// 对应Python: core/url_dispatch/bastion_client.py::scan_argv_for_url
QString urlFromArguments(const QStringList &args)
{
    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("-url") && i + 1 < args.size())
            return args.at(i + 1);
        // scheme 清单与 QFileOpenEvent 过滤器共用（含 telnet:// 与 rdp 变体）。
        if (cubeshell::isSupportedUrlScheme(arg))
            return arg;
    }
    return QString();
}

} // namespace

int main(int argc, char *argv[])
{
#ifndef Q_OS_WIN
    // Ignore SIGPIPE: libssh2 writes to SSH sockets that may already be closed
    // by the remote end (e.g. during channel/session teardown when closing a
    // tab). Without this, the default SIGPIPE handler terminates the process.
    // macOS does not support MSG_NOSIGNAL, so process-level ignore is required.
    std::signal(SIGPIPE, SIG_IGN);
#endif

#ifdef Q_OS_MACOS
    // 强制 Qt Multimedia 用原生 darwin(AVFoundation/CoreAudio) 后端。
    // VoiceInput 的 QAudioSource 麦克风采集走它；FFmpeg 后端在打包时已被剔除
    // （见 build.yml macOS 瘦身段），这里写死可确保 Qt 绝不会去回退加载 FFmpeg。
    // 须在首次触碰 QMediaDevices/QAudioSource 之前设置（后端是懒解析的）。
    qputenv("QT_MEDIA_BACKEND", "darwin");
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("cube-shell"));
    QApplication::setOrganizationName(QStringLiteral("CubeShell"));
    // 版本号来自 CMake 的 PROJECT_VERSION（见根 CMakeLists 的 CUBESHELL_VERSION）。
    // 更新检查/关于对话框都以此为准，不读 theme.json 的 "version"。
    QApplication::setApplicationVersion(QStringLiteral(CUBESHELL_VERSION));
#ifndef Q_OS_MACOS
    // Linux/Windows: 从编译资源设置窗口/任务栏图标。
    // macOS: 由 bundle 的 Info.plist → logo.icns 处理 Dock 图标，不可在此覆盖。
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/logo.png")));
#endif

    // 0. URL 事件过滤器：必须紧跟 QApplication 构造安装，早于一切可能自旋事件
    // 循环的代码（下面单实例检查里的 QMessageBox 就会自旋）。
    // 对应Python: cube-shell.py 里 `app = CubeShellApp(...)` 之后立刻
    //             `url_filter = create_url_event_filter(app)`
    //
    // macOS 冷启动（浏览器里点"打开 cube-shell.app"）时系统投递 QFileOpenEvent
    // 的时机很早，装晚了事件已经被丢掉，表现就是"应用起来了但没连接"。
    // 此刻窗口还不存在，URL 先进过滤器的缓存，等窗口就绪再消费。
    // 鸿蒙：系统交互走 Want/Uri，无此事件，过滤器不安装。
#ifndef CUBESHELL_PLATFORM_OHOS
    auto *urlFilter = new UrlOpenFilter(&app);
    app.installEventFilter(urlFilter);
#endif

    // 0. URL Scheme 注册（jms:// / cubeshell:// / telnet:// → 本程序）。
    // 对应Python: cube-shell.py __main__ 段的 ensure_registered()。
    // 幂等：已注册（注册表命令指向当前 exe）则直接返回；未注册/指向旧程序则
    // 静默改写。Windows 上写 HKCU\Software\Classes\<scheme>，无需管理员权限。
    // 缺失这一步会导致注册表仍指向旧版 Python 程序——JumpServer 页面点击连接
    // 拉起的是老程序。鸿蒙内部短路为 no-op（HAP 由系统声明 scheme）。
    cubeshell::UrlSchemeRegistrar::ensureRegistered();

    // 1. 配置加载（theme.json → GlobalState 单例）。
    // 对应Python: util.THEME 的初始化
    cubeshell::GlobalState &state = cubeshell::GlobalState::instance();
    state.loadTheme(locateThemeJson());

    // 1b. 收敛含凭据配置文件的权限到 0600。
    // 必须在这里做而不是只在写出时做：历史版本按 umask 建出来的是 0644，
    // 用户磁盘上现存的那一份才是正在泄露的那一份。幂等，每次启动跑一遍。
    for (const QString &path : cubeshell::GlobalState::hardenConfigPermissions())
        qWarning("无法收紧配置文件权限（该卷可能不支持 POSIX 权限）: %s", qPrintable(path));

    // 2. 主题应用（QSS + QPalette）。对应Python: applyAppearance
    cubeshell::ThemeManager::applyTheme(&app, state.appearance());

    // 3. 语言加载（复用 Python 侧 i18n/*.qm）。
    // 对应Python: language_manager.initialize(app) + load_from_config
    cubeshell::LanguageManager &lang = cubeshell::LanguageManager::instance();
    lang.initialize(&app);
    lang.loadFromConfig(state.language());

#ifndef CUBESHELL_PLATFORM_OHOS
    // 4. 单实例检查（QLockFile）。
    // 对应Python: cube-shell.py 的单实例保护
    // 鸿蒙：HAP 由系统保证单实例（同包名不可多开），且沙箱下跨应用共享
    // TmpLocation 语义不同，QLockFile 不适用。
    QLockFile lock(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                       .filePath(QStringLiteral("cube-shell.lock")));
    lock.setStaleLockTime(30 * 1000);
    if (!lock.tryLock(100)) {
        QMessageBox::warning(nullptr, QObject::tr("CubeShell"),
                             QObject::tr("CubeShell is already running."));
        return 0;
    }
#endif

    // 5. 命令行 URL 参数（jms:// 直连；cubeshell://open-local 开本机终端）。
    QString startupUrl = urlFromArguments(QCoreApplication::arguments());
    // Windows 右键菜单传裸目录路径，转成 cubeshell://open-local 走同一条分发。
    if (startupUrl.isEmpty())
        startupUrl = cubeshell::directoryArgumentAsUrl(QCoreApplication::arguments());

    // 6. 创建主窗口。
    cubeshell::MainWindow window;
    window.show();

#ifndef CUBESHELL_PLATFORM_OHOS
    urlFilter->setWindow(&window);
    // 窗口就绪前缓存下来的 URL，现在消费掉（命令行参数优先）。
    if (startupUrl.isEmpty())
        startupUrl = urlFilter->takePendingUrl();
#endif

    // 7. 启动参数里带 URL → 主窗口就绪后处理（BastionClient 解析并自动连接）。
    if (!startupUrl.isEmpty())
        window.handleUrl(startupUrl);

#ifndef CUBESHELL_PLATFORM_OHOS
    // 8. 延迟兜底：FileOpen 事件也可能在 exec() 起来之后才到（macOS 冷启动
    // 常见）。此时 setWindow() 已生效，过滤器会直接转发；这里只兜住"事件恰好
    // 落在 setWindow() 之前、又没被上面消费到"的窄缝。
    // 对应Python: bastion_client.py::setup_deferred_url_check 的 500ms 检查。
    if (startupUrl.isEmpty()) {
        QTimer::singleShot(500, &window, [urlFilter, &window]() {
            const QString pending = urlFilter->takePendingUrl();
            if (!pending.isEmpty())
                window.handleUrl(pending);
        });
    }
#endif

    return QApplication::exec();
}
