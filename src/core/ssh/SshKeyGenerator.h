#pragma once

// SshKeyGenerator.h — OpenSSH 兼容的密钥对生成器（Ed25519 / RSA / ECDSA）。
//
// libssh2 只负责「用既有私钥文件认证」（libssh2_userauth_publickey_fromfile），
// 本身没有 keygen API；而 shell 出 ssh-keygen 在 Windows / HarmonyOS 沙箱里
// 没有可执行体。OpenSSL 已是 cube_core 的直接链接依赖（见 core/CMakeLists.txt
// 的 CUBESHELL_WITH_SSH 段），故这里直接调它的 EVP API 生成密钥。
//
// 产物与 OpenSSH 完全互通（这是选「文件存储」的初衷——能被系统 ssh/scp 复用）：
//   * 私钥：PKCS#8 PEM（无口令不加密；有口令走 EVP_aes_256_cbc）
//   * 公钥：单行 "type base64 comment"（authorized_keys / .pub 格式）
//   * 指纹：SHA256:<base64-no-padding>（对齐 ssh-keygen -lf 的输出）
//
// 全部为纯静态函数、不碰网络，便于单测。整个实现仅在 CUBESHELL_WITH_SSH
// 下编译出真实逻辑；否则 generate() 一律返回「SSH 支持未启用」。

#include <QByteArray>
#include <QString>

namespace cubeshell {

// 生成参数。type 决定算法；bits/curveNid 只对相应类型生效。
struct SshKeyGenParams {
    // "ssh-ed25519" | "ssh-rsa" | "ecdsa-sha2-nistp256/384/521"
    QString type = QStringLiteral("ssh-ed25519");
    int     bits = 2048;          // 仅 RSA：2048/3072/4096
    QString comment;              // 公钥行尾注释（自由文本，部署前会净化）
    QString passphrase;           // 空 = 私钥不加密
};

// 一次生成的完整产物。
struct SshKeyGenResult {
    QString type;                 // 归一化后的算法名（同 params.type）
    int     bits = 0;             // Ed25519 为 0；RSA 为位长；ECDSA 为曲线尺寸
    QByteArray privateKeyPem;     // PKCS#8 PEM 私钥（含头尾 -----BEGIN/END-----）
    QByteArray publicKeyBlob;     // RFC4253 wire-format 公钥 blob（指纹算它）
    QString publicKeyLine;        // "type base64 comment"（可直接进 authorized_keys）
    QString fingerprint;          // "SHA256:<base64-no-padding>"
};

namespace SshKeyGenerator {

// 生成一对密钥。成功返回 true 并填满 out；失败返回 false 并填 errorOut。
// CUBESHELL_WITH_SSH=OFF 时恒 false（errorOut = "SSH 支持未启用"）。
bool generate(const SshKeyGenParams &params, SshKeyGenResult *out, QString *errorOut = nullptr);

// --- 纯工具（不依赖 OpenSSL，便于单测与复用） ---

// 由公钥 blob 算 OpenSSH 风格指纹：SHA256:<base64 去 = 填充>。
// 与 KnownHostsStore 的差异仅在于去掉末尾 '='，对齐 ssh-keygen -lf。
QString fingerprintFromBlob(const QByteArray &publicKeyBlob);

// 由公钥 blob 推出算法名（blob 第一个 string 字段即类型）。
QString typeNameFromBlob(const QByteArray &publicKeyBlob);

// 净化注释：剥离会破坏远端 shell 单引号包裹的字符（' \ $ ` 与换行），
// 换成空格并收敛首尾。生成时调用一次，部署 worker 还会再做单引号转义。
QString sanitizeComment(const QString &comment);

} // namespace SshKeyGenerator

} // namespace cubeshell
