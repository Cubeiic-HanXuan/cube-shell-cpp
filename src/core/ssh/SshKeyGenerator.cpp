// SshKeyGenerator.cpp — 见 SshKeyGenerator.h。

#include "SshKeyGenerator.h"

#include <QCryptographicHash>

#ifdef CUBESHELL_WITH_SSH

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>
#include <openssl/opensslv.h>

namespace cubeshell {
namespace {

// ---- OpenSSH blob 拼装（RFC4253 wire format）----
// string = uint32 BE 长度 + 字节；mpint = 二进制大端幅值（最高位为 1 时前置 0x00）。

void putUint32(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>((v >> 24) & 0xff));
    out.append(static_cast<char>((v >> 16) & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
    out.append(static_cast<char>(v & 0xff));
}

void putString(QByteArray &out, const QByteArray &bytes)
{
    putUint32(out, static_cast<quint32>(bytes.size()));
    out.append(bytes);
}

void putMpint(QByteArray &out, const BIGNUM *bn)
{
    QByteArray mag(BN_num_bytes(bn), Qt::Uninitialized);
    BN_bn2bin(bn, reinterpret_cast<unsigned char *>(mag.data()));
    // mpint 若最高位置位会被误读成负数，须前置一个 0x00 字节。
    if (!mag.isEmpty() && (static_cast<unsigned char>(mag.at(0)) & 0x80))
        mag.prepend('\0');
    putString(out, mag);
}

// ---- 内存 BIO → QByteArray ----

QByteArray bioToByteArray(BIO *bio)
{
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    if (len <= 0 || !data)
        return QByteArray();
    return QByteArray(data, static_cast<int>(len));
}

// 序列化私钥为 PKCS#8 PEM。passphrase 为空则不加密，否则 AES-256-CBC。
bool writePrivateKeyPem(EVP_PKEY *pkey, const QString &passphrase,
                        QByteArray *pemOut, QString *errorOut)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        if (errorOut) *errorOut = QStringLiteral("无法分配 OpenSSL BIO");
        return false;
    }
    bool ok = false;
    if (passphrase.isEmpty()) {
        ok = PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    } else {
        const QByteArray pass = passphrase.toUtf8();
        ok = PEM_write_bio_PrivateKey(bio, pkey, EVP_aes_256_cbc(),
                                      reinterpret_cast<const unsigned char *>(pass.constData()),
                                      pass.size(), nullptr, nullptr) == 1;
    }
    if (ok)
        *pemOut = bioToByteArray(bio);
    else if (errorOut)
        *errorOut = QStringLiteral("私钥 PEM 序列化失败");
    BIO_free(bio);
    return ok;
}

// base64 后按 70 列折行，套上 BEGIN/END 头尾（OpenSSH 私钥外皮）。
QByteArray wrapOpensshPem(const QByteArray &body)
{
    QByteArray out = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    const QByteArray b64 = body.toBase64();
    for (int i = 0; i < b64.size(); i += 70) {
        out.append(b64.mid(i, 70));
        out.append('\n');
    }
    out.append("-----END OPENSSH PRIVATE KEY-----\n");
    return out;
}

// 无口令 Ed25519 → OpenSSH 原生私钥格式（openssh-key-v1）。
//
// 为什么 Ed25519 特殊：OpenSSH 的 ssh-keygen/ssh 对 Ed25519 只认自家
// openssh-key-v1，读不了 PKCS#8 PEM；而 libssh2（OpenSSL 后端）两种都认。
// 为了让生成的 Ed25519 私钥能被系统 ssh/scp 直接复用（选文件存储的初衷），
// 无口令时输出 OpenSSH 格式。带口令的 Ed25519 退回加密 PKCS#8（OpenSSH 格式
// 的加密要 bcrypt KDF，OpenSSL 不提供），仅 libssh2 可读。
bool writeEd25519Openssh(EVP_PKEY *pkey, const QString &comment,
                         QByteArray *out, QString *errorOut)
{
    unsigned char pub[32], priv[32];
    size_t publen = sizeof(pub), privlen = sizeof(priv);
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &publen) != 1 || publen != sizeof(pub) ||
        EVP_PKEY_get_raw_private_key(pkey, priv, &privlen) != 1 || privlen != sizeof(priv)) {
        if (errorOut) *errorOut = QStringLiteral("读取 Ed25519 原始密钥失败");
        return false;
    }

    // 公钥 blob（与 authorized_keys 用的是同一份）。
    QByteArray pubBlob;
    putString(pubBlob, QByteArray("ssh-ed25519"));
    putString(pubBlob, QByteArray(reinterpret_cast<const char *>(pub), sizeof(pub)));

    // 私钥节：checkint 对 + 类型 + 公钥 + (私钥种子‖公钥) + 注释 + 填充。
    quint32 check = 0;
    RAND_bytes(reinterpret_cast<unsigned char *>(&check), sizeof(check));
    QByteArray section;
    putUint32(section, check);
    putUint32(section, check);
    putString(section, QByteArray("ssh-ed25519"));
    putString(section, QByteArray(reinterpret_cast<const char *>(pub), sizeof(pub)));
    QByteArray priv64(reinterpret_cast<const char *>(priv), sizeof(priv));
    priv64.append(reinterpret_cast<const char *>(pub), sizeof(pub));
    putString(section, priv64);
    putString(section, comment.toUtf8());
    // cipher "none" 的块大小为 8：按 1,2,3… 填充到 8 的倍数。
    int pad = 1;
    while (section.size() % 8 != 0)
        section.append(static_cast<char>(pad++));

    // 容器：magic + cipher/kdf + 公钥列表 + 私钥节。
    QByteArray body("openssh-key-v1\0", 15);
    putString(body, QByteArray("none"));   // ciphername
    putString(body, QByteArray("none"));   // kdfname
    putString(body, QByteArray());         // kdfoptions
    putUint32(body, 1);                    // nkeys
    putString(body, pubBlob);
    putString(body, section);

    // 私钥种子已并入 body，尽快清掉栈上的明文。
    OPENSSL_cleanse(priv, sizeof(priv));

    *out = wrapOpensshPem(body);
    return true;
}

// ---- 各算法：keygen + 公钥 blob ----

EVP_PKEY *generatePkey(const SshKeyGenParams &params, int *curveNid, QString *errorOut)
{
    const QString &type = params.type;
    int evpType = 0;
    if (type == QLatin1String("ssh-ed25519"))
        evpType = EVP_PKEY_ED25519;
    else if (type == QLatin1String("ssh-rsa"))
        evpType = EVP_PKEY_RSA;
    else if (type.startsWith(QLatin1String("ecdsa-sha2-")))
        evpType = EVP_PKEY_EC;
    else {
        if (errorOut) *errorOut = QStringLiteral("不支持的密钥类型：%1").arg(type);
        return nullptr;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(evpType, nullptr);
    if (!ctx) {
        if (errorOut) *errorOut = QStringLiteral("无法创建 OpenSSL keygen 上下文");
        return nullptr;
    }
    EVP_PKEY *pkey = nullptr;
    bool ok = EVP_PKEY_keygen_init(ctx) > 0;

    if (ok && evpType == EVP_PKEY_RSA) {
        const int bits = params.bits >= 1024 ? params.bits : 2048;
        ok = EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) > 0;
    } else if (ok && evpType == EVP_PKEY_EC) {
        int nid = NID_X9_62_prime256v1;
        if (type.endsWith(QLatin1String("nistp384")))      nid = NID_secp384r1;
        else if (type.endsWith(QLatin1String("nistp521"))) nid = NID_secp521r1;
        if (curveNid) *curveNid = nid;
        ok = EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) > 0;
    }

    if (ok)
        ok = EVP_PKEY_keygen(ctx, &pkey) > 0;

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        if (errorOut) *errorOut = QStringLiteral("OpenSSL 密钥生成失败");
        return nullptr;
    }
    return pkey;
}

// Ed25519：string("ssh-ed25519") + string(raw32)
bool buildEd25519Blob(EVP_PKEY *pkey, QByteArray *blobOut, QString *errorOut)
{
    unsigned char raw[32];
    size_t len = sizeof(raw);
    if (EVP_PKEY_get_raw_public_key(pkey, raw, &len) != 1 || len != sizeof(raw)) {
        if (errorOut) *errorOut = QStringLiteral("读取 Ed25519 公钥失败");
        return false;
    }
    blobOut->clear();
    putString(*blobOut, QByteArray("ssh-ed25519"));
    putString(*blobOut, QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw)));
    return true;
}

// RSA：string("ssh-rsa") + mpint(e) + mpint(n)
bool buildRsaBlob(EVP_PKEY *pkey, QByteArray *blobOut, QString *errorOut)
{
    const BIGNUM *n = nullptr, *e = nullptr;
#if OPENSSL_VERSION_MAJOR >= 3
    BIGNUM *nn = nullptr, *ee = nullptr;
    if (EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &nn) != 1 ||
        EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &ee) != 1) {
        BN_free(nn);
        BN_free(ee);
        if (errorOut) *errorOut = QStringLiteral("读取 RSA 公钥参数失败");
        return false;
    }
    n = nn;   // get_bn_param 新分配，下面统一 free
    e = ee;
#else
    const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
    if (rsa)
        RSA_get0_key(rsa, &n, &e, nullptr);   // 借用内部指针，勿 free
    if (!n || !e) {
        if (errorOut) *errorOut = QStringLiteral("读取 RSA 公钥参数失败");
        return false;
    }
#endif
    blobOut->clear();
    putString(*blobOut, QByteArray("ssh-rsa"));
    putMpint(*blobOut, e);
    putMpint(*blobOut, n);
#if OPENSSL_VERSION_MAJOR >= 3
    BN_free(const_cast<BIGNUM *>(n));
    BN_free(const_cast<BIGNUM *>(e));
#endif
    return true;
}

// ECDSA：string("ecdsa-sha2-nistpN") + string("nistpN") + string(未压缩 SEC1 点)
bool buildEcdsaBlob(EVP_PKEY *pkey, const QString &type, QByteArray *blobOut, QString *errorOut)
{
    QByteArray point;
#if OPENSSL_VERSION_MAJOR >= 3
    size_t plen = 0;
    if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                        nullptr, 0, &plen) != 1 || plen == 0) {
        if (errorOut) *errorOut = QStringLiteral("读取 ECDSA 公钥失败");
        return false;
    }
    point.resize(static_cast<int>(plen));
    if (EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PUB_KEY,
                                        reinterpret_cast<unsigned char *>(point.data()),
                                        point.size(), &plen) != 1) {
        if (errorOut) *errorOut = QStringLiteral("读取 ECDSA 公钥失败");
        return false;
    }
    point.resize(static_cast<int>(plen));
#else
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
    const EC_GROUP *grp = ec ? EC_KEY_get0_group(ec) : nullptr;
    const EC_POINT *pub = ec ? EC_KEY_get0_public_key(ec) : nullptr;
    if (!grp || !pub) {
        if (errorOut) *errorOut = QStringLiteral("读取 ECDSA 公钥失败");
        return false;
    }
    const size_t plen = EC_POINT_point2oct(grp, pub, POINT_CONVERSION_UNCOMPRESSED,
                                           nullptr, 0, nullptr);
    point.resize(static_cast<int>(plen));
    EC_POINT_point2oct(grp, pub, POINT_CONVERSION_UNCOMPRESSED,
                       reinterpret_cast<unsigned char *>(point.data()), plen, nullptr);
#endif
    const QString curve = type.mid(QStringLiteral("ecdsa-sha2-").size());  // "nistpN"
    blobOut->clear();
    putString(*blobOut, type.toLatin1());
    putString(*blobOut, curve.toLatin1());
    putString(*blobOut, point);
    return true;
}

} // namespace

namespace SshKeyGenerator {

bool generate(const SshKeyGenParams &params, SshKeyGenResult *out, QString *errorOut)
{
    if (!out) {
        if (errorOut) *errorOut = QStringLiteral("内部错误：out 为空");
        return false;
    }
    int curveNid = 0;
    EVP_PKEY *pkey = generatePkey(params, &curveNid, errorOut);
    if (!pkey)
        return false;

    bool ok = false;
    QByteArray blob;
    const QString &type = params.type;
    if (type == QLatin1String("ssh-ed25519"))
        ok = buildEd25519Blob(pkey, &blob, errorOut);
    else if (type == QLatin1String("ssh-rsa"))
        ok = buildRsaBlob(pkey, &blob, errorOut);
    else
        ok = buildEcdsaBlob(pkey, type, &blob, errorOut);

    QByteArray pem;
    if (ok) {
        // 无口令 Ed25519 用 OpenSSH 原生格式（系统 ssh 可读）；其余一律 PKCS#8 PEM。
        if (type == QLatin1String("ssh-ed25519") && params.passphrase.isEmpty())
            ok = writeEd25519Openssh(pkey, sanitizeComment(params.comment), &pem, errorOut);
        else
            ok = writePrivateKeyPem(pkey, params.passphrase, &pem, errorOut);
    }
    EVP_PKEY_free(pkey);
    if (!ok)
        return false;

    out->type = type;
    if (type == QLatin1String("ssh-ed25519"))
        out->bits = 0;
    else if (type == QLatin1String("ssh-rsa"))
        out->bits = params.bits >= 1024 ? params.bits : 2048;
    else
        out->bits = (curveNid == NID_secp384r1) ? 384
                  : (curveNid == NID_secp521r1) ? 521 : 256;

    out->privateKeyPem = pem;
    out->publicKeyBlob = blob;
    const QString b64 = QString::fromLatin1(blob.toBase64());
    const QString comment = sanitizeComment(params.comment);
    out->publicKeyLine = comment.isEmpty()
        ? QStringLiteral("%1 %2").arg(type, b64)
        : QStringLiteral("%1 %2 %3").arg(type, b64, comment);
    out->fingerprint = fingerprintFromBlob(blob);
    return true;
}

} // namespace SshKeyGenerator
} // namespace cubeshell

#else  // !CUBESHELL_WITH_SSH

namespace cubeshell {
namespace SshKeyGenerator {
bool generate(const SshKeyGenParams &, SshKeyGenResult *, QString *errorOut)
{
    if (errorOut) *errorOut = QStringLiteral("SSH 支持未启用");
    return false;
}
} // namespace SshKeyGenerator
} // namespace cubeshell

#endif // CUBESHELL_WITH_SSH

// ---- 与 OpenSSL 无关的纯工具：两种构建下都可用（单测可覆盖） ----

namespace cubeshell {
namespace SshKeyGenerator {

QString fingerprintFromBlob(const QByteArray &publicKeyBlob)
{
    const QByteArray digest = QCryptographicHash::hash(publicKeyBlob, QCryptographicHash::Sha256);
    QString b64 = QString::fromLatin1(digest.toBase64());
    while (b64.endsWith(QLatin1Char('=')))   // 对齐 ssh-keygen -lf（不带 = 填充）
        b64.chop(1);
    return QStringLiteral("SHA256:%1").arg(b64);
}

QString typeNameFromBlob(const QByteArray &publicKeyBlob)
{
    if (publicKeyBlob.size() < 4)
        return QString();
    const quint32 len = (static_cast<quint32>(static_cast<unsigned char>(publicKeyBlob.at(0))) << 24)
                      | (static_cast<quint32>(static_cast<unsigned char>(publicKeyBlob.at(1))) << 16)
                      | (static_cast<quint32>(static_cast<unsigned char>(publicKeyBlob.at(2))) << 8)
                      | (static_cast<quint32>(static_cast<unsigned char>(publicKeyBlob.at(3))));
    if (publicKeyBlob.size() < 4 + static_cast<int>(len))
        return QString();
    return QString::fromLatin1(publicKeyBlob.mid(4, static_cast<int>(len)));
}

QString sanitizeComment(const QString &comment)
{
    QString out;
    out.reserve(comment.size());
    for (const QChar c : comment) {
        // 单引号/反斜杠/$/反引号会破坏远端 shell 的单引号包裹；换行会把一行
        // 公钥拆成两行。统一换成空格。
        if (c == QLatin1Char('\'') || c == QLatin1Char('\\') || c == QLatin1Char('$')
            || c == QLatin1Char('`') || c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            out.append(QLatin1Char(' '));
        else
            out.append(c);
    }
    return out.simplified();
}

} // namespace SshKeyGenerator
} // namespace cubeshell
