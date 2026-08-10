// SecretMigration.cpp — 明文密码 → 钥匙串。见 SecretMigration.h 的步骤说明。

#include "SecretMigration.h"

#include <QFile>
#include <QFileInfo>

#include "ConfigUtil.h"
#include "DeviceConfigStore.h"
#include "Secrets.h"

namespace cubeshell {

namespace SecretMigration {

Result run(DeviceConfigStore &store, const QString &jsonPath)
{
    Result r;

    if (!store.needsMigration()) {
        r.status = Result::NotNeeded;
        return r;
    }

    // --- 0. 后端不可用就彻底不动 ------------------------------------------
    // Windows/Linux 的后端目前还是空壳（storeSecret 恒 false）。不先问这一句
    // 就照迁不误，等于把明文删掉却什么都没存进去。
    if (!Secrets::isAvailable()) {
        r.status = Result::Unsupported;
        return r;
    }

    // 没有任何密码要搬（全是密钥登录 / 空配置）——直接翻格式即可，
    // 不必备份也不必碰钥匙串。
    const QHash<QString, QString> snapshot = store.secretsSnapshot();
    int pending = 0;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it)
        if (!it.value().isEmpty())
            ++pending;
    if (pending == 0) {
        store.setInlinePasswords(false);
        QString err;
        if (!store.saveJson(jsonPath, &err)) {
            r.status = Result::Failed;
            r.error  = err;
            store.setInlinePasswords(true);
            return r;
        }
        r.status = Result::Migrated;
        return r;
    }

    // --- 1. 备份明文 --------------------------------------------------------
    // 用 writeSecure 而不是 QFile::copy：copy 继承源文件的 0644，备份本身
    // 就成了新的泄露点。writeSecure 落盘后会把权限收到 0600。
    //
    // 文件不存在是正常情况：设备是从 Python 版的 config.dat 读来的，
    // devices.json 还没写过。那种情况下 config.dat 本身就是备份，且我们从不删它。
    if (QFileInfo::exists(jsonPath)) {
        QFile src(jsonPath);
        if (!src.open(QIODevice::ReadOnly)) {
            r.status = Result::Failed;
            r.error  = QStringLiteral("无法读取 %1 做备份").arg(jsonPath);
            return r;
        }
        const QByteArray bytes = src.readAll();
        src.close();

        const QString backup = jsonPath + QStringLiteral(".plain.bak");
        QString err;
        if (!ConfigUtil::writeSecure(backup, bytes, &err)) {
            r.status = Result::Failed;
            r.error  = QStringLiteral("备份失败：%1").arg(err);
            return r;
        }
        r.backupPath = backup;
    }

    // --- 2. 第一遍写：持久化 id，仍写明文 -----------------------------------
    // inlinePasswords 此刻必须还是 true。先落 id 是为了让崩溃可续：
    // 重启后 id 还在，不会重新分配 UUID 而在钥匙串里留下一堆对不上号的孤儿。
    {
        QString err;
        if (!store.saveJson(jsonPath, &err)) {
            r.status = Result::Failed;
            r.error  = QStringLiteral("写入 id 失败：%1").arg(err);
            return r;
        }
    }

    // --- 3. 存进钥匙串 ------------------------------------------------------
    {
        QString err;
        if (!store.flushSecrets(&err)) {
            r.status = Result::Failed;
            r.error  = QStringLiteral("写入钥匙串失败：%1").arg(err);
            return r;
        }
    }

    // --- 4. 读回校验 --------------------------------------------------------
    // 走的是生产读路径（resolved()），而不是直接看内存表——要验证的正是
    // 「下次启动能不能读出来」，只对比内存等于什么都没验。
    store.invalidateSecretCache();
    for (const DeviceEntry &e : store.devices()) {
        const QString want = snapshot.value(e.id);
        if (want.isEmpty())
            continue;                       // 本就没密码的条目不参与校验
        if (store.resolved(e.name).password != want) {
            // 一条对不上就全盘中止。此时磁盘上是 pass-1 写的「id + 明文」，
            // 密码一条没少；恢复内存表后用户可以照常使用，下次启动会再试一次。
            store.restoreSecrets(snapshot);
            r.status = Result::Failed;
            r.error  = QStringLiteral("校验失败：设备「%1」的密码无法从钥匙串读回")
                           .arg(e.name);
            return r;
        }
    }

    // --- 5. 翻闸门 + 第二遍写：这一步才真正删掉明文 --------------------------
    store.setInlinePasswords(false);
    {
        QString err;
        if (!store.saveJson(jsonPath, &err)) {
            // 写失败 → 翻回去。磁盘上还是 pass-1 的明文版本，可安全重试。
            store.setInlinePasswords(true);
            r.status = Result::Failed;
            r.error  = QStringLiteral("删除明文失败：%1").arg(err);
            return r;
        }
    }

    r.status   = Result::Migrated;
    r.migrated = pending;
    return r;
}

} // namespace SecretMigration

} // namespace cubeshell
