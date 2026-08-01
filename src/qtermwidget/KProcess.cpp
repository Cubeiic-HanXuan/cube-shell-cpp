// KProcess.cpp — C++ port of qtermwidget/kprocess.py

#include "KProcess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace Konsole {

// 对应C++: static const char DUMMYENV[] = "_KPROCESS_DUMMY_";
static const char DUMMYENV[] = "_KPROCESS_DUMMY_";

void KProcess::setEnv(const QString &name, const QString &value, bool overwrite)
{
    QProcessEnvironment env = processEnvironment();
    if (env.isEmpty()) {
        env = QProcessEnvironment::systemEnvironment();
        env.remove(QLatin1String(DUMMYENV));
    }
    if (env.contains(name) && !overwrite)
        return;
    env.insert(name, value);
    setProcessEnvironment(env);
}

void KProcess::unsetEnv(const QString &name)
{
    QProcessEnvironment env = processEnvironment();
    if (env.isEmpty()) {
        env = QProcessEnvironment::systemEnvironment();
        env.remove(QLatin1String(DUMMYENV));
    }
    if (env.contains(name)) {
        env.remove(name);
        if (env.isEmpty())
            env.insert(QLatin1String(DUMMYENV), QString());
        setProcessEnvironment(env);
    }
}

void KProcess::clearEnvironment()
{
    QProcessEnvironment env;
    env.insert(QLatin1String(DUMMYENV), QString());
    setProcessEnvironment(env);
}

void KProcess::setProgram(const QString &exe, const QStringList &args)
{
    m_prog = exe;
    m_args = args;
#ifdef Q_OS_WIN
    setNativeArguments(QString());
#endif
}

void KProcess::setProgram(const QStringList &argv)
{
    Q_ASSERT(!argv.isEmpty());
    QStringList a = argv;
    m_prog = a.takeFirst();
    m_args = a;
#ifdef Q_OS_WIN
    setNativeArguments(QString());
#endif
}

void KProcess::clearProgram()
{
    m_prog.clear();
    m_args.clear();
#ifdef Q_OS_WIN
    setNativeArguments(QString());
#endif
}

void KProcess::setShellCommand(const QString &cmd)
{
    m_args.clear();
#ifdef Q_OS_WIN
    // Resolve cmd.exe from the system directory.
    wchar_t buffer[MAX_PATH + 1];
    UINT len = GetSystemDirectoryW(buffer, MAX_PATH + 1);
    QString sysdir = len ? QString::fromWCharArray(buffer)
                         : qEnvironmentVariable("SYSTEMROOT", QStringLiteral("C:\\Windows"))
                               + QStringLiteral("\\System32");
    m_prog = sysdir + QLatin1String("\\cmd.exe");
    setNativeArguments(QStringLiteral("/V:OFF /S /C \"%1\"").arg(cmd));
#else
    QString sh = QStringLiteral("/bin/sh");
    // Resolve symlink (e.g. /bin/sh -> dash/bash).
    const QString target = QFile::symLinkTarget(sh);
    if (!target.isEmpty())
        sh = QDir(QFileInfo(sh).absolutePath()).absoluteFilePath(target);
    m_prog = sh;
    m_args = { QStringLiteral("-c"), cmd };
#endif
}

int KProcess::execute(int msecs)
{
    start();
    if (!waitForFinished(msecs)) {
        kill();
        waitForFinished(-1);
        return -2;
    }
    return (exitStatus() == QProcess::NormalExit) ? exitCode() : -1;
}

int KProcess::execute(const QString &exe, const QStringList &args, int msecs)
{
    KProcess p;
    p.setProgram(exe, args);
    return p.execute(msecs);
}

int KProcess::execute(const QStringList &argv, int msecs)
{
    KProcess p;
    p.setProgram(argv);
    return p.execute(msecs);
}

int KProcess::startDetached()
{
    qint64 pid = 0;
    bool ok = QProcess::startDetached(m_prog, m_args, workingDirectory(), &pid);
    return ok ? int(pid) : 0;
}

int KProcess::startDetached(const QString &exe, const QStringList &args)
{
    qint64 pid = 0;
    bool ok = QProcess::startDetached(exe, args, QString(), &pid);
    return ok ? int(pid) : 0;
}

int KProcess::startDetached(const QStringList &argv)
{
    if (argv.isEmpty())
        return 0;
    return startDetached(argv.first(), argv.mid(1));
}

} // namespace Konsole
