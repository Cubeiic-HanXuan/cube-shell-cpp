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
    // 1. 编进二进制的 qrc 资源（前缀 "/kb-layouts"，见 qtermwidget_res.qrc）。
    //    鸿蒙 HAP 无可探测的 resources/ 目录，下面的文件系统探测全落空后只剩
    //    编译期兜底翻译表（仅 Tab 一键），方向键/Home/Delete/F 键全部失效；
    //    qrc 是鸿蒙下唯一可靠来源，桌面平台同样用作优先项（与配色方案一致）。
    const QString qrcDir = QStringLiteral(":/kb-layouts");
    if (QDir(qrcDir).exists()
        && !QDir(qrcDir).entryList(QStringList(QStringLiteral("*.keytab"))).isEmpty())
        return qrcDir + QLatin1Char('/');

    // 2. 文件系统候选：macOS bundle / 可执行旁 resources/ / 编译期源码树。
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

    // 1. 编进二进制的 qrc 资源（前缀 "/color-schemes"，见 qtermwidget_res.qrc）。
    //    QDir 可直接枚举 qrc 路径，QFile/QSettings 也能读 ":/..."。这是鸿蒙 HAP
    //    （无可探测 resources/ 目录）下唯一可靠的来源，桌面平台同样可用作兜底。
    //    仅在确实有文件时采用，避免 qrc 未挂到可执行目标时产生一个空目录命中。
    const QString qrcDir = QStringLiteral(":/color-schemes");
    if (QDir(qrcDir).exists()
        && !QDir(qrcDir).entryList(QStringList(QStringLiteral("*.colorscheme"))).isEmpty())
        rval << qrcDir + QLatin1Char('/');

    // 2. 文件系统候选（macOS bundle / 可执行旁 resources/ / 编译期源码树），
    //    取探测顺序里的第一个命中项，保持与既有行为一致。
    const QStringList candidates =
        resourceDirCandidates(QStringLiteral("color-schemes"), COLORSCHEMES_DIR);
    for (const QString &dir : candidates) {
        if (QDir(dir).exists()) {
            rval << dir;
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
