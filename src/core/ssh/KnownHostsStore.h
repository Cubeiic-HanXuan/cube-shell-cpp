#pragma once

// KnownHostsStore.h — OpenSSH-compatible known_hosts persistence for SSH
// server host key verification.
//
// Unlike libssh2's LIBSSH2_KNOWNHOSTS API, this store does not require a live
// session to parse or query entries. It keeps a simple in-memory list of
// records and supports loading/saving the standard known_hosts line format.

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>

namespace cubeshell {

struct KnownHostRecord {
    QString host;       // normalized host (brackets stripped)
    quint16 port = 22;  // normalized port
    QString keyType;    // e.g. "ssh-ed25519", "ecdsa-sha2-nistp256"
    QByteArray publicKey; // raw decoded public key bytes
};

class KnownHostsStore {
public:
    // Default path: GlobalState::configDir()/known_hosts
    static QString defaultPath();

    // Thread-safe singleton backed by defaultPath(). The first call loads the
    // file; later calls return the already-loaded instance.
    static std::shared_ptr<KnownHostsStore> defaultInstance();

    KnownHostsStore();
    explicit KnownHostsStore(const QString &path);

    // Load from disk. If the file does not exist, the store is empty and valid.
    bool load(QString *errorOut = nullptr);
    bool load(const QString &path, QString *errorOut = nullptr);

    // Save current entries to disk (atomic write to a temp file then rename).
    bool save(QString *errorOut = nullptr) const;
    bool save(const QString &path, QString *errorOut = nullptr) const;

    enum class CheckResult {
        Match,      // host:port exists and the key matches
        Mismatch,   // host:port exists but the key differs (possible MITM)
        NotFound,   // no record for this host:port
    };

    // Check a server fingerprint against the store.
    // fingerprintSha256 is the raw 32-byte SHA-256 digest of the server's
    // public key (the same binary data libssh2_hostkey_hash returns).
    CheckResult check(const QString &host, quint16 port,
                      const QByteArray &fingerprintSha256,
                      const QString &keyType) const;

    // Add or replace a record for host:port and persist.
    bool accept(const QString &host, quint16 port,
                const QByteArray &publicKey,
                const QString &keyType,
                QString *errorOut = nullptr);

    // Remove the record for host:port and persist.
    bool remove(const QString &host, quint16 port, QString *errorOut = nullptr);

    bool isLoaded() const { return m_loaded; }
    QString path() const;
    int count() const;

    // Compute the OpenSSH-style SHA256 fingerprint display string.
    static QString fingerprintDisplayString(const QByteArray &fingerprintSha256);
    static QByteArray fingerprintSha256(const QByteArray &publicKey);

    // Convert a libssh2 host key type constant to an OpenSSH key type string.
    static QString keyTypeFromLibssh2(int libssh2HostkeyType);

private:
    QString m_path;
    mutable QMutex m_mutex;
    QList<KnownHostRecord> m_records;
    bool m_loaded = false;

    static QString hostPortToken(const QString &host, quint16 port);
    static bool parseHostPortToken(const QString &token, QString *host, quint16 *port);
    static QByteArray decodeBase64Key(const QString &base64);
    static QString encodeBase64Key(const QByteArray &key);
};

} // namespace cubeshell
