#pragma once

// FrpManager.h — FRP（frpc/frps）本地管理器。
//
// 对应Python: core/frp_manager.py（路径/安装状态/版本与包名映射）
//           + cube-shell.py::FRPConnectThread（frpc.toml 生成与进程启停）
//
// 与 Python 版的对应关系：
//  - frp 目录/frpc/frps 路径、安装检测 —— 镜像 get_frp_dir /
//    get_local_frpc_path / get_local_frps_path / is_frpc_installed。
//  - frpc.toml 生成复用 util/FileUtil.h 的 frpcConfig / frpsConfig 模板
//    （对应 function/traversal.py），写文件显式 UTF-8。
//  - 进程启停：Python 用 os.system("nohup frpc ... > frpc.log 2>&1 &") /
//    pkill；C++ 用 QProcess 托管子进程 —— 生命周期可控、可采集日志、
//    可感知退出码，stopXxx() 走 terminate -> kill 两段式。
//  - 二进制的按需下载（download_frpc / download_frps_for_server）依赖
//    网络与远端 SSH 部署，不属于本层（Phase 2b 只要求配置生成 + 启停 +
//    状态监控 + 日志采集）；本类提供版本号与平台包名映射供上层下载器复用。
//
// 日志采集：QProcess::MergedChannels 合并 stdout/stderr，readyRead 时按
// UTF-8 解码逐行发 logOutput(name, line)，同时追加写入 frp 目录下的
// frpc.log / frps.log（与 Python 的 shell 重定向落盘行为一致）。
//
// 线程模型：本类与其 QProcess 同线程（QProcess 信号本就同线程投递），
// 上层跨线程使用时按工程约定自行以 Qt::QueuedConnection 连接。

#include <QObject>
#include <QProcess>
#include <QString>

namespace cubeshell {

class FrpManager : public QObject {
    Q_OBJECT
public:
    // 对应Python: core/frp_manager.py::FRP_VERSION
    static const QString kFrpVersion;

    // 进程运行状态（frpc/frps 各自独立一份）。
    enum class Status {
        Stopped,    // 未运行
        Starting,   // 已发起启动，等待进程起来
        Running,    // 进程运行中
        Failed,     // 启动失败或异常退出
    };
    Q_ENUM(Status)

    explicit FrpManager(QObject *parent = nullptr);
    ~FrpManager() override;

    // --- 路径与安装状态（静态，可单测） ---
    // frp 存储目录（自动创建）。对应Python: get_frp_dir
    static QString frpDir();
    // 本地 frpc 可执行文件路径。对应Python: get_local_frpc_path
    static QString frpcPath();
    // 本地 frps 路径（用于上传到服务器）。对应Python: get_local_frps_path
    static QString frpsPath();
    // frpc 是否已安装且可执行。对应Python: is_frpc_installed
    static bool isFrpcInstalled();

    // 当前平台的下载包名；不支持的平台返回空串。
    // 对应Python: get_platform_key + FRP_DOWNLOADS
    static QString packageNameForCurrentPlatform();
    // 服务器架构（`arch` 命令输出）对应的 linux 包名；未知架构返回空串。
    // 对应Python: SERVER_ARCH_MAP
    static QString serverPackageNameForArch(const QString &arch);

    // --- 配置生成（复用 FileUtil 模板，显式 UTF-8 落盘） ---
    // 生成 frpc.toml 内容并写入 configPath；errorOut 带回失败原因。
    // 对应Python: cube-shell.py::FRPConnectThread._start_services 中
    //             traversal.frpc(...) + open('frpc.toml','w').write(frpc)
    static bool writeFrpcConfigFile(const QString &configPath,
                                    const QString &serverAddr, const QString &token,
                                    const QString &antType, int localPort,
                                    int remotePort, QString *errorOut = nullptr);
    // 生成 frps.toml 内容（远端部署由上层经 SSH 写入 $HOME/frp/frps.toml）。
    // 对应Python: traversal.frps(token, ant_type, server_prot)
    static QString buildFrpsConfig(const QString &token, const QString &antType,
                                   int httpPort);

    // --- 进程启停（QProcess 托管） ---
    // 启动 frpc -c <configPath>；工作目录为 frp 目录。已在运行则直接返回 true。
    // 对应Python: cube-shell.py::FRPConnectThread._start_services 的
    //             nohup frpc -c frpc.toml > frpc.log 2>&1 &
    bool startFrpc(const QString &configPath, QString *errorOut = nullptr);
    // 停止 frpc（terminate -> kill 两段式，等待 msecs）。
    // 对应Python: cube-shell.py::FRPConnectThread._stop_services 的 pkill -9 frpc
    void stopFrpc(int msecs = 3000);
    bool isFrpcRunning() const;
    Status frpcStatus() const { return m_frpcStatus; }

    // 启动本地 frps（本地自建服务端场景；远端 frps 由上层经 SSH 启动）。
    // 对应Python: cube-shell.py::FRPConnectThread 中远端
    //             `nohup ./frps -c frps.toml &> frps.log &` 的本地等价物
    bool startFrps(const QString &configPath, QString *errorOut = nullptr);
    void stopFrps(int msecs = 3000);
    bool isFrpsRunning() const;
    Status frpsStatus() const { return m_frpsStatus; }

    // 日志文件路径（frp 目录下 frpc.log / frps.log）。
    static QString frpcLogPath();
    static QString frpsLogPath();

signals:
    // 运行状态变化。name 为 "frpc" / "frps"。
    void statusChanged(const QString &name, cubeshell::FrpManager::Status status);
    // 进程已启动（pid 可用）。
    void processStarted(const QString &name, qint64 pid);
    // 进程已退出。对应Python: 无（Python 用 pgrep 轮询判断）
    void processStopped(const QString &name, int exitCode);
    // 进程输出的一行日志（UTF-8 解码，已去掉行尾换行）。
    void logOutput(const QString &name, const QString &line);
    // 启动/运行错误（对应 Python 的 "服务端 frps 启动失败" 一类提示）。
    void errorOccurred(const QString &name, const QString &message);

private:
    // 启停的公共实现（frpc/frps 只差可执行文件与名字）。
    bool startProcess(const QString &name, QProcess *&slot, const QString &program,
                      const QString &configPath, const QString &logPath,
                      QString *errorOut);
    void stopProcess(const QString &name, QProcess *&slot, int msecs);
    void wireProcess(const QString &name, QProcess *proc, const QString &logPath);
    void setStatus(const QString &name, Status status);
    // 追加写日志文件（显式 UTF-8 字节，不做本地编码转换）。
    static void appendLog(const QString &logPath, const QByteArray &utf8Bytes);

    QProcess *m_frpc = nullptr;
    QProcess *m_frps = nullptr;
    Status m_frpcStatus = Status::Stopped;
    Status m_frpsStatus = Status::Stopped;
    // 主动停止标记：terminate/kill 导致的退出不算异常退出（不报 Failed）。
    bool m_frpcStopping = false;
    bool m_frpsStopping = false;
    // 行缓冲（进程输出可能在任意字节处切断）。
    QByteArray m_frpcBuf;
    QByteArray m_frpsBuf;
};

} // namespace cubeshell
