// UpdateDownloader.cpp — see UpdateDownloader.h for the port map.
// 对应Python: core/update/worker.py(下载) + installer.py(调起)

#include "update/UpdateDownloader.h"

#include "config/GlobalState.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

#include "update/UpdateChecker.h" // kUserAgent

namespace cubeshell {

UpdateDownloader::UpdateDownloader(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

// 对应Python: platform_match.get_download_dir(appdirs user_data_dir/updates)
QString UpdateDownloader::downloadDir()
{
    const QString dir = GlobalState::dataDir() + QStringLiteral("/updates");
    QDir().mkpath(dir);
    return dir;
}

// 对应Python: worker.UpdateWorker._run_download
void UpdateDownloader::download(const QString &url, const QString &fileName,
                                qint64 expectedSize)
{
    if (m_reply) {
        emit downloadFailed(QStringLiteral("已有下载任务在进行中。"));
        return;
    }
    m_cancelled = false;
    m_expectedSize = expectedSize;
    m_finalPath = downloadDir() + QLatin1Char('/') + fileName;

    // .part 临时文件,完成校验后再重命名,避免半成品被当成完整包
    m_partFile.setFileName(m_finalPath + QStringLiteral(".part"));
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(QStringLiteral("无法写入下载文件:%1")
                                .arg(m_partFile.fileName()));
        return;
    }

    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(UpdateChecker::kUserAgent));
    // GitHub asset 直链会 302 到对象存储,Qt6 默认策略自动跟随重定向

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead,
            this, &UpdateDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &UpdateDownloader::onProgress);
    connect(m_reply, &QNetworkReply::finished,
            this, &UpdateDownloader::onFinished);
}

void UpdateDownloader::cancel()
{
    if (!m_reply)
        return;
    m_cancelled = true;
    m_reply->abort(); // finished() 收尾清理 .part
}

void UpdateDownloader::onReadyRead()
{
    if (m_reply && m_partFile.isOpen())
        m_partFile.write(m_reply->readAll());
}

void UpdateDownloader::onProgress(qint64 received, qint64 total)
{
    emit downloadProgress(received, total);
}

void UpdateDownloader::cleanupPart()
{
    if (m_partFile.isOpen())
        m_partFile.close();
    if (!m_partFile.fileName().isEmpty())
        QFile::remove(m_partFile.fileName());
}

void UpdateDownloader::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_cancelled) {
        cleanupPart();
        emit downloadFailed(QStringLiteral("下载已取消。"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        cleanupPart();
        emit downloadFailed(QStringLiteral("下载失败:%1").arg(reply->errorString()));
        return;
    }
    if (m_partFile.isOpen()) {
        m_partFile.write(reply->readAll());
        m_partFile.close();
    }

    // 校验:下载大小与 asset.size 一致(无校验和则只比大小)
    // 对应Python: worker._run_download 的 getsize 校验
    const qint64 actual = QFileInfo(m_partFile.fileName()).size();
    if (m_expectedSize > 0 && actual != m_expectedSize) {
        cleanupPart();
        emit downloadFailed(QStringLiteral("下载文件大小不匹配,可能已损坏,请重试。"));
        return;
    }

    QFile::remove(m_finalPath); // 覆盖旧包
    if (!QFile::rename(m_partFile.fileName(), m_finalPath)) {
        cleanupPart();
        emit downloadFailed(QStringLiteral("无法保存下载文件:%1").arg(m_finalPath));
        return;
    }
    emit downloadFinished(m_finalPath);
}

// ---------------------------------------------------------------------------
// platform installer launch
// ---------------------------------------------------------------------------

// 对应Python: installer.install_and_restart(简化为"调起",不做自替换脚本)
bool UpdateDownloader::launchInstaller(const QString &filePath, QString *errorOut)
{
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        if (errorOut)
            *errorOut = QStringLiteral("安装包不存在:%1").arg(filePath);
        return false;
    }
    const QString low = filePath.toLower();

#if defined(Q_OS_MACOS)
    // 对应Python: installer._install_macos / _fallback_open（macOS 分支）
    Q_UNUSED(low);
    return QProcess::startDetached(QStringLiteral("open"), {filePath});
#elif defined(Q_OS_WIN)
    if (low.endsWith(QLatin1String(".exe"))) {
        // 对应Python: installer._install_windows_inno(Inno Setup 静默安装)
        return QProcess::startDetached(filePath, {
            QStringLiteral("/VERYSILENT"), QStringLiteral("/NORESTART"),
            QStringLiteral("/NOCANCEL"), QStringLiteral("/CLOSEAPPLICATIONS"),
            QStringLiteral("/RESTARTAPPLICATIONS")});
    }
    // .zip 等其它格式交给系统默认程序(资源管理器)打开
    return QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
#elif defined(Q_OS_LINUX)
    if (low.endsWith(QLatin1String(".appimage"))) {
        QFile f(filePath);
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner
                         | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        return QProcess::startDetached(filePath, {});
    }
    if (low.endsWith(QLatin1String(".deb"))) {
        // 交给图形软件中心处理(gdebi/discover);无桌面环境时用户可手动 dpkg -i
        return QProcess::startDetached(QStringLiteral("xdg-open"), {filePath});
    }
    // .tar.xz/.tar.gz 等交给文件管理器
    return QProcess::startDetached(QStringLiteral("xdg-open"), {filePath});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
#endif
}

} // namespace cubeshell
