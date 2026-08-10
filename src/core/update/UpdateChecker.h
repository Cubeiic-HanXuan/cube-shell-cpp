#pragma once

// UpdateChecker.h — GitHub Releases update check.
// 对应Python: core/update/github_api.py + version.py + platform_match.py
//
// - fetch latest release   对应Python: github_api.fetch_latest_release
// - semantic version cmp   对应Python: version.parse_version/compare_versions
// - platform asset match   对应Python: platform_match.select_asset
//
// All network work runs on the QNetworkAccessManager event loop (no worker
// thread), so signals are emitted on the owner's thread.

#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {

// 对应Python: github_api.AssetInfo
struct UpdateAssetInfo {
    QString name;
    QString browserDownloadUrl;
    qint64 size = 0;
    QString contentType;
};

// 对应Python: github_api.ReleaseInfo
struct UpdateReleaseInfo {
    QString tagName;  // 原始,可能带 v 前缀
    QString name;
    QString body;     // 更新说明(markdown 原文)
    QString htmlUrl;  // 浏览器打开的 release 页(兜底用)
    QList<UpdateAssetInfo> assets;
};

// Parsed semantic version. 对应Python: version.parse_version 的返回结构
struct SemVer {
    bool valid = false;
    int major = 0;
    int minor = 0;
    int patch = 0;
    // 正式版 preRank=1;dev=-3 alpha=-2 beta=-1 rc=0;未知预发布 -4
    int preRank = 1;
    struct PreId {
        bool isNum = false;
        qint64 num = 0;
        QString text;
    };
    QList<PreId> preParts;
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    // 对应Python: github_api.GITHUB_API / RELEASE_PAGE / USER_AGENT
    // 指向 C++ 重写版仓库 cube-shell-cpp。老仓库 cube-shell 是 Python 版，
    // 最新 tag 停在 V2.8.0，资产命名也不同（cube-shell-<os>-<arch>.tar.xz），
    // 继续指过去会把 3.x 用户往回推 2.8.0。
    static constexpr const char *kGitHubApi =
        "https://api.github.com/repos/Cubeiic-HanXuan/cube-shell-cpp/releases/latest";
    static constexpr const char *kReleasePage =
        "https://github.com/Cubeiic-HanXuan/cube-shell-cpp/releases/latest";
    static constexpr const char *kUserAgent =
        "cube-shell-updater (+https://github.com/Cubeiic-HanXuan/cube-shell-cpp)";

    explicit UpdateChecker(QObject *parent = nullptr);

    // Kick off an asynchronous check against the given local version.
    // 对应Python: worker.UpdateWorker._run_check
    void checkForUpdates(const QString &localVersion);
    bool isChecking() const { return m_reply != nullptr; }

    // Release fetched by the last successful check (for the downloader).
    UpdateReleaseInfo lastRelease() const { return m_lastRelease; }

    // --- pure helpers (unit-testable) ---

    // 对应Python: version.parse_version(无法解析返回 valid=false)
    static SemVer parseVersion(const QString &s);
    // 对应Python: version.compare_versions(任一无法解析返回 0)
    static int compareVersions(const QString &a, const QString &b);
    // 对应Python: version.is_newer
    static bool isNewer(const QString &remote, const QString &local);

    // 对应Python: platform_match.select_asset(评分制,歧义返回空 name)
    static UpdateAssetInfo selectAsset(const QList<UpdateAssetInfo> &assets);
    // Parse the GitHub JSON payload (exposed for tests).
    static UpdateReleaseInfo parseReleaseJson(const QByteArray &json, QString *errorOut);

signals:
    void updateAvailable(const QString &version, const QString &url,
                         const QString &changelog);
    void noUpdateAvailable();
    void checkFailed(const QString &error);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    QString m_localVersion;
    UpdateReleaseInfo m_lastRelease;
};

} // namespace cubeshell
