// FrpConnectWorker.cpp — 内网穿透连接/停止流程线程。见 FrpConnectWorker.h。
//
// 对应Python: cube-shell.py::FRPConnectThread

#include "FrpConnectWorker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QTcpSocket>

#include "FrpInstaller.h"
#include "FrpManager.h"
#include "config/DeviceConfigStore.h"
#include "config/GlobalState.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SftpClient.h"
#include "ssh/SshClient.h"

Q_LOGGING_CATEGORY(frpConnLog, "cubeshell.frp.connect")

namespace cubeshell {

namespace {
const QString kFrpcConfigName = QStringLiteral("frpc.toml");
} // namespace

FrpConnectWorker::FrpConnectWorker(const FrpConnectParams &params, bool isStop,
                                   QObject *parent)
    : QThread(parent)
    , m_params(params)
    , m_isStop(isStop)
{
}

FrpConnectWorker::~FrpConnectWorker() = default;

// ---------------------------------------------------------------------------
// 纯工具
// ---------------------------------------------------------------------------

// 对应Python: function/util.py::check_server_accessibility
//   socket.create_connection((hostname, port), timeout=1)
bool FrpConnectWorker::checkServerAccessibility(const QString &host, quint16 port,
                                                int timeoutMs)
{
    if (host.isEmpty())
        return false;
    QTcpSocket sock;
    sock.connectToHost(host, port);
    // waitForConnected 不需要事件循环，可直接在 worker 线程用。
    return sock.waitForConnected(timeoutMs);
}

// 对应Python: cube-shell.py::abspath('frpc.toml') —— 工程根 conf/frpc.toml。
// 探测顺序与 LinuxCommandsDialog::resolveCommandsFile 保持一致：
//   可执行文件旁 conf/ → macOS bundle Resources/conf/ → 当前工作目录 conf/ →
//   开发期源码树 conf/。都不存在时回退到用户配置目录（保证可写）。
QString FrpConnectWorker::frpcConfigPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/conf/") + kFrpcConfigName
               << appDir + QStringLiteral("/../Resources/conf/") + kFrpcConfigName
               << QDir::currentPath() + QStringLiteral("/conf/") + kFrpcConfigName;
#ifdef CUBESHELL_SOURCE_CONF_DIR
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/") + kFrpcConfigName;
#endif

    // 已存在的文件优先（与 Python 版读写同一个文件）。
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QDir::cleanPath(path);
    }
    // 其次是 conf 目录已存在、只缺文件的候选。
    for (const QString &path : candidates) {
        if (QFileInfo(path).absoluteDir().exists())
            return QDir::cleanPath(path);
    }
    // 最后回退到用户配置目录（打包运行且 conf/ 不可写时）。
    return GlobalState::configFilePath(kFrpcConfigName);
}

// 对应Python: os.system("pkill -9 frpc 2>/dev/null") /
//             subprocess.run(['taskkill', '/f', '/im', 'frpc.exe'])
void FrpConnectWorker::killLocalFrpc()
{
#ifdef Q_OS_WIN
    QProcess proc;
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
    proc.start(QStringLiteral("taskkill"),
               {QStringLiteral("/f"), QStringLiteral("/im"), QStringLiteral("frpc.exe")});
    proc.waitForFinished(3000);
#else
    QProcess::execute(QStringLiteral("pkill"), {QStringLiteral("-9"), QStringLiteral("frpc")});
#endif
}

// C++ 特有安全修复：替代 Python 的 int() + except 兜底，见头文件说明。
bool FrpConnectWorker::isValidPort(const QString &text, int *portOut)
{
    bool ok = false;
    const int port = text.trimmed().toInt(&ok);
    if (!ok || port < 1 || port > 65535)
        return false;
    if (portOut)
        *portOut = port;
    return true;
}

// C++ 特有：把 time.sleep 拆成 100ms 片，便于响应 requestInterruption()。
bool FrpConnectWorker::interruptibleSleep(int ms)
{
    const int kSliceMs = 100;
    int remaining = ms;
    while (remaining > 0) {
        if (isInterruptionRequested())
            return false;
        const int slice = qMin(kSliceMs, remaining);
        QThread::msleep(static_cast<unsigned long>(slice));
        remaining -= slice;
    }
    return !isInterruptionRequested();
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

// 对应Python: FRPConnectThread.run
void FrpConnectWorker::run()
{
    // 对应Python: host_addr, host_port = util.parse_host_port(host)
    const HostPort hp = parseHostPort(m_params.host);

    // 对应Python: self.status_updated.emit("正在检查服务器连接...")
    emit statusUpdated(QStringLiteral("正在检查服务器连接..."));
    if (!checkServerAccessibility(hp.host, hp.port)) {
        emit finishedSignal(false, QStringLiteral("服务器无法连接，请检查网络或服务器状态。"),
                            !m_isStop);
        return;
    }

    // C++ 特有：调用方已请求中断（对话框正在关闭/析构）时静默收尾。
    if (isInterruptionRequested())
        return;

    // 对应Python: self.status_updated.emit("正在建立 SSH 连接...")
    emit statusUpdated(QStringLiteral("正在建立 SSH 连接..."));

    SshClient ssh;
    ssh.setHost(hp.host, hp.port);
    ssh.setUsername(m_params.username);
    ssh.setPassword(m_params.password);
    if (!m_params.keyType.isEmpty() && !m_params.keyFile.isEmpty())
        ssh.setPrivateKey(m_params.keyType, m_params.keyFile);

    SshError error;
    // 对应Python: ssh_conn.connect()（失败抛异常 → finished_signal(False, str(e), ...)）
    if (!ssh.connectToHost(nullptr, error)) {
        qCWarning(frpConnLog) << "ssh connect failed:" << error.message;
        emit finishedSignal(false, error.message, !m_isStop);
        return;
    }

    CommandExecutor exec(&ssh);

    if (isInterruptionRequested()) {
        ssh.disconnectFromHost();
        return;
    }

    if (m_isStop) {
        // 对应Python: self._stop_services(ssh_conn)
        stopServices(&exec);
    } else {
        // 对应Python: sftp = ssh_conn.open_sftp(); self._start_services(ssh_conn, sftp)
        SftpClient sftp(&ssh);
        if (!sftp.open(error)) {
            qCWarning(frpConnLog) << "open_sftp failed:" << error.message;
            emit finishedSignal(false, error.message, true);
            ssh.disconnectFromHost();
            return;
        }
        startServices(&exec, &sftp);
    }

    // 对应Python: ssh_conn.close()（成功/失败分支都会走到）
    ssh.disconnectFromHost();
}

// 对应Python: FRPConnectThread._start_services
void FrpConnectWorker::startServices(CommandExecutor *exec, SftpClient *sftp)
{
    // --- 端口校验 ---
    // 对应Python: server_port = int(self.params['server_prot'])
    // C++ 特有安全修复：toInt() 失败会静默给 0，必须先校验再进入后续流程，
    // 否则会误判成「低端口需要 root」并写出 localPort=0 的非法 frpc.toml。
    int serverPort = 0;
    int localPort = 0;
    if (!isValidPort(m_params.serverPort, &serverPort)
        || !isValidPort(m_params.localPort, &localPort)) {
        emit finishedSignal(false, QStringLiteral("端口号无效，请输入 1-65535 之间的数字。"),
                            true);
        return;
    }

    // --- 检查服务端代理端口权限 ---
    if (serverPort <= 1024) {
        // 对应Python: try: whoami ... except: pass
        const ExecResult whoami = exec->exec(QStringLiteral("whoami"), false);
        if (whoami.ok()) {
            const QString remoteUser = whoami.stdoutText.trimmed();
            if (remoteUser != QStringLiteral("root")) {
                emit finishedSignal(false,
                                    QStringLiteral("服务端代理端口 %1 需要 root 权限。\n"
                                                   "当前用户为: %2\n"
                                                   "请使用大于 1024 的端口（如 8088、8888 等）")
                                        .arg(serverPort)
                                        .arg(remoteUser),
                                    true);
                return;
            }
        }
    }

    // --- 检查是否需要安装 ---
    if (isInterruptionRequested())
        return;
    // 对应Python: need_client = not self.frp_manager.is_frpc_ready()
    const bool needClient = !FrpManager::isFrpcInstalled();
    // 对应Python: need_server = not util.check_remote_frp_exists(ssh_conn)
    bool needServer = true;
    {
        const ExecResult r =
            exec->exec(QStringLiteral("test -f $HOME/frp/frps && echo 'exists'"), false);
        needServer = !r.stdoutText.contains(QStringLiteral("exists"));
    }

    FrpInstaller installer;
    // 对应Python: status_callback=lambda msg: self.status_updated.emit(msg)
    connect(&installer, &FrpInstaller::status, this, &FrpConnectWorker::statusUpdated,
            Qt::DirectConnection);
    // 对应Python: update_progress(downloaded, total) → int(downloaded * 100 / total)
    connect(&installer, &FrpInstaller::progress, this,
            [this](qint64 downloaded, qint64 total) {
                if (total > 0)
                    emit progressUpdated(static_cast<int>(downloaded * 100 / total));
            },
            Qt::DirectConnection);

    // --- 安装客户端 ---
    if (needClient) {
        // 对应Python: self.status_updated.emit("正在下载 FRP 客户端...")
        emit statusUpdated(QStringLiteral("正在下载 FRP 客户端..."));
        if (!installer.ensureFrpc()) {
            emit finishedSignal(false, QStringLiteral("FRP 客户端下载失败"), true);
            return;
        }
    }

    // --- 安装服务端 ---
    if (needServer) {
        // 对应Python: self.status_updated.emit("正在部署 FRP 服务端...") + progress 0
        emit statusUpdated(QStringLiteral("正在部署 FRP 服务端..."));
        emit progressUpdated(0);
        if (!installer.ensureFrpsOnServer(exec, sftp)) {
            emit finishedSignal(false, QStringLiteral("FRP 服务端部署失败"), true);
            return;
        }
    }

    // --- 启动服务端 ---
    if (isInterruptionRequested())
        return;
    // 对应Python: self.status_updated.emit("正在启动服务端...")
    emit statusUpdated(QStringLiteral("正在启动服务端..."));
    // 先彻底杀死所有 frps 进程（包括可能在 /opt/frp 下的旧进程）
    // 对应Python: exec_command(timeout=2, "killall -9 frps ...; pkill -9 frps ...")
    exec->exec(QStringLiteral("killall -9 frps 2>/dev/null; pkill -9 frps 2>/dev/null"),
               false, 2000);
    // 对应Python: time.sleep(2) 等待端口释放
    if (!interruptibleSleep(2000))
        return;

    // 对应Python: traversal.frps(token, ant_type, server_prot)
    const QString frpsConfig =
        FrpManager::buildFrpsConfig(m_params.token, m_params.antType, serverPort);
    // 对应Python: ssh_conn.exec(f"cat > $HOME/frp/frps.toml << 'EOF'\n{frps_config}\nEOF")
    exec->exec(QStringLiteral("cat > $HOME/frp/frps.toml << 'EOF'\n%1\nEOF").arg(frpsConfig),
               false);

    // 对应Python: cmd1 = "cd $HOME/frp && nohup ./frps -c frps.toml &> frps.log &"
    exec->exec(QStringLiteral("cd $HOME/frp && nohup ./frps -c frps.toml &> frps.log &"),
               false, 2000);
    if (!interruptibleSleep(2000)) // 对应Python: time.sleep(2)
        return;

    // 对应Python: check_result = ssh_conn.exec("pgrep -x frps")
    const ExecResult check = exec->exec(QStringLiteral("pgrep -x frps"), false);
    if (check.stdoutText.trimmed().isEmpty()) {
        emit finishedSignal(false, QStringLiteral("服务端 frps 启动失败，请检查服务器日志"), true);
        return;
    }

    // --- 启动客户端 ---
    // 对应Python: self.status_updated.emit("正在启动客户端...")
    emit statusUpdated(QStringLiteral("正在启动客户端..."));

    // 对应Python: pkill -9 frpc / taskkill（清掉游离进程）
    killLocalFrpc();
    if (!interruptibleSleep(500)) // 对应Python: time.sleep(0.5)
        return;

    // 对应Python: frpc_host, _ = util.parse_host_port(self.params['host'])
    const QString frpcHost = parseHostPort(m_params.host).host;
    // 对应Python: traversal.frpc(...) + open(abspath('frpc.toml'), 'w').write(frpc)
    const QString configPath = frpcConfigPath();
    QString writeError;
    if (!FrpManager::writeFrpcConfigFile(configPath, frpcHost, m_params.token,
                                         m_params.antType, localPort,
                                         serverPort, &writeError)) {
        // 对应Python: open(...) 抛异常 → run() 的 except 分支
        emit finishedSignal(false, writeError, true);
        return;
    }

    qCInfo(frpConnLog).noquote()
        << QStringLiteral("FRP 配置: 服务器=%1, 服务端端口=%2, 本地端口=%3")
               .arg(frpcHost, m_params.serverPort, m_params.localPort);

    // 本地 frpc 的启动交给主线程（QProcess 与 FrpManager 同线程约束），
    // 见 FrpConnectWorker.h 的说明与 NatDialog::onConnectFinished。
    // 对应Python: os.system('cd ... && nohup frpc -c ... > frpc.log 2>&1 &')
    emit finishedSignal(true, QString(), true);
}

// 对应Python: FRPConnectThread._stop_services
void FrpConnectWorker::stopServices(CommandExecutor *exec)
{
    // 对应Python: self.status_updated.emit("正在停止服务...")
    emit statusUpdated(QStringLiteral("正在停止服务..."));

    // 对应Python: exec_command(timeout=1, command="pkill -9 frps")
    exec->exec(QStringLiteral("pkill -9 frps"), false, 1000);

    // 本地 frpc 的停止交给主线程（FrpManager::stopFrpc + killLocalFrpc）。
    // 对应Python: os.system("pkill -9 frpc") / taskkill
    emit finishedSignal(true, QString(), false);
}

} // namespace cubeshell
