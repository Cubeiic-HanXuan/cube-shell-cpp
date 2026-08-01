// FrpManager.cpp — FRP 本地管理器。见 FrpManager.h 的设计说明。

#include "FrpManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcessEnvironment>

#include "util/FileUtil.h"

#ifndef Q_OS_WIN
#  include <unistd.h>
#endif

Q_LOGGING_CATEGORY(frpLog, "cubeshell.frp")

namespace cubeshell {

// 对应Python: core/frp_manager.py::FRP_VERSION
const QString FrpManager::kFrpVersion = QStringLiteral("0.61.1");

namespace {
const QString kNameFrpc = QStringLiteral("frpc");
const QString kNameFrps = QStringLiteral("frps");
} // namespace

FrpManager::FrpManager(QObject *parent)
    : QObject(parent)
{
    // 对应Python: FRPManager.__init__ 的 self.frp_dir = get_frp_dir()
    frpDir(); // 目录懒创建
}

FrpManager::~FrpManager()
{
    // 析构时收掉托管的子进程，避免留下孤儿 frpc/frps。
    stopProcess(kNameFrpc, m_frpc, 1000);
    stopProcess(kNameFrps, m_frps, 1000);
}

// ---------------------------------------------------------------------------
// 路径与安装状态
// ---------------------------------------------------------------------------

// 对应Python: core/frp_manager.py::get_frp_dir
QString FrpManager::frpDir()
{
    QString dir;
#ifdef Q_OS_DARWIN
    dir = QDir::home().filePath(
        QStringLiteral("Library/Application Support/cube-shell/frp"));
#elif defined(Q_OS_WIN)
    const QString appData =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    dir = QDir(appData).filePath(QStringLiteral("cube-shell/frp"));
#else
    dir = QDir::home().filePath(QStringLiteral(".cube-shell/frp"));
#endif
    QDir().mkpath(dir); // 对应Python: frp_dir.mkdir(parents=True, exist_ok=True)
    return QDir::cleanPath(dir);
}

// 对应Python: core/frp_manager.py::get_local_frpc_path
QString FrpManager::frpcPath()
{
#ifdef Q_OS_WIN
    return QDir(frpDir()).filePath(QStringLiteral("frpc.exe"));
#else
    return QDir(frpDir()).filePath(kNameFrpc);
#endif
}

// 对应Python: core/frp_manager.py::get_local_frps_path
QString FrpManager::frpsPath()
{
    return QDir(frpDir()).filePath(kNameFrps);
}

// 对应Python: core/frp_manager.py::is_frpc_installed
bool FrpManager::isFrpcInstalled()
{
    const QFileInfo fi(frpcPath());
    return fi.exists() && fi.isExecutable();
}

QString FrpManager::frpcLogPath()
{
    return QDir(frpDir()).filePath(QStringLiteral("frpc.log"));
}

QString FrpManager::frpsLogPath()
{
    return QDir(frpDir()).filePath(QStringLiteral("frps.log"));
}

// 对应Python: core/frp_manager.py::get_platform_key + FRP_DOWNLOADS
QString FrpManager::packageNameForCurrentPlatform()
{
    const QString v = kFrpVersion;
#ifdef Q_OS_DARWIN
#  if defined(Q_PROCESSOR_ARM)
    return QStringLiteral("frp_%1_darwin_arm64.tar.gz").arg(v);
#  else
    return QStringLiteral("frp_%1_darwin_amd64.tar.gz").arg(v);
#  endif
#elif defined(Q_OS_WIN)
#  if defined(Q_PROCESSOR_X86_64) || defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("frp_%1_windows_amd64.zip").arg(v);
#  else
    return QStringLiteral("frp_%1_windows_386.zip").arg(v);
#  endif
#elif defined(Q_OS_LINUX)
#  if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("frp_%1_linux_arm64.tar.gz").arg(v);
#  elif defined(Q_PROCESSOR_ARM)
    return QStringLiteral("frp_%1_linux_arm.tar.gz").arg(v);
#  else
    return QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(v);
#  endif
#else
    return QString(); // 对应Python: get_platform_key 返回 None
#endif
}

// 对应Python: core/frp_manager.py::SERVER_ARCH_MAP
QString FrpManager::serverPackageNameForArch(const QString &arch)
{
    const QString a = arch.trimmed();
    if (a == QStringLiteral("x86_64") || a == QStringLiteral("amd64"))
        return QStringLiteral("frp_%1_linux_amd64.tar.gz").arg(kFrpVersion);
    if (a == QStringLiteral("aarch64") || a == QStringLiteral("arm64"))
        return QStringLiteral("frp_%1_linux_arm64.tar.gz").arg(kFrpVersion);
    if (a == QStringLiteral("armv7l"))
        return QStringLiteral("frp_%1_linux_arm.tar.gz").arg(kFrpVersion);
    return QString();
}

// ---------------------------------------------------------------------------
// 配置生成
// ---------------------------------------------------------------------------

// 对应Python: cube-shell.py::FRPConnectThread._start_services
//             （traversal.frpc + open('frpc.toml','w').write）
bool FrpManager::writeFrpcConfigFile(const QString &configPath,
                                     const QString &serverAddr, const QString &token,
                                     const QString &antType, int localPort,
                                     int remotePort, QString *errorOut)
{
    // 复用 util/FileUtil.h 的模板（对应Python: function/traversal.py::frpc）
    const QString content =
        FileUtil::frpcConfig(serverAddr, token, antType, localPort, remotePort);

    const QFileInfo fi(configPath);
    if (!fi.absoluteDir().exists())
        QDir().mkpath(fi.absolutePath());

    QFile f(configPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入配置文件: %1").arg(configPath);
        return false;
    }
    // 显式 UTF-8：不经 QTextStream 的本地编码，直接写 UTF-8 字节。
    const QByteArray utf8 = content.toUtf8();
    const qint64 n = f.write(utf8);
    f.close();
    if (n != utf8.size()) {
        if (errorOut)
            *errorOut = QStringLiteral("配置文件写入不完整: %1").arg(configPath);
        return false;
    }
    return true;
}

// 对应Python: function/traversal.py::frps（经 FileUtil 模板）
QString FrpManager::buildFrpsConfig(const QString &token, const QString &antType,
                                    int httpPort)
{
    return FileUtil::frpsConfig(token, antType, httpPort);
}

// ---------------------------------------------------------------------------
// 进程启停
// ---------------------------------------------------------------------------

void FrpManager::setStatus(const QString &name, Status status)
{
    Status &slot = (name == kNameFrpc) ? m_frpcStatus : m_frpsStatus;
    if (slot == status)
        return;
    slot = status;
    qCInfo(frpLog) << name << "status ->" << int(status);
    emit statusChanged(name, status);
}

void FrpManager::appendLog(const QString &logPath, const QByteArray &utf8Bytes)
{
    QFile f(logPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(utf8Bytes); // 已是 UTF-8 字节，原样落盘
}

void FrpManager::wireProcess(const QString &name, QProcess *proc, const QString &logPath)
{
    // 合并 stdout/stderr（对应Python: `> frpc.log 2>&1` / `&> frps.log`）
    proc->setProcessChannelMode(QProcess::MergedChannels);

    // 日志采集：QProcess 信号与本对象同线程，无需跨线程投递。
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, name, proc, logPath]() {
        const QByteArray chunk = proc->readAllStandardOutput();
        appendLog(logPath, chunk);
        QByteArray &buf = (name == kNameFrpc) ? m_frpcBuf : m_frpsBuf;
        buf.append(chunk);
        int idx;
        while ((idx = buf.indexOf('\n')) >= 0) {
            const QByteArray lineBytes = buf.left(idx);
            buf.remove(0, idx + 1);
            // 显式 UTF-8 解码（frp 输出为 UTF-8）
            QString line = QString::fromUtf8(lineBytes);
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            emit logOutput(name, line);
        }
    });

    connect(proc, &QProcess::started, this, [this, name, proc]() {
        setStatus(name, Status::Running);
        emit processStarted(name, proc->processId());
    });

    connect(proc, &QProcess::errorOccurred, this, [this, name](QProcess::ProcessError e) {
        // FailedToStart 等价于 Python 侧 "启动失败" 分支。
        if (e == QProcess::FailedToStart) {
            setStatus(name, Status::Failed);
            emit errorOccurred(name, QStringLiteral("%1 启动失败（可执行文件不存在或无权限）")
                                         .arg(name));
        }
    });

    connect(proc, &QProcess::finished, this,
            [this, name](int exitCode, QProcess::ExitStatus exitStatus) {
                // 冲掉行缓冲里没有换行结尾的残余。
                QByteArray &buf = (name == kNameFrpc) ? m_frpcBuf : m_frpsBuf;
                if (!buf.isEmpty()) {
                    emit logOutput(name, QString::fromUtf8(buf));
                    buf.clear();
                }
                const bool crashed = (exitStatus == QProcess::CrashExit);
                // 主动 stopXxx() 触发的 terminate/kill 不算异常退出
                // （对应Python: pkill -9 frpc 后不报错）。
                const bool stopping = (name == kNameFrpc) ? m_frpcStopping : m_frpsStopping;
                const bool abnormal = !stopping && (crashed || exitCode != 0);
                setStatus(name, abnormal ? Status::Failed : Status::Stopped);
                if (abnormal) {
                    emit errorOccurred(name, QStringLiteral("%1 异常退出(code=%2)")
                                                 .arg(name).arg(exitCode));
                }
                emit processStopped(name, exitCode);
            });
}

bool FrpManager::startProcess(const QString &name, QProcess *&slot, const QString &program,
                              const QString &configPath, const QString &logPath,
                              QString *errorOut)
{
    if (slot && slot->state() != QProcess::NotRunning)
        return true; // 已在运行（对应Python: 启动前 pkill 后重启，这里幂等）

    if (!QFileInfo::exists(program)) {
        setStatus(name, Status::Failed);
        const QString msg = QStringLiteral("%1 未安装: %2").arg(name, program);
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(name, msg);
        return false;
    }
    if (!QFileInfo::exists(configPath)) {
        setStatus(name, Status::Failed);
        const QString msg = QStringLiteral("配置文件不存在: %1").arg(configPath);
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(name, msg);
        return false;
    }

    if (slot) {
        slot->deleteLater();
        slot = nullptr;
    }
    slot = new QProcess(this);
    (name == kNameFrpc ? m_frpcStopping : m_frpsStopping) = false;
    // 工作目录为 frp 目录（对应Python: cd "{frp_log_dir}" && nohup ...）
    slot->setWorkingDirectory(frpDir());
    wireProcess(name, slot, logPath);

    setStatus(name, Status::Starting);
    slot->start(program, { QStringLiteral("-c"), configPath });
    if (!slot->waitForStarted(5000)) {
        setStatus(name, Status::Failed);
        const QString msg = QStringLiteral("%1 启动超时").arg(name);
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(name, msg);
        return false;
    }
    return true;
}

void FrpManager::stopProcess(const QString &name, QProcess *&slot, int msecs)
{
    if (!slot)
        return;
    (name == kNameFrpc ? m_frpcStopping : m_frpsStopping) = true;
    if (slot->state() != QProcess::NotRunning) {
        slot->terminate(); // 先 SIGTERM，让 frp 正常收尾
        if (!slot->waitForFinished(msecs))
            slot->kill(); // 对应Python: pkill -9
        slot->waitForFinished(1000);
    }
    slot->deleteLater();
    slot = nullptr;
    setStatus(name, Status::Stopped);
}

bool FrpManager::startFrpc(const QString &configPath, QString *errorOut)
{
    return startProcess(kNameFrpc, m_frpc, frpcPath(), configPath,
                        frpcLogPath(), errorOut);
}

void FrpManager::stopFrpc(int msecs)
{
    stopProcess(kNameFrpc, m_frpc, msecs);
}

bool FrpManager::isFrpcRunning() const
{
    return m_frpc && m_frpc->state() == QProcess::Running;
}

bool FrpManager::startFrps(const QString &configPath, QString *errorOut)
{
    return startProcess(kNameFrps, m_frps, frpsPath(), configPath,
                        frpsLogPath(), errorOut);
}

void FrpManager::stopFrps(int msecs)
{
    stopProcess(kNameFrps, m_frps, msecs);
}

bool FrpManager::isFrpsRunning() const
{
    return m_frps && m_frps->state() == QProcess::Running;
}

} // namespace cubeshell
