#include "SshKeyStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "config/ConfigUtil.h"
#include "config/GlobalState.h"

namespace cubeshell {

SshKeyStore::SshKeyStore(const QString &filePath)
    : m_filePath(filePath.isEmpty() ? GlobalState::configFilePath(QStringLiteral("ssh_keys.json"))
                                    : filePath)
{
}

QString SshKeyStore::keyDir() const
{
    return QFileInfo(m_filePath).absolutePath() + QStringLiteral("/keys");
}

QList<SshKeyEntry> SshKeyStore::load() const
{
    QList<SshKeyEntry> out;
    const QJsonValue root = ConfigUtil::readJson(m_filePath);
    if (!root.isObject())
        return out;   // 缺失/坏文件 = 空表

    const QJsonArray arr = root.toObject().value(QStringLiteral("keys")).toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SshKeyEntry k;
        k.id = o.value(QStringLiteral("id")).toString();
        if (k.id.isEmpty())   // 旧数据没 id：补一个，保住定位不变量
            k.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        k.name = o.value(QStringLiteral("name")).toString();
        k.type = o.value(QStringLiteral("type")).toString();
        k.bits = o.value(QStringLiteral("bits")).toInt(0);
        k.comment = o.value(QStringLiteral("comment")).toString();
        k.fingerprint = o.value(QStringLiteral("fingerprint")).toString();
        k.publicKeyLine = o.value(QStringLiteral("publicKeyLine")).toString();
        k.privateKeyPath = o.value(QStringLiteral("privateKeyPath")).toString();
        k.publicKeyPath = o.value(QStringLiteral("publicKeyPath")).toString();
        k.createdAt = o.value(QStringLiteral("createdAt")).toString();
        if (k.name.isEmpty() && k.publicKeyLine.isEmpty())
            continue;   // 空条目没意义
        out.append(k);
    }
    return out;
}

bool SshKeyStore::save(const QList<SshKeyEntry> &keys, QString *errorOut) const
{
    QJsonArray arr;
    for (const SshKeyEntry &k : keys) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), k.id);
        o.insert(QStringLiteral("name"), k.name);
        o.insert(QStringLiteral("type"), k.type);
        o.insert(QStringLiteral("bits"), k.bits);
        o.insert(QStringLiteral("comment"), k.comment);
        o.insert(QStringLiteral("fingerprint"), k.fingerprint);
        o.insert(QStringLiteral("publicKeyLine"), k.publicKeyLine);
        o.insert(QStringLiteral("privateKeyPath"), k.privateKeyPath);
        o.insert(QStringLiteral("publicKeyPath"), k.publicKeyPath);
        o.insert(QStringLiteral("createdAt"), k.createdAt);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("keys"), arr);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return ConfigUtil::writeSecure(m_filePath, data, errorOut);
}

bool SshKeyStore::upsert(const SshKeyEntry &key, QString *errorOut)
{
    QList<SshKeyEntry> all = load();
    SshKeyEntry k = key;
    if (k.id.isEmpty())
        k.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool found = false;
    for (SshKeyEntry &cur : all) {
        if (cur.id == k.id) {
            cur = k;
            found = true;
            break;
        }
    }
    if (!found)
        all.append(k);
    return save(all, errorOut);
}

bool SshKeyStore::remove(const QString &id, QString *errorOut)
{
    QList<SshKeyEntry> all = load();
    for (auto it = all.begin(); it != all.end(); ++it) {
        if (it->id == id) {
            // 先记下密钥文件路径再删索引条目，文件删除失败不影响索引收口。
            const QString priv = it->privateKeyPath;
            const QString pub = it->publicKeyPath;
            all.erase(it);
            if (!save(all, errorOut))
                return false;
            if (!priv.isEmpty()) QFile::remove(priv);
            if (!pub.isEmpty())  QFile::remove(pub);
            return true;
        }
    }
    return true;   // 不存在视为已删除（幂等）
}

bool SshKeyStore::createKey(const QString &name, const SshKeyGenParams &params,
                            SshKeyEntry *out, QString *errorOut)
{
    SshKeyGenResult gen;
    if (!SshKeyGenerator::generate(params, &gen, errorOut))
        return false;

    QDir dir;
    if (!dir.mkpath(keyDir())) {
        if (errorOut) *errorOut = QStringLiteral("无法创建密钥目录：%1").arg(keyDir());
        return false;
    }

    SshKeyEntry k;
    k.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 文件名用 id 派生而非名称派生：改名不会撞文件名。
    const QString shortId = k.id.left(8);
    k.privateKeyPath = keyDir() + QStringLiteral("/id_%1").arg(shortId);
    k.publicKeyPath = k.privateKeyPath + QStringLiteral(".pub");
    k.name = name.isEmpty() ? QStringLiteral("key-%1").arg(shortId) : name;
    k.type = gen.type;
    k.bits = gen.bits;
    k.comment = SshKeyGenerator::sanitizeComment(params.comment);
    k.fingerprint = gen.fingerprint;
    k.publicKeyLine = gen.publicKeyLine;
    k.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // 私钥必须 0600；公钥同样走 writeSecure（0600 对 .pub 也无妨）。
    if (!ConfigUtil::writeSecure(k.privateKeyPath, gen.privateKeyPem, errorOut))
        return false;
    if (!ConfigUtil::writeSecure(k.publicKeyPath, gen.publicKeyLine.toUtf8() + '\n', errorOut)) {
        QFile::remove(k.privateKeyPath);   // 公钥写失败时把已落盘的私钥一起回滚
        return false;
    }

    if (!upsert(k, errorOut)) {
        QFile::remove(k.privateKeyPath);
        QFile::remove(k.publicKeyPath);
        return false;
    }

    if (out)
        *out = k;
    return true;
}

} // namespace cubeshell
