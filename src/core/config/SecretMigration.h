#pragma once

// SecretMigration.h — 把 devices.json 里的明文密码搬进平台钥匙串。
//
// 历史上 devices.json 的每个条目都带一个明文 password 字段，文件权限还是
// 默认的 0644。这个模块负责一次性把它们搬进钥匙串，并从 JSON 里删掉。
//
// 为什么值得单独一个文件：这是**唯一一处可能把用户密码弄丢**的代码。
// 顺序错一步就是 21 条密码同时蒸发，且没有撤销。把它从 DeviceConfigStore
// 里拎出来，是为了让这个顺序能被单独读、单独测。

#include <QString>

namespace cubeshell {

class DeviceConfigStore;

namespace SecretMigration {

struct Result {
    enum Status {
        NotNeeded,      // 已经是新格式，无事可做
        Unsupported,    // 本平台钥匙串后端不可用 → 保持明文，只加固权限
        Migrated,       // 迁移成功，JSON 里已无密码
        Failed,         // 迁移失败，**明文原样保留**，可安全重试
    };
    Status status = NotNeeded;
    int migrated = 0;         // 搬进钥匙串的密码条数
    QString backupPath;       // 明文备份的位置（Migrated / Failed 时有值）
    QString error;            // Failed 时的原因
};

// 执行迁移。store 必须已经载入完毕，jsonPath 是它的落盘位置。
//
// 步骤（顺序不可调换，每一步都在防一种具体的丢失）：
//   0. 后端不可用 → Unsupported，不动任何东西
//   1. 备份明文到 <jsonPath>.plain.bak，权限 0600
//   2. 第一遍写：补齐 id 并落盘，**此时仍写明文**
//      —— 目的是让 id 先持久化。这步之后崩溃，重启能凭 id 续上，
//         不会重新分配 UUID 导致钥匙串里全是对不上号的孤儿
//   3. 存进钥匙串
//   4. 清缓存 → 重新读回 → 逐条比对。任一条不等 → Failed，明文保持不动
//   5. 通过 → 关掉 inlinePasswords → 第二遍写，此时才真正删掉明文
Result run(DeviceConfigStore &store, const QString &jsonPath);

} // namespace SecretMigration

} // namespace cubeshell
