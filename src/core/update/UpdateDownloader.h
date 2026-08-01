#pragma once

// UpdateDownloader.h — installer package download + platform installer launch.
// 对应Python: core/update/worker.py(下载阶段) + installer.py(调起部分)
//
// Download flow:
//   QNetworkAccessManager GET → write to <dataDir>/updates/<name>.part
//   → progress signals → size validation → rename to final path.
//
// Installer launch is a simplified port of installer.py: the package is
// handed to the platform (open / detached installer / xdg-open) instead of
// running the full self-replace scripts of the Python version.
//
// All work happens on the owner's event loop — no worker thread involved.

#include <QFile>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {

class UpdateDownloader : public QObject {
    Q_OBJECT
public:
    explicit UpdateDownloader(QObject *parent = nullptr);

    // Download dir: <dataDir>/updates(created on demand).
    // 对应Python: platform_match.get_download_dir
    static QString downloadDir();

    // Start downloading; expectedSize > 0 enables the size check.
    // 对应Python: worker.UpdateWorker._run_download
    void download(const QString &url, const QString &fileName,
                  qint64 expectedSize = 0);
    // 对应Python: worker.UpdateWorker.request_cancel
    void cancel();
    bool isDownloading() const { return m_reply != nullptr; }

    // Launch the platform installer for a downloaded package:
    //   macOS  → open .dmg/.zip
    //   Windows→ detached installer.exe (Inno silent) / open .zip
    //   Linux  → chmod+run AppImage / xdg-open .deb 及其它
    // 对应Python: installer.install_and_restart 的调起分支(简化版)
    static bool launchInstaller(const QString &filePath, QString *errorOut = nullptr);

signals:
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(const QString &filePath);
    void downloadFailed(const QString &error);

private slots:
    void onReadyRead();
    void onProgress(qint64 received, qint64 total);
    void onFinished();

private:
    void cleanupPart();

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile m_partFile;
    QString m_finalPath;
    qint64 m_expectedSize = 0;
    bool m_cancelled = false;
};

} // namespace cubeshell
