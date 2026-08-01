// FrpInstaller.cpp — frp 二进制的按需下载与部署。见 FrpInstaller.h。
//
// 对应Python: core/frp_manager.py

#include "FrpInstaller.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>

#include "FrpManager.h"
#include "ssh/CommandExecutor.h"
#include "ssh/SftpClient.h"
#include "ssh/SshClient.h"

Q_LOGGING_CATEGORY(frpInstallLog, "cubeshell.frp.install")

namespace cubeshell {

FrpInstaller::FrpInstaller(QObject *parent)
    : QObject(parent)
{
}

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

// 对应Python: core/frp_manager.py::FRP_GITHUB_BASE
QString FrpInstaller::githubBase()
{
    return QStringLiteral("https://github.com/fatedier/frp/releases/download/v%1")
        .arg(FrpManager::kFrpVersion);
}

// 对应Python: f"{FRP_GITHUB_BASE}/{package_name}"
QString FrpInstaller::downloadUrl(const QString &packageName)
{
    return githubBase() + QLatin1Char('/') + packageName;
}

// 对应Python: package_name.replace(".tar.gz", "").replace(".zip", "")
QString FrpInstaller::archiveBaseName(const QString &packageName)
{
    QString name = packageName;
    name.replace(QStringLiteral(".tar.gz"), QString());
    name.replace(QStringLiteral(".zip"), QString());
    return name;
}

// 对应Python: core/frp_manager.py::download_frps_for_server 的 cmd（逐字一致）
QString FrpInstaller::remoteExtractCommand(const QString &homeDir,
                                           const QString &packageName)
{
    return QStringLiteral("cd %1 && tar -xzf %2 && rm -rf frp && mv %3 frp && rm -f %2")
        .arg(homeDir, packageName, archiveBaseName(packageName));
}

// ---------------------------------------------------------------------------
// 本地 frpc 安装
// ---------------------------------------------------------------------------

// 对应Python: FRPManager.ensure_frpc + download_frpc
bool FrpInstaller::ensureFrpc()
{
    // 对应Python: if self.is_frpc_ready(): return True
    if (FrpManager::isFrpcInstalled())
        return true;

    // 对应Python: if self._download_in_progress: return False
    if (m_downloadInProgress)
        return false;
    m_downloadInProgress = true;
    struct Guard {
        bool *flag;
        ~Guard() { *flag = false; }
    } guard{&m_downloadInProgress}; // 对应Python: finally: _download_in_progress = False

    // 对应Python: get_platform_key() is None -> status_callback("不支持的平台")
    const QString packageName = FrpManager::packageNameForCurrentPlatform();
    if (packageName.isEmpty()) {
        qCWarning(frpInstallLog) << "unsupported platform";
        emit status(QStringLiteral("不支持的平台"));
        return false;
    }

    // 对应Python: status_callback(f"正在下载 frpc (v{FRP_VERSION})...")
    emit status(QStringLiteral("正在下载 frpc (v%1)...").arg(FrpManager::kFrpVersion));

    // 对应Python: with tempfile.TemporaryDirectory() as tmp_dir
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qCWarning(frpInstallLog) << "temp dir failed:" << tmp.errorString();
        return false;
    }
    const QString archivePath = QDir(tmp.path()).filePath(packageName);

    if (!downloadFile(downloadUrl(packageName), archivePath))
        return false;

    emit status(QStringLiteral("正在解压...")); // 对应Python: status_callback("正在解压...")

    if (!extractArchive(archivePath, tmp.path()))
        return false;

    // 对应Python: extract_folder = os.path.join(tmp_dir, package_name.replace(...))
    const QDir extractFolder(QDir(tmp.path()).filePath(archiveBaseName(packageName)));
#ifdef Q_OS_WIN
    const QString srcFrpc = extractFolder.filePath(QStringLiteral("frpc.exe"));
    const QString srcFrps = extractFolder.filePath(QStringLiteral("frps.exe"));
#else
    const QString srcFrpc = extractFolder.filePath(QStringLiteral("frpc"));
    const QString srcFrps = extractFolder.filePath(QStringLiteral("frps"));
#endif

    // 对应Python: 复制 frpc（缺失即失败）
    if (!QFile::exists(srcFrpc)) {
        qCWarning(frpInstallLog) << "frpc not found in archive:" << srcFrpc;
        return false;
    }
    if (!installBinary(srcFrpc, FrpManager::frpcPath()))
        return false;

    // 对应Python: 同时保存 frps（用于上传到服务器），缺失不算失败
    if (QFile::exists(srcFrps))
        installBinary(srcFrps, FrpManager::frpsPath());

    emit status(QStringLiteral("安装完成")); // 对应Python: status_callback("安装完成")
    return true;
}

// ---------------------------------------------------------------------------
// 远端 frps 部署
// ---------------------------------------------------------------------------

// 对应Python: FRPManager.ensure_frps_on_server + download_frps_for_server
bool FrpInstaller::ensureFrpsOnServer(CommandExecutor *exec, SftpClient *sftp)
{
    if (!exec || !sftp)
        return false;

    // 对应Python: ssh_conn.exec("test -f $HOME/frp/frps && echo 'exists'")
    {
        const ExecResult r =
            exec->exec(QStringLiteral("test -f $HOME/frp/frps && echo 'exists'"), false);
        if (r.stdoutText.contains(QStringLiteral("exists")))
            return true;
    }

    // 对应Python: get_server_arch(ssh_conn) -> `arch`
    QString serverArch;
    {
        const ExecResult r = exec->exec(QStringLiteral("arch"), false);
        serverArch = r.stdoutText.trimmed();
    }
    if (serverArch.isEmpty()) {
        // 对应Python: raise FRPInstallError("无法获取服务器架构")
        qCWarning(frpInstallLog) << "cannot detect server arch";
        emit status(QStringLiteral("无法获取服务器架构"));
        return false;
    }

    // 对应Python: get_remote_home_dir(ssh_conn) -> `echo $HOME`
    QString homeDir;
    {
        const ExecResult r = exec->exec(QStringLiteral("echo $HOME"), false);
        homeDir = r.stdoutText.trimmed();
    }
    if (homeDir.isEmpty()) {
        // 对应Python: status_callback("无法获取远程用户 home 目录")
        qCWarning(frpInstallLog) << "cannot detect remote home dir";
        emit status(QStringLiteral("无法获取远程用户 home 目录"));
        return false;
    }

    // 对应Python: SERVER_ARCH_MAP.get(server_arch.strip())
    const QString packageName = FrpManager::serverPackageNameForArch(serverArch);
    if (packageName.isEmpty()) {
        // 对应Python: status_callback(f"不支持的服务器架构: {server_arch}")
        emit status(QStringLiteral("不支持的服务器架构: %1").arg(serverArch));
        return false;
    }

    // 对应Python: status_callback(f"正在下载 frps (v{FRP_VERSION}) for {server_arch}...")
    emit status(QStringLiteral("正在下载 frps (v%1) for %2...")
                    .arg(FrpManager::kFrpVersion, serverArch));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        qCWarning(frpInstallLog) << "temp dir failed:" << tmp.errorString();
        return false;
    }
    const QString archivePath = QDir(tmp.path()).filePath(packageName);
    if (!downloadFile(downloadUrl(packageName), archivePath))
        return false;

    // 对应Python: status_callback("正在上传到服务器...")
    emit status(QStringLiteral("正在上传到服务器..."));

    // 对应Python: try: ssh_conn.exec(f"mkdir -p {home_dir}") except: pass
    exec->exec(QStringLiteral("mkdir -p %1").arg(homeDir), false);

    // 对应Python: sftp.put(archive_path, f"{home_dir}/{package_name}")
    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly)) {
        qCWarning(frpInstallLog) << "reopen archive failed:" << archive.errorString();
        return false;
    }
    const QByteArray payload = archive.readAll();
    archive.close();

    SshError sftpError;
    const QString remoteArchive = homeDir + QLatin1Char('/') + packageName;
    if (!sftp->writeFile(remoteArchive, payload, sftpError)) {
        qCWarning(frpInstallLog) << "upload failed:" << sftpError.message;
        return false;
    }

    // 对应Python: status_callback("正在服务器上解压...")
    emit status(QStringLiteral("正在服务器上解压..."));

    const ExecResult extractResult =
        exec->exec(remoteExtractCommand(homeDir, packageName), false);
    if (!extractResult.errorMessage.isEmpty() || extractResult.timedOut) {
        // 对应Python: except -> logger.error("服务器解压失败") + return False
        qCWarning(frpInstallLog) << "remote extract failed:" << extractResult.errorMessage;
        return false;
    }

    // 对应Python: status_callback("frps 部署完成")
    emit status(QStringLiteral("frps 部署完成"));
    return true;
}

// ---------------------------------------------------------------------------
// 私有实现
// ---------------------------------------------------------------------------

// 对应Python: core/frp_manager.py::download_with_progress
// Python 用 urllib.urlretrieve（阻塞）；这里用 QNAM + 局部事件循环同步等待，
// 两者都可在 worker 线程中直接调用。
bool FrpInstaller::downloadFile(const QString &url, const QString &destPath)
{
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(frpInstallLog) << "open dest failed:" << out.errorString();
        return false;
    }

    QNetworkAccessManager nam;
    QNetworkRequest request{QUrl(url)};
    // GitHub release 的下载地址会 302 到 objects.githubusercontent.com。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        out.write(reply->readAll());
    });
    // 对应Python: reporthook -> progress_callback(downloaded, total)
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [this](qint64 received, qint64 total) {
                         if (total > 0)
                             emit progress(received, total);
                     });
    loop.exec();

    const bool ok = (reply->error() == QNetworkReply::NoError);
    if (!ok)
        qCWarning(frpInstallLog) << "下载失败:" << reply->errorString();
    else
        out.write(reply->readAll()); // 收尾残留字节
    reply->deleteLater();
    out.close();

    if (!ok)
        QFile::remove(destPath);
    return ok;
}

// 对应Python: core/frp_manager.py::extract_archive
// Python 用 tarfile/zipfile；这里调系统 tar（Windows 内置 bsdtar 能解 zip）。
bool FrpInstaller::extractArchive(const QString &archivePath, const QString &extractDir)
{
    QStringList args;
    if (archivePath.endsWith(QStringLiteral(".tar.gz"))
        || archivePath.endsWith(QStringLiteral(".tgz"))) {
        args << QStringLiteral("-xzf");
    } else if (archivePath.endsWith(QStringLiteral(".zip"))) {
        args << QStringLiteral("-xf"); // bsdtar 自动识别 zip
    } else {
        qCWarning(frpInstallLog) << "不支持的归档格式:" << archivePath;
        return false;
    }
    args << archivePath << QStringLiteral("-C") << extractDir;

    QProcess tar;
#ifdef Q_OS_WIN
    // 防止解压时闪出控制台黑窗（对应Python: 无，Python 走库内解压不起进程）
    tar.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *cpArgs) {
        cpArgs->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    tar.start(QStringLiteral("tar"), args);
    if (!tar.waitForStarted(5000)) {
        qCWarning(frpInstallLog) << "解压失败: tar 无法启动";
        return false;
    }
    if (!tar.waitForFinished(120000)) {
        qCWarning(frpInstallLog) << "解压失败: tar 超时";
        tar.kill();
        tar.waitForFinished(1000);
        return false;
    }
    if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        qCWarning(frpInstallLog) << "解压失败:" << tar.readAllStandardError();
        return false;
    }
    return true;
}

// 对应Python: shutil.copy2 + os.chmod(..., S_IXUSR | S_IXGRP | S_IXOTH)
bool FrpInstaller::installBinary(const QString &srcPath, const QString &destPath)
{
    if (QFile::exists(destPath))
        QFile::remove(destPath); // QFile::copy 不覆盖已有文件
    if (!QFile::copy(srcPath, destPath)) {
        qCWarning(frpInstallLog) << "install failed:" << srcPath << "->" << destPath;
        return false;
    }
#ifndef Q_OS_WIN
    QFile f(destPath);
    f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup
                     | QFileDevice::ExeOther);
#endif
    qCInfo(frpInstallLog) << "installed" << destPath;
    return true;
}

} // namespace cubeshell
