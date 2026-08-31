#pragma once

// SshKeyStore.h — SSH 密钥管理：密钥索引（ssh_keys.json）+ 密钥文件落盘。
//
// 密钥本体以文件形式存到配置目录的 keys/ 子目录（私钥 0600），索引 JSON 只记
// 元数据（指纹/备注/类型/路径/创建时间）。选文件存储而非钥匙串，是为了让生成的
// 密钥能被系统 ssh/scp 等外部工具直接复用——私钥路径就直接填进 DeviceEntry.keyFile。
//
// 镜像 SnippetsStore 的持久化约定：读缺失/坏文件返回空表，写走
// ConfigUtil::writeSecure（原子写 + chmod 0600），见 config/snippets_store.h。
//
// 文件格式：
//   { "keys": [ { "id", "name", "type", "bits", "comment", "fingerprint",
//                 "publicKeyLine", "privateKeyPath", "publicKeyPath",
//                 "createdAt" }, ... ] }

#include <QList>
#include <QString>

#include "ssh/SshKeyGenerator.h"

namespace cubeshell {

struct SshKeyEntry {
    QString id;             // 稳定 id（QUuid），改名/改注释不变
    QString name;           // 显示名
    QString type;           // "ssh-ed25519" | "ssh-rsa" | "ecdsa-sha2-nistpN"
    int     bits = 0;       // Ed25519 为 0；RSA 位长；ECDSA 曲线尺寸
    QString comment;        // 公钥行尾注释
    QString fingerprint;    // "SHA256:<base64-no-padding>"
    QString publicKeyLine;  // 完整 authorized_keys 行（type base64 comment）
    QString privateKeyPath;
    QString publicKeyPath;
    QString createdAt;      // ISO 8601 UTC
};

class SshKeyStore {
public:
    // filePath 为空 -> GlobalState::configFilePath("ssh_keys.json")。
    explicit SshKeyStore(const QString &filePath = QString());

    QString filePath() const { return m_filePath; }
    // 密钥文件目录：与索引同级的 keys/ 子目录（测试用临时索引即得隔离目录）。
    QString keyDir() const;

    // 读全部密钥条目。文件缺失/坏 JSON 返回空表（不报错），同 SnippetsStore 容错。
    QList<SshKeyEntry> load() const;
    // 整表原子写回。
    bool save(const QList<SshKeyEntry> &keys, QString *errorOut = nullptr) const;

    // 便捷 CRUD（读→改→写回）。
    bool upsert(const SshKeyEntry &key, QString *errorOut = nullptr);
    // 删索引条目的同时尽力删掉对应密钥文件（幂等；文件删不掉不影响索引删除）。
    bool remove(const QString &id, QString *errorOut = nullptr);

    // 生成一对密钥、写私钥（0600）与公钥文件、登记进索引，返回新条目。
    // 失败时不落任何文件、不改索引，返回 false 并填 errorOut。
    bool createKey(const QString &name, const SshKeyGenParams &params,
                   SshKeyEntry *out, QString *errorOut = nullptr);

private:
    QString m_filePath;
};

} // namespace cubeshell
