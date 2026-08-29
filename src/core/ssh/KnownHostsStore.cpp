// KnownHostsStore.cpp — OpenSSH-compatible known_hosts persistence.

#include "KnownHostsStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include "config/GlobalState.h"

namespace cubeshell {

QString KnownHostsStore::defaultPath()
{
    return GlobalState::configDir() + QStringLiteral("/known_hosts");
}

std::shared_ptr<KnownHostsStore> KnownHostsStore::defaultInstance()
{
    static std::shared_ptr<KnownHostsStore> instance = []() {
        auto store = std::make_shared<KnownHostsStore>();
        QString err;
        if (!store->load(&err))
            qWarning() << "KnownHostsStore::defaultInstance load failed:" << err;
        return store;
    }();
    return instance;
}

KnownHostsStore::KnownHostsStore()
    : m_path(defaultPath())
{
}

KnownHostsStore::KnownHostsStore(const QString &path)
    : m_path(path)
{
}

bool KnownHostsStore::load(QString *errorOut)
{
    return load(m_path, errorOut);
}

bool KnownHostsStore::load(const QString &path, QString *errorOut)
{
    QMutexLocker lock(&m_mutex);

    m_path = path;
    m_records.clear();
    m_loaded = false;

    QFile file(path);
    if (!file.exists()) {
        m_loaded = true;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法打开 known_hosts: %1").arg(file.errorString());
        return false;
    }

    QTextStream in(&file);
    int lineNo = 0;
    while (!in.atEnd()) {
        ++lineNo;
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 3) {
            qWarning() << "KnownHostsStore: skipping malformed line" << lineNo;
            continue;
        }

        // OpenSSH format: [marker] hosts keytype base64-key [comment]
        // We don't support the @revoked / @cert-authority markers.
        int idx = 0;
        if (parts[0].startsWith(QLatin1Char('@'))) {
            qWarning() << "KnownHostsStore: markers not supported on line" << lineNo;
            continue;
        }

        const QString hostToken = parts[idx++];
        const QString keyType = parts[idx++];
        const QString base64Key = parts[idx++];

        QString host;
        quint16 port = 22;
        if (!parseHostPortToken(hostToken, &host, &port)) {
            qWarning() << "KnownHostsStore: unparseable host token on line" << lineNo;
            continue;
        }

        const QByteArray publicKey = decodeBase64Key(base64Key);
        if (publicKey.isEmpty()) {
            qWarning() << "KnownHostsStore: invalid base64 key on line" << lineNo;
            continue;
        }

        KnownHostRecord rec;
        rec.host = host;
        rec.port = port;
        rec.keyType = keyType;
        rec.publicKey = publicKey;
        m_records.append(rec);
    }

    m_loaded = true;
    return true;
}

bool KnownHostsStore::save(QString *errorOut) const
{
    return save(m_path, errorOut);
}

bool KnownHostsStore::save(const QString &path, QString *errorOut) const
{
    QMutexLocker lock(&m_mutex);

    const QFileInfo fi(path);
    const QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errorOut)
            *errorOut = QStringLiteral("无法创建目录: %1").arg(dir.absolutePath());
        return false;
    }

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入 known_hosts: %1").arg(out.errorString());
        return false;
    }

    QTextStream ts(&out);
    for (const auto &rec : std::as_const(m_records)) {
        ts << hostPortToken(rec.host, rec.port)
           << QLatin1Char(' ')
           << rec.keyType
           << QLatin1Char(' ')
           << encodeBase64Key(rec.publicKey)
           << QLatin1Char('\n');
    }

    if (!out.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("保存 known_hosts 失败: %1").arg(out.errorString());
        return false;
    }
    return true;
}

KnownHostsStore::CheckResult KnownHostsStore::check(const QString &host,
                                                     quint16 port,
                                                     const QByteArray &fingerprintSha256,
                                                     const QString &keyType) const
{
    QMutexLocker lock(&m_mutex);

    for (const auto &rec : std::as_const(m_records)) {
        if (rec.host == host && rec.port == port) {
            if (rec.keyType != keyType)
                return CheckResult::Mismatch;
            const QByteArray stored = KnownHostsStore::fingerprintSha256(rec.publicKey);
            if (stored == fingerprintSha256)
                return CheckResult::Match;
            return CheckResult::Mismatch;
        }
    }
    return CheckResult::NotFound;
}

bool KnownHostsStore::accept(const QString &host, quint16 port,
                             const QByteArray &publicKey,
                             const QString &keyType,
                             QString *errorOut)
{
    {
        QMutexLocker lock(&m_mutex);
        bool replaced = false;
        for (auto &rec : m_records) {
            if (rec.host == host && rec.port == port) {
                rec.keyType = keyType;
                rec.publicKey = publicKey;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            KnownHostRecord rec;
            rec.host = host;
            rec.port = port;
            rec.keyType = keyType;
            rec.publicKey = publicKey;
            m_records.append(rec);
        }
    }
    return save(errorOut);
}

bool KnownHostsStore::remove(const QString &host, quint16 port, QString *errorOut)
{
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_records.begin();
        while (it != m_records.end()) {
            if (it->host == host && it->port == port)
                it = m_records.erase(it);
            else
                ++it;
        }
    }
    return save(errorOut);
}

QString KnownHostsStore::path() const
{
    QMutexLocker lock(&m_mutex);
    return m_path;
}

int KnownHostsStore::count() const
{
    QMutexLocker lock(&m_mutex);
    return m_records.size();
}

QString KnownHostsStore::fingerprintDisplayString(const QByteArray &fingerprintSha256)
{
    return QStringLiteral("SHA256:%1").arg(QString::fromLatin1(fingerprintSha256.toBase64()));
}

QByteArray KnownHostsStore::fingerprintSha256(const QByteArray &publicKey)
{
    return QCryptographicHash::hash(publicKey, QCryptographicHash::Sha256);
}

QString KnownHostsStore::keyTypeFromLibssh2(int libssh2HostkeyType)
{
    switch (libssh2HostkeyType) {
    case 1: return QStringLiteral("ssh-rsa");
    case 2: return QStringLiteral("ssh-dss");
    case 3: return QStringLiteral("ecdsa-sha2-nistp256");
    case 4: return QStringLiteral("ecdsa-sha2-nistp384");
    case 5: return QStringLiteral("ecdsa-sha2-nistp521");
    case 6: return QStringLiteral("ssh-ed25519");
    default: return QStringLiteral("unknown");
    }
}

QString KnownHostsStore::hostPortToken(const QString &host, quint16 port)
{
    if (port == 22)
        return host;
    return QStringLiteral("[%1]:%2").arg(host).arg(port);
}

bool KnownHostsStore::parseHostPortToken(const QString &token, QString *host, quint16 *port)
{
    if (token.startsWith(QLatin1Char('[')) && token.contains(QLatin1Char(']'))) {
        const int close = token.indexOf(QLatin1Char(']'));
        *host = token.mid(1, close - 1);
        if (close + 1 < token.size() && token[close + 1] == QLatin1Char(':'))
            *port = token.mid(close + 2).toUShort();
        else
            *port = 22;
        return true;
    }
    if (token.contains(QLatin1Char(':'))) {
        // Could be an IPv6 literal without brackets; we can't reliably parse
        // that here. Skip it.
        return false;
    }
    *host = token;
    *port = 22;
    return true;
}

QByteArray KnownHostsStore::decodeBase64Key(const QString &base64)
{
    return QByteArray::fromBase64(base64.toLatin1());
}

QString KnownHostsStore::encodeBase64Key(const QByteArray &key)
{
    return QString::fromLatin1(key.toBase64());
}

} // namespace cubeshell
