// UpdateChecker.cpp — see UpdateChecker.h for the port map.
// 对应Python: core/update/github_api.py + version.py + platform_match.py

#include "update/UpdateChecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>

namespace cubeshell {

// ---------------------------------------------------------------------------
// semantic version parsing / comparison
// ---------------------------------------------------------------------------

// 对应Python: version._VERSION_RE
static const QRegularExpression &versionRe()
{
    static const QRegularExpression re(
        QStringLiteral("^[vV]?(\\d+)(?:\\.(\\d+))?(?:\\.(\\d+))?"
                       "(?:-([0-9A-Za-z.-]+))?(?:\\+([0-9A-Za-z.-]+))?$"));
    return re;
}

// 对应Python: version.parse_version
SemVer UpdateChecker::parseVersion(const QString &s)
{
    SemVer v;
    const QRegularExpressionMatch m = versionRe().match(s.trimmed());
    if (!m.hasMatch())
        return v;
    v.valid = true;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    v.patch = m.captured(3).isEmpty() ? 0 : m.captured(3).toInt();
    const QString pre = m.captured(4);
    if (pre.isEmpty()) {
        v.preRank = 1; // 正式版永远大于任何预发布
        return v;
    }
    const QStringList parts = pre.split(QLatin1Char('.'));
    const QString head = parts.first().toLower();
    // 对应Python: re.match(r'([a-z]+)(\d*)', head)
    static const QRegularExpression headRe(QStringLiteral("^([a-z]+)(\\d*)"));
    const QRegularExpressionMatch hm = headRe.match(head);
    const QString key = hm.hasMatch() ? hm.captured(1) : head;
    // 对应Python: version._PRE_RANK(未知标识按 -4 最弱处理)
    if (key == QLatin1String("dev"))
        v.preRank = -3;
    else if (key == QLatin1String("alpha") || key == QLatin1String("a"))
        v.preRank = -2;
    else if (key == QLatin1String("beta") || key == QLatin1String("b"))
        v.preRank = -1;
    else if (key == QLatin1String("rc"))
        v.preRank = 0;
    else
        v.preRank = -4;

    SemVer::PreId first;
    first.isNum = true;
    first.num = (hm.hasMatch() && !hm.captured(2).isEmpty())
                    ? hm.captured(2).toLongLong() : 0;
    v.preParts.append(first);
    for (int i = 1; i < parts.size(); ++i) {
        SemVer::PreId id;
        bool ok = false;
        const qint64 n = parts.at(i).toLongLong(&ok);
        if (ok && !parts.at(i).isEmpty()
            && parts.at(i).at(0).isDigit()) {
            id.isNum = true;
            id.num = n;
        } else {
            id.text = parts.at(i);
        }
        v.preParts.append(id);
    }
    return v;
}

// 对应Python: version.compare_versions
int UpdateChecker::compareVersions(const QString &a, const QString &b)
{
    const SemVer pa = parseVersion(a);
    const SemVer pb = parseVersion(b);
    if (!pa.valid || !pb.valid)
        return 0; // 无法解析视为相等,保守不升级

    // 1. 主版本号逐位数值比较
    if (pa.major != pb.major) return pa.major > pb.major ? 1 : -1;
    if (pa.minor != pb.minor) return pa.minor > pb.minor ? 1 : -1;
    if (pa.patch != pb.patch) return pa.patch > pb.patch ? 1 : -1;
    // 2. 同版本号下正式版 > 预发布
    if (pa.preRank != pb.preRank) return pa.preRank > pb.preRank ? 1 : -1;
    // 3. 同预发布类型,逐个比较标识符(数字标识符永远小于字母标识符)
    const int n = qMin(pa.preParts.size(), pb.preParts.size());
    for (int i = 0; i < n; ++i) {
        const SemVer::PreId &x = pa.preParts.at(i);
        const SemVer::PreId &y = pb.preParts.at(i);
        if (x.isNum && y.isNum) {
            if (x.num == y.num) continue;
            return x.num > y.num ? 1 : -1;
        }
        if (!x.isNum && !y.isNum) {
            if (x.text == y.text) continue;
            return x.text > y.text ? 1 : -1;
        }
        return x.isNum ? -1 : 1;
    }
    // 4. 字段更少的预发布版本更小
    if (pa.preParts.size() != pb.preParts.size())
        return pa.preParts.size() > pb.preParts.size() ? 1 : -1;
    return 0;
}

// 对应Python: version.is_newer
bool UpdateChecker::isNewer(const QString &remote, const QString &local)
{
    return compareVersions(remote, local) > 0;
}

// ---------------------------------------------------------------------------
// platform asset matching
// ---------------------------------------------------------------------------

// 对应Python: platform_match._PLATFORM_RULES + get_platform_key
struct PlatformRule {
    QStringList platKw;
    QStringList exts;   // 首选格式排前面
    QStringList archKw;
};

static bool currentPlatformRule(PlatformRule *out)
{
    const QString arch = QSysInfo::currentCpuArchitecture(); // x86_64 / arm64 / i386 / arm
#if defined(Q_OS_WIN)
    if (arch == QLatin1String("x86_64")) {
        *out = {{"windows", "win", "win64"}, {".zip", ".exe"}, {"x86_64", "x64", "amd64"}};
        return true;
    }
    if (arch == QLatin1String("i386")) {
        *out = {{"windows", "win", "win32"}, {".zip", ".exe"}, {"x86", "386", "32"}};
        return true;
    }
    return false;
#elif defined(Q_OS_MACOS)
    if (arch == QLatin1String("arm64")) {
        *out = {{"macos", "mac", "darwin"}, {".zip", ".dmg"}, {"arm64", "aarch64", "apple"}};
        return true;
    }
    if (arch == QLatin1String("x86_64")) {
        *out = {{"macos", "mac", "darwin"}, {".zip", ".dmg"}, {"x86_64", "x64", "amd64", "intel"}};
        return true;
    }
    return false;
#elif defined(Q_OS_LINUX)
    if (arch == QLatin1String("x86_64")) {
        *out = {{"linux"}, {".tar.xz", ".tar.gz"}, {"x86_64", "x64", "amd64"}};
        return true;
    }
    if (arch == QLatin1String("arm64")) {
        *out = {{"linux"}, {".tar.xz", ".tar.gz"}, {"aarch64", "arm64"}};
        return true;
    }
    if (arch.startsWith(QLatin1String("arm"))) {
        *out = {{"linux"}, {".tar.xz", ".tar.gz"}, {"armv7", "arm", "armhf"}};
        return true;
    }
    return false;
#else
    Q_UNUSED(out);
    return false;
#endif
}

// 对应Python: platform_match._ALL_ARCH_TOKENS
static const QStringList kAllArchTokens = {
    QStringLiteral("arm64"), QStringLiteral("aarch64"), QStringLiteral("x86_64"),
    QStringLiteral("x64"), QStringLiteral("amd64"), QStringLiteral("386"),
    QStringLiteral("x86"), QStringLiteral("armv7"), QStringLiteral("armhf"),
    QStringLiteral("intel"), QStringLiteral("apple"), QStringLiteral("win32"),
    QStringLiteral("win64"),
};

// 对应Python: platform_match.select_asset(唯一最高分;歧义/零匹配返回空)
UpdateAssetInfo UpdateChecker::selectAsset(const QList<UpdateAssetInfo> &assets)
{
    PlatformRule rule;
    if (!currentPlatformRule(&rule))
        return {};
    const QString primaryExt = rule.exts.first();

    QList<QPair<int, UpdateAssetInfo>> scored;
    for (const UpdateAssetInfo &a : assets) {
        const QString n = a.name.toLower();
        QString matchedExt;
        for (const QString &e : rule.exts) {
            if (n.endsWith(e)) { matchedExt = e; break; }
        }
        if (matchedExt.isEmpty())
            continue;
        bool platHit = false;
        for (const QString &k : rule.platKw)
            platHit = platHit || n.contains(k);
        if (!platHit)
            continue;
        // 若 asset 名带架构标识,则必须命中当前架构关键词
        bool hasArchToken = false;
        for (const QString &k : kAllArchTokens)
            hasArchToken = hasArchToken || n.contains(k);
        bool archHit = false;
        for (const QString &k : rule.archKw)
            archHit = archHit || n.contains(k);
        if (hasArchToken && !archHit)
            continue;
        // 评分:平台 +1,架构 +2,同名 cube-shell +1,首选格式 +2
        const int score = 1 + (archHit ? 2 : 0)
                          + (n.contains(QLatin1String("cube-shell")) ? 1 : 0)
                          + (matchedExt == primaryExt ? 2 : 0);
        scored.append({score, a});
    }
    if (scored.isEmpty())
        return {};
    std::sort(scored.begin(), scored.end(),
              [](const auto &l, const auto &r) { return l.first > r.first; });
    const int top = scored.first().first;
    int topCount = 0;
    for (const auto &p : scored)
        if (p.first == top) ++topCount;
    if (topCount > 1)
        return {}; // 同分歧义 → 走兜底,绝不自动装错架构
    return scored.first().second;
}

// ---------------------------------------------------------------------------
// GitHub API
// ---------------------------------------------------------------------------

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

// 对应Python: github_api.fetch_latest_release 的 JSON → ReleaseInfo 部分
UpdateReleaseInfo UpdateChecker::parseReleaseJson(const QByteArray &json, QString *errorOut)
{
    UpdateReleaseInfo info;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("解析 Release 失败:%1").arg(err.errorString());
        return info;
    }
    const QJsonObject obj = doc.object();
    info.tagName = obj.value(QStringLiteral("tag_name")).toString().trimmed();
    info.name    = obj.value(QStringLiteral("name")).toString().trimmed();
    info.body    = obj.value(QStringLiteral("body")).toString().trimmed();
    info.htmlUrl = obj.value(QStringLiteral("html_url")).toString();
    if (info.htmlUrl.isEmpty())
        info.htmlUrl = QString::fromLatin1(kReleasePage);
    const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &v : assets) {
        const QJsonObject a = v.toObject();
        UpdateAssetInfo asset;
        asset.name = a.value(QStringLiteral("name")).toString();
        asset.browserDownloadUrl =
            a.value(QStringLiteral("browser_download_url")).toString();
        asset.size = static_cast<qint64>(a.value(QStringLiteral("size")).toDouble());
        asset.contentType = a.value(QStringLiteral("content_type")).toString();
        info.assets.append(asset);
    }
    return info;
}

// 对应Python: worker.UpdateWorker._run_check + github_api.fetch_latest_release
void UpdateChecker::checkForUpdates(const QString &localVersion)
{
    if (m_reply)
        return; // 已有检查在进行
    m_localVersion = localVersion;

    QNetworkRequest req{QUrl(QString::fromLatin1(kGitHubApi))};
    // GitHub REST API 强制要求 User-Agent 头,缺失直接 403
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(15000); // 对应Python: timeout=15.0

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished,
            this, &UpdateChecker::onReplyFinished);
}

void UpdateChecker::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 对应Python: fetch_latest_release 的错误分类
    if (reply->error() != QNetworkReply::NoError) {
        if (httpStatus == 403) {
            const QByteArray remaining = reply->rawHeader("X-RateLimit-Remaining");
            if (remaining == "0") {
                emit checkFailed(QStringLiteral(
                    "GitHub API 访问过于频繁(匿名限频 60 次/小时),请稍后再试,"
                    "或手动访问 Release 页。"));
                return;
            }
            emit checkFailed(QStringLiteral("GitHub API 拒绝访问(HTTP 403)。"));
            return;
        }
        if (httpStatus == 404) {
            emit checkFailed(QStringLiteral("未找到任何 Release,请确认仓库已发布版本。"));
            return;
        }
        if (httpStatus > 0) {
            emit checkFailed(QStringLiteral("GitHub API 请求失败(HTTP %1)。").arg(httpStatus));
            return;
        }
        if (reply->error() == QNetworkReply::OperationCanceledError
            || reply->error() == QNetworkReply::TimeoutError) {
            emit checkFailed(QStringLiteral("连接 GitHub 超时,请检查网络或代理设置。"));
            return;
        }
        emit checkFailed(QStringLiteral("无法连接 GitHub,请检查网络:%1")
                             .arg(reply->errorString()));
        return;
    }

    QString parseError;
    const UpdateReleaseInfo rel = parseReleaseJson(reply->readAll(), &parseError);
    if (rel.tagName.isEmpty() && !parseError.isEmpty()) {
        emit checkFailed(parseError);
        return;
    }
    m_lastRelease = rel;

    if (isNewer(rel.tagName, m_localVersion)) {
        // url 优先取当前平台 asset 的直链,未匹配到则给 Release 页兜底
        const UpdateAssetInfo asset = selectAsset(rel.assets);
        const QString url = asset.browserDownloadUrl.isEmpty()
                                ? rel.htmlUrl : asset.browserDownloadUrl;
        emit updateAvailable(rel.tagName, url, rel.body);
    } else {
        emit noUpdateAvailable();
    }
}

} // namespace cubeshell
