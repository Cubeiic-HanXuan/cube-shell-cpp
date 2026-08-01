// tools.cpp — C++ port of qtermwidget/tools.py

#include "tools.h"

#include <QCoreApplication>
#include <QDir>

namespace Konsole {

Q_LOGGING_CATEGORY(qtermwidgetLogger, "qtermwidget", QtWarningMsg)

// Compile-time defaults (injected via -D by the build system; they point at
// the source tree cpp/src/qtermwidget/ resource dirs for development runs,
// mirroring the Python qtermwidget/ package layout).
#ifndef KB_LAYOUT_DIR
#define KB_LAYOUT_DIR "kb-layouts"
#endif
#ifndef COLORSCHEMES_DIR
#define COLORSCHEMES_DIR "color-schemes"
#endif

namespace {
QStringList custom_color_schemes_dirs;

// 资源目录探测顺序：
//   1. .app bundle 的 Contents/Resources/<sub>（macOS）
//   2. 可执行文件旁的 resources/<sub>（Linux / Windows 部署布局）
//   3. 编译期宏指向的源码树 cpp/src/qtermwidget/<sub>（开发期兜底，
//      与 Python 版 qtermwidget/<sub> 包内布局一致）
QStringList resourceDirCandidates(const QString &subdir, const char *compileTimeDir)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#ifdef Q_OS_MAC
    candidates << appDir + QLatin1String("/../Resources/") + subdir + QLatin1Char('/');
#endif
    candidates << appDir + QLatin1String("/resources/") + subdir + QLatin1Char('/');
    QString fixed = QLatin1String(compileTimeDir);
    if (!fixed.isEmpty()) {
        if (!fixed.endsWith(QLatin1Char('/')))
            fixed += QLatin1Char('/');
        candidates << fixed;
    }
    return candidates;
}
} // namespace

QString getKbLayoutDir()
{
    const QStringList candidates =
        resourceDirCandidates(QStringLiteral("kb-layouts"), KB_LAYOUT_DIR);
    for (const QString &dir : candidates) {
        if (QDir(dir).exists())
            return dir;
    }

#ifdef QT_DEBUG
    qCDebug(qtermwidgetLogger) << "Cannot find kb-layouts in any location!";
#endif
    return QString();
}

void addCustomColorSchemeDir(const QString &customDir)
{
    if (!custom_color_schemes_dirs.contains(customDir))
        custom_color_schemes_dirs << customDir;
}

QStringList getColorSchemesDirs()
{
    QStringList rval;
    const QStringList candidates =
        resourceDirCandidates(QStringLiteral("color-schemes"), COLORSCHEMES_DIR);
    for (const QString &dir : candidates) {
        if (QDir(dir).exists()) {
            rval << dir;    // 取探测顺序里的第一个命中项
            break;
        }
    }

    for (const QString &customDir : std::as_const(custom_color_schemes_dirs)) {
        if (QDir(customDir).exists())
            rval << customDir;
    }

#ifdef QT_DEBUG
    if (!rval.isEmpty())
        qCDebug(qtermwidgetLogger) << "Using color-schemes: " << rval;
    else
        qCDebug(qtermwidgetLogger) << "Cannot find color-schemes in any location!";
#endif

    return rval;
}

void clearCustomColorSchemeDirs()
{
    custom_color_schemes_dirs.clear();
}

QStringList getCustomColorSchemeDirs()
{
    return custom_color_schemes_dirs;
}

} // namespace Konsole
