// main.cpp — 应用入口：完整启动流程。
// 对应Python: cube-shell.py 末尾 `if __name__ == "__main__":` 启动段
//
// 流程：GlobalState/theme.json 加载 → 主题应用 → 语言加载 → 单实例检查
// (QLockFile) → 解析命令行 jms:// URL → 创建主窗口 → 处理 URL 事件 →
// 进入事件循环。macOS 下额外安装 QFileOpenEvent 过滤器处理 URL Scheme。

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

#ifndef Q_OS_WIN
#include <csignal>
#endif

#include "main_window.h"
#include "LanguageManager.h"

#include "config/GlobalState.h"
#include "util/ThemeManager.h"

namespace {

#ifndef CUBESHELL_PLATFORM_OHOS
// macOS URL Scheme 事件（jms:// 由系统经 QFileOpenEvent 投递给运行中的应用）。
// 对应Python: cube-shell.py::UrlSchemeApplication.event(QFileOpenEvent)
// 鸿蒙：无 QFileOpenEvent 机制（系统交互走 Want/Uri），不编译。
class UrlOpenFilter : public QObject {
public:
    explicit UrlOpenFilter(cubeshell::MainWindow *window, QObject *parent = nullptr)
        : QObject(parent)
        , m_window(window)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *openEvent = static_cast<QFileOpenEvent *>(event);
            const QString url = openEvent->url().isValid()
                                    ? openEvent->url().toString()
                                    : openEvent->file();
            if (!url.isEmpty() && m_window) {
                m_window->handleUrl(url);
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    cubeshell::MainWindow *m_window;
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
        if (arg.startsWith(QLatin1String("jms://"))
                || arg.startsWith(QLatin1String("ssh://"))
                || arg.startsWith(QLatin1String("telnet://"))
                || arg.startsWith(QLatin1String("cubeshell://")))
            return arg;
#ifdef CUBESHELL_WITH_RDP
        // rdp:// / rdp+ntlm-password:// → MainWindow::handleUrl 直开 RDP 标签
        if (arg.startsWith(QLatin1String("rdp://"))
                || arg.startsWith(QLatin1String("rdp+")))
            return arg;
#endif
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

    // 5. 命令行 URL 参数（jms:// 直连）。
    const QString startupUrl = urlFromArguments(QCoreApplication::arguments());

    // 6. 创建主窗口。
    cubeshell::MainWindow window;
    window.show();

    // 7. macOS QFileOpenEvent（应用已运行时系统投递的 URL Scheme 事件）。
    // 鸿蒙：系统交互走 Want/Uri，无此事件，过滤器不安装。
#ifndef CUBESHELL_PLATFORM_OHOS
    auto *urlFilter = new UrlOpenFilter(&window, &app);
    app.installEventFilter(urlFilter);
#endif

    // 8. 启动参数里带 URL → 主窗口就绪后处理（BastionClient 解析并自动连接）。
    if (!startupUrl.isEmpty())
        window.handleUrl(startupUrl);

    return QApplication::exec();
}
