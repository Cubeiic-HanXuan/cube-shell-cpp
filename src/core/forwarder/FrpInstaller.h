#pragma once

// FrpInstaller.h — frp 二进制的按需下载与部署。
//
// 对应Python: core/frp_manager.py 的下载/解压/远端部署部分：
//   - download_with_progress / extract_archive / download_frpc
//     → ensureFrpc()
//   - get_remote_home_dir / get_server_arch / download_frps_for_server
//     + FRPManager.ensure_frps_on_server → ensureFrpsOnServer()
//
// 与 FrpManager 的分工：FrpManager 负责路径、平台包名映射、配置生成与本地
// 进程托管（不碰网络）；本类只做「下载 + 落盘 + 远端部署」，两者组合即
// Python 侧 FRPManager 的全部职责。
//
// 实现差异（PORTING_CONTRACT 认可）：
//  - 下载：Python 用 urllib.urlretrieve（阻塞）；C++ 用 QNetworkAccessManager
//    + 局部 QEventLoop 做同步等待，可在 worker 线程调用（QNAM 与事件循环都
//    创建在调用线程内），进度语义与 reporthook 一致。
//  - 解压：Python 用 tarfile/zipfile；C++ 用 QProcess 调系统 tar
//    （.tar.gz → `tar -xzf`，Windows 的 .zip → 内置 bsdtar `tar -xf`），
//    Windows 下带 CREATE_NO_WINDOW 规避控制台黑窗。
//  - 上传：Python 用 paramiko sftp.put；C++ 用 SftpClient::writeFile
//    （同步，可在 worker 线程调用）。
//
// 线程模型：ensureFrpc/ensureFrpsOnServer 均为阻塞调用，设计在 worker 线程
// （FrpConnectWorker）中使用；两个信号从调用线程发出，跨线程消费方按 Qt
// 默认 AutoConnection 即得到 Queued 投递。

#include <QObject>
#include <QString>

namespace cubeshell {

class CommandExecutor;
class SftpClient;

class FrpInstaller : public QObject {
    Q_OBJECT
public:
    explicit FrpInstaller(QObject *parent = nullptr);

    // --- 纯函数（无网络，可单测） ---

    // GitHub release 下载前缀。
    // 对应Python: core/frp_manager.py::FRP_GITHUB_BASE
    static QString githubBase();
    // 完整下载地址。对应Python: f"{FRP_GITHUB_BASE}/{package_name}"
    static QString downloadUrl(const QString &packageName);
    // 归档包名去掉 .tar.gz / .zip 后缀后的解压目录名。
    // 对应Python: package_name.replace(".tar.gz", "").replace(".zip", "")
    static QString archiveBaseName(const QString &packageName);
    // 远端解压命令（与 Python 逐字一致，两版行为可互换）。
    // 对应Python: core/frp_manager.py::download_frps_for_server 的 cmd
    static QString remoteExtractCommand(const QString &homeDir,
                                        const QString &packageName);

    // --- 阻塞式安装/部署 ---

    // 确保本地 frpc 就绪：已安装直接返回 true，否则下载并安装。
    // 对应Python: FRPManager.ensure_frpc + download_frpc
    bool ensureFrpc();

    // 确保远端 $HOME/frp/frps 就绪。exec/sftp 必须已连上同一台服务器。
    // 对应Python: FRPManager.ensure_frps_on_server + download_frps_for_server
    bool ensureFrpsOnServer(CommandExecutor *exec, SftpClient *sftp);

signals:
    // 下载进度（字节）。对应Python: progress_callback(downloaded, total)
    void progress(qint64 downloaded, qint64 total);
    // 状态文案。对应Python: status_callback(msg)
    void status(const QString &message);

private:
    // 同步下载到 destPath（内部起局部 QEventLoop）。
    // 对应Python: core/frp_manager.py::download_with_progress
    bool downloadFile(const QString &url, const QString &destPath);
    // 用系统 tar 解压到 extractDir。
    // 对应Python: core/frp_manager.py::extract_archive
    static bool extractArchive(const QString &archivePath, const QString &extractDir);
    // 复制并补上可执行位（Python: shutil.copy2 + os.chmod S_IX*）。
    static bool installBinary(const QString &srcPath, const QString &destPath);

    // 对应Python: FRPManager._download_in_progress
    bool m_downloadInProgress = false;
};

} // namespace cubeshell
