// Secrets.cpp — platform keychain abstraction. See Secrets.h.

#include "Secrets.h"

#include <QByteArray>
#include <QHash>
#include <QStringList>

#if defined(CUBESHELL_PLATFORM_OHOS)
#include <QSettings>
#endif

#include "GlobalState.h"

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace cubeshell {

namespace Secrets {

#if defined(Q_OS_MACOS)

// --- macOS: generic passwords in the login Keychain (keyring-compatible) ---

// Small RAII wrapper so every CFTypeRef is released on scope exit.
namespace {
struct CFReleaser {
    CFTypeRef ref = nullptr;
    explicit CFReleaser(CFTypeRef r) : ref(r) {}
    ~CFReleaser() { if (ref) CFRelease(ref); }
    CFReleaser(const CFReleaser &) = delete;
    CFReleaser &operator=(const CFReleaser &) = delete;
};

CFStringRef toCfString(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8 *>(utf8.constData()),
                                   utf8.size(), kCFStringEncodingUTF8, false);
}

QString osStatusMessage(OSStatus status)
{
    CFStringRef msg = SecCopyErrorMessageString(status, nullptr);
    if (!msg)
        return QStringLiteral("OSStatus %1").arg(status);
    CFReleaser guard(msg);
    const CFIndex len = CFStringGetLength(msg);
    QString out(int(len), QChar(0));
    CFStringGetCharacters(msg, CFRangeMake(0, len),
                          reinterpret_cast<UniChar *>(out.data()));
    return out;
}
} // namespace

// 对应Python: keyring.set_password (macOS Keychain 后端)
bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    CFStringRef cfService = toCfString(service);
    CFStringRef cfAccount = toCfString(account);
    CFReleaser g1(cfService), g2(cfAccount);

    const QByteArray secretUtf8 = secret.toUtf8();
    CFDataRef cfSecret = CFDataCreate(kCFAllocatorDefault,
                                      reinterpret_cast<const UInt8 *>(secretUtf8.constData()),
                                      secretUtf8.size());
    CFReleaser g3(cfSecret);

    // Query identifying the item (class + service + account).
    const void *queryKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void *queryVals[] = {kSecClassGenericPassword, cfService, cfAccount};
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, queryKeys, queryVals, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFReleaser g4(query);

    // Try updating an existing item first (keyring's set_password overwrites).
    const void *updKeys[] = {kSecValueData};
    const void *updVals[] = {cfSecret};
    CFDictionaryRef update = CFDictionaryCreate(kCFAllocatorDefault, updKeys, updVals, 1,
                                                &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    CFReleaser g5(update);

    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        // Not there yet: add a fresh item.
        const void *addKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void *addVals[] = {kSecClassGenericPassword, cfService, cfAccount, cfSecret};
        CFDictionaryRef add = CFDictionaryCreate(kCFAllocatorDefault, addKeys, addVals, 4,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
        CFReleaser g6(add);
        status = SecItemAdd(add, nullptr);
    }
    if (status != errSecSuccess) {
        if (errorOut) *errorOut = osStatusMessage(status);
        return false;
    }
    return true;
}

// 对应Python: keyring.get_password (macOS Keychain 后端)
QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    CFStringRef cfService = toCfString(service);
    CFStringRef cfAccount = toCfString(account);
    CFReleaser g1(cfService), g2(cfAccount);

    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                          kSecReturnData, kSecMatchLimit};
    const void *vals[] = {kSecClassGenericPassword, cfService, cfAccount,
                          kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFReleaser g3(query);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    if (status == errSecItemNotFound)
        return QString(); // Python: keyring returns None -> "" here
    if (status != errSecSuccess) {
        if (errorOut) *errorOut = osStatusMessage(status);
        return QString();
    }
    CFReleaser g4(result);
    CFDataRef data = static_cast<CFDataRef>(result);
    return QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                             int(CFDataGetLength(data)));
}

// 对应Python: keyring.delete_password (macOS Keychain 后端)
bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    CFStringRef cfService = toCfString(service);
    CFStringRef cfAccount = toCfString(account);
    CFReleaser g1(cfService), g2(cfAccount);

    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void *vals[] = {kSecClassGenericPassword, cfService, cfAccount};
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFReleaser g3(query);

    const OSStatus status = SecItemDelete(query);
    if (status != errSecSuccess) {
        if (errorOut) *errorOut = osStatusMessage(status);
        return false;
    }
    return true;
}

// Security.framework 随系统提供，不存在「装没装」的问题。
bool isAvailable() { return true; }

#elif defined(Q_OS_WIN)

// --- Windows: DPAPI (CryptProtectData) + 文件存储 ---
//
// Windows 没有「钥匙串」式的系统密钥服务可存任意账号密码；keyring 的 Windows
// 后端走的是 Credential Manager，但那套 API 面向「站点凭据」，对纯粹的
// service/account 机密并不顺手。这里用 DPAPI：CryptProtectData 用当前登录用户
// 的主密钥加密，密文只有同一用户能解（CRYPTPROTECT_UI_FORBIDDEN 保证绝不弹
// UI）。密文落盘到 dataDir()/secrets/<hash>.bin，哈希避免 service/account 里
// 的非法字符进文件名。
//
// 与 keyring 的差异：密文文件而非凭据管理器，机密性等价（同为每用户 DPAPI），
// 但不与其他凭据管理器客户端互读——本应用自成一体，无跨进程共享需求。
//
// 对应Python: keyring Windows 后端的 per-user 保护语义（无提示、当前用户可解）。

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <windows.h>
#include <wincrypt.h>   // CryptProtectData / CryptUnprotectData（链 Crypt32）

namespace {

// 把 service+account 映射成稳定的文件名（哈希避免路径非法字符与长度问题）。
QString secretFilePath(const QString &service, const QString &account)
{
    const QByteArray key = (service + QLatin1Char('\n') + account).toUtf8();
    const QByteArray hex = QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex();
    return GlobalState::dataDir() + QStringLiteral("/secrets/") + QString::fromLatin1(hex)
           + QStringLiteral(".bin");
}

// DPAPI 加密一段字节。成功返回密文，失败返回空 QByteArray 并填 errorOut。
QByteArray dpapiProtect(const QByteArray &plain, QString *errorOut)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    // CRYPTPROTECT_UI_FORBIDDEN：绝不弹 UI（守护进程/无桌面场景必须）。
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (errorOut)
            *errorOut = QStringLiteral("CryptProtectData 失败 (0x%1)")
                            .arg(GetLastError(), 0, 16);
        return QByteArray();
    }
    const QByteArray blob(reinterpret_cast<const char *>(out.pbData),
                          int(out.cbData));
    LocalFree(out.pbData);
    return blob;
}

QByteArray dpapiUnprotect(const QByteArray &blob, QString *errorOut)
{
    if (blob.isEmpty())
        return QByteArray();
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (errorOut)
            *errorOut = QStringLiteral("CryptUnprotectData 失败 (0x%1)")
                            .arg(GetLastError(), 0, 16);
        return QByteArray();
    }
    const QByteArray plain(reinterpret_cast<const char *>(out.pbData),
                           int(out.cbData));
    LocalFree(out.pbData);
    return plain;
}

} // namespace

bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    const QByteArray blob = dpapiProtect(secret.toUtf8(), errorOut);
    if (blob.isEmpty())
        return false;

    const QString path = secretFilePath(service, account);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 无法写入 %1").arg(path);
        return false;
    }
    if (f.write(blob) != blob.size()) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 写入 %1 不完整").arg(path);
        return false;
    }
    return true;
}

QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    QFile f(secretFilePath(service, account));
    if (!f.exists())
        return QString();   // 未存过：与 macOS「找不到返回空且不报错」一致
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 无法读取机密文件");
        return QString();
    }
    const QByteArray blob = f.readAll();
    const QByteArray plain = dpapiUnprotect(blob, errorOut);
    return QString::fromUtf8(plain);
}

bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    const QString path = secretFilePath(service, account);
    if (!QFile::exists(path))
        return false;   // 与 keyring.delete_password 找不到条目返回失败一致
    if (!QFile::remove(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 删除 %1 失败").arg(path);
        return false;
    }
    return true;
}

// DPAPI 随系统提供（Crypt32 是 Windows 核心组件），无需运行期探测。
bool isAvailable() { return true; }

#elif defined(CUBESHELL_PLATFORM_OHOS)

// --- HarmonyOS: 应用沙箱内的本地加密文件 ---
//
// 鸿蒙的系统级密钥库是 HUKS（HarmonyOS Universal KeyStore），需经 NDK 的
// huks C 接口访问。一期先用沙箱内文件落地，理由：
//   * 鸿蒙应用沙箱本身已隔离到 /data/app/el2/<userid>/base/<bundle>/，
//     其他应用无法读取，威胁模型接近 Linux 下 ~/.config 的 0600 文件；
//   * HUKS 接口需要在真机上反复验证，放到二期与 USB/串口一并做。
// TODO(ohos): 二期接 HUKS（OH_Huks_GenerateKeyItem / EncryptData），
//             把下面的 obfuscate() 换成真正的 AES-GCM。
//
// 存储格式：QSettings(IniFormat) 于 GlobalState::configFilePath("secrets.ini")，
// 键为 "<service>/<account>"，值为混淆后的 base64。

namespace {

// 轻量混淆：与固定盐 XOR。**不是加密**，只防止明文直接可读；
// 真正的机密性依赖沙箱隔离。二期换 HUKS 后此函数删除。
QByteArray xorWithSalt(const QByteArray &in)
{
    static const char kSalt[] = "cube-shell/ohos/v1";
    const int saltLen = int(sizeof(kSalt) - 1);
    QByteArray out(in.size(), Qt::Uninitialized);
    for (int i = 0; i < in.size(); ++i)
        out[i] = char(in[i] ^ kSalt[i % saltLen]);
    return out;
}

QByteArray obfuscate(const QByteArray &plain)
{
    return xorWithSalt(plain).toBase64();
}

QByteArray deobfuscate(const QByteArray &b64)
{
    return xorWithSalt(QByteArray::fromBase64(b64));
}

QString secretsFilePath()
{
    return GlobalState::configFilePath(QStringLiteral("secrets.ini"));
}

QString secretKey(const QString &service, const QString &account)
{
    return service + QLatin1Char('/') + account;
}

} // namespace

bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    QSettings store(secretsFilePath(), QSettings::IniFormat);
    store.setValue(secretKey(service, account),
                   QString::fromLatin1(obfuscate(secret.toUtf8())));
    store.sync();
    if (store.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 写入 %1 失败").arg(secretsFilePath());
        return false;
    }
    return true;
}

QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    QSettings store(secretsFilePath(), QSettings::IniFormat);
    const QString stored = store.value(secretKey(service, account)).toString();
    if (stored.isEmpty()) {
        // 未存过不算错误——与 macOS 分支「找不到条目返回空且不报错」一致。
        Q_UNUSED(errorOut);
        return QString();
    }
    return QString::fromUtf8(deobfuscate(stored.toLatin1()));
}

bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    QSettings store(secretsFilePath(), QSettings::IniFormat);
    store.remove(secretKey(service, account));
    store.sync();
    if (store.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("Secrets: 删除失败");
        return false;
    }
    return true;
}

// 沙箱内文件后端总是可用（写不进去时 storeSecret 自己会报错）。
// 注意这里的"可用"只保证能存取，不保证强机密性——见上面 obfuscate 的说明。
bool isAvailable() { return true; }

#else

// --- Linux: libsecret（dlopen，非 link-time）---
//
// 对应Python: keyring 的 SecretService 后端 —— 经 D-Bus Secret Service API 与
// GNOME Keyring / KWallet 通信。这里直接用 libsecret 的同步便捷函数
// secret_password_store_sync / lookup_sync / clear_sync，schema 用
// SECRET_SCHEMA_NOTE（任意键值对），属性 {service, account}，与 Python 侧
// keyring 写入的条目属性一致、可互读。
//
// 为什么必须 dlopen 而非 link-time 链接：
//   linux-x86_64 在 rockylinux:8 容器里构建、以 portable tarball 分发。
//   link-time 依赖 libsecret-1.so.0 会让没装 libsecret 的机器**根本起不来**
//   ——为了保护密码把应用变成打不开，是净损失。dlopen 让库缺失时仅降级为
//   「钥匙串不可用 → 保持明文 + 0600」，应用照常可用。
//
// libsecret 本身是 GLib 的 C 库，我们用 dlsym 取出所需函数与符号，避免
// 把 glib/libsecret 的头文件依赖引进构建。

#include <dlfcn.h>

namespace {

// glib 基础类型的最小定义（不引 glib 头，避免构建期依赖）。
// gboolean/gchar 在 ABI 上分别是 int / char。
typedef int   gboolean;
typedef char  gchar;

// SecretSchema 的真实布局（逐字段对齐 libsecret 公开头 secret-schema.h，稳定 ABI）。
// 不能传 nullptr：secret_password_*_sync 会解引用 schema->name 做条目标签名。
// 也不能只定义前几个字段——libsecret 按完整 sizeof(SecretSchema)（含尾部 7 个
// reserved 字段）读取，短结构体会被越界读。这里完整复刻。
// name 用 org.freedesktop.Secret.Generic —— 这正是 Python keyring 的
// SecretService 后端写入的 schema 名，两边条目可互读。
typedef struct {
    const char *name;
    int type;                  // SecretSchemaAttributeType；STRING = 0
} SecretSchemaAttribute;

typedef struct {
    const char *name;
    int flags;                 // SecretSchemaFlags；NONE = 0
    SecretSchemaAttribute attributes[32];
    // <private> 保留字段：占位以对齐 sizeof，libsecret 运行期不使用。
    int   reserved;
    void *reserved1;
    void *reserved2;
    void *reserved3;
    void *reserved4;
    void *reserved5;
    void *reserved6;
    void *reserved7;
} SecretSchema;

static const SecretSchema kCubeSchema = {
    "org.freedesktop.Secret.Generic",
    0,   // SECRET_SCHEMA_NONE
    {{"service", 0 /*STRING*/}, {"account", 0 /*STRING*/}},   // 其余零初始化
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

// libsecret 同步 API 的最小签名子集（C ABI，变参属性表，NULL 结尾）。
typedef gboolean (*secret_password_store_sync_fn)(
    const SecretSchema *schema, const char *collection, const char *label,
    const char *password, void *cancellable, void **error,
    const char *attr1_name, const char *attr1_val,
    const char *attr2_name, const char *attr2_val,
    const char *sentinel);
typedef gchar *(*secret_password_lookup_sync_fn)(
    const SecretSchema *schema, void *cancellable, void **error,
    const char *attr1_name, const char *attr1_val,
    const char *attr2_name, const char *attr2_val,
    const char *sentinel);
typedef gboolean (*secret_password_clear_sync_fn)(
    const SecretSchema *schema, void *cancellable, void **error,
    const char *attr1_name, const char *attr1_val,
    const char *attr2_name, const char *attr2_val,
    const char *sentinel);
typedef void (*secret_password_free_fn)(gchar *password);
typedef void (*g_error_free_fn)(void *error);

// 运行期解析出的函数表；全部非空才算可用。
struct LibsecretApi {
    void *handle = nullptr;
    secret_password_store_sync_fn  store = nullptr;
    secret_password_lookup_sync_fn lookup = nullptr;
    secret_password_clear_sync_fn  clear = nullptr;
    secret_password_free_fn        passwordFree = nullptr;
    g_error_free_fn                errorFree = nullptr;
    bool ok = false;
};

// 惰性 dlopen + dlsym，结果缓存（进程内只解析一次）。
const LibsecretApi &libsecret()
{
    static const LibsecretApi api = [] {
        LibsecretApi a;
        // RTLD_LOCAL：不把 libsecret 的符号泄进全局命名空间，避免与 Qt/其它库冲突。
        a.handle = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!a.handle)
            return a;   // 没装 libsecret → ok=false，上层降级
        a.store        = reinterpret_cast<secret_password_store_sync_fn>(
            dlsym(a.handle, "secret_password_store_sync"));
        a.lookup       = reinterpret_cast<secret_password_lookup_sync_fn>(
            dlsym(a.handle, "secret_password_lookup_sync"));
        a.clear        = reinterpret_cast<secret_password_clear_sync_fn>(
            dlsym(a.handle, "secret_password_clear_sync"));
        a.passwordFree = reinterpret_cast<secret_password_free_fn>(
            dlsym(a.handle, "secret_password_free"));
        // g_error_free 在 libglib-2.0，libsecret 传递依赖已加载，可从全局取。
        a.errorFree    = reinterpret_cast<g_error_free_fn>(
            dlsym(RTLD_DEFAULT, "g_error_free"));
        a.ok = a.store && a.lookup && a.clear && a.passwordFree && a.errorFree;
        return a;
    }();
    return api;
}

// 从 GError 提取可读信息并释放。GError 布局：{GQuark domain; gint code; gchar *message;}
// message 是第三个字段。为免依赖 glib 头，按 ABI 直接取偏移。
QString gerrorMessage(void *error)
{
    if (!error)
        return QStringLiteral("未知 libsecret 错误");
    struct GErrorLayout { unsigned int domain; int code; char *message; };
    const auto *e = reinterpret_cast<const GErrorLayout *>(error);
    return e->message ? QString::fromUtf8(e->message)
                      : QStringLiteral("libsecret 错误码 %1").arg(e->code);
}

} // namespace

bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    const LibsecretApi &api = libsecret();
    if (!api.ok) {
        if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret 不可用");
        return false;
    }
    void *error = nullptr;
    const QByteArray svc = service.toUtf8();
    const QByteArray acc = account.toUtf8();
    const QByteArray pwd = secret.toUtf8();
    // label 给人看（钥匙串管理器里显示），属性是 service/account 供检索。
    const QByteArray label = (service + QLatin1Char('/') + account).toUtf8();
    const gboolean okFlag = api.store(
        &kCubeSchema, /*collection(默认 login)*/ nullptr, label.constData(),
        pwd.constData(), nullptr, &error,
        "service", svc.constData(), "account", acc.constData(), nullptr);
    if (!okFlag) {
        if (errorOut) *errorOut = gerrorMessage(error);
        if (error && api.errorFree) api.errorFree(error);
        return false;
    }
    return true;
}

QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    const LibsecretApi &api = libsecret();
    if (!api.ok) {
        if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret 不可用");
        return QString();
    }
    void *error = nullptr;
    const QByteArray svc = service.toUtf8();
    const QByteArray acc = account.toUtf8();
    gchar *raw = api.lookup(&kCubeSchema, nullptr, &error,
                            "service", svc.constData(),
                            "account", acc.constData(), nullptr);
    if (error) {
        if (errorOut) *errorOut = gerrorMessage(error);
        if (api.errorFree) api.errorFree(error);
        return QString();
    }
    if (!raw)
        return QString();   // 未存过：与 macOS「找不到返回空且不报错」一致
    const QString out = QString::fromUtf8(raw);
    api.passwordFree(raw);
    return out;
}

bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    const LibsecretApi &api = libsecret();
    if (!api.ok) {
        if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret 不可用");
        return false;
    }
    void *error = nullptr;
    const QByteArray svc = service.toUtf8();
    const QByteArray acc = account.toUtf8();
    const gboolean okFlag = api.clear(&kCubeSchema, nullptr, &error,
                                      "service", svc.constData(),
                                      "account", acc.constData(), nullptr);
    if (!okFlag) {
        if (errorOut) *errorOut = gerrorMessage(error);
        if (error && api.errorFree) api.errorFree(error);
        return false;
    }
    return true;
}

// 运行期探测 libsecret-1.so.0 能否 dlopen 且所需符号齐全。
// 库缺失（无 GNOME Keyring/KWallet 的最小系统）→ false，上层保持明文 + 0600。
bool isAvailable() { return libsecret().ok; }

#endif

// 对应Python: core/ai/secrets.py::_PROVIDER_ENV_VARS
static QStringList providerEnvVars(const QString &provider)
{
    static const QHash<QString, QStringList> map = {
        {QStringLiteral("zhipuai"),  {QStringLiteral("ZAI_API_KEY"), QStringLiteral("ZHIPUAI_API_KEY")}},
        {QStringLiteral("deepseek"), {QStringLiteral("DEEPSEEK_API_KEY")}},
        {QStringLiteral("aliyun"),   {QStringLiteral("DASHSCOPE_API_KEY")}},
        {QStringLiteral("moonshot"), {QStringLiteral("MOONSHOT_API_KEY")}},
        {QStringLiteral("spark"),    {QStringLiteral("SPARK_API_KEY")}},
        {QStringLiteral("baichuan"), {QStringLiteral("BAICHUAN_API_KEY")}},
        {QStringLiteral("minimax"),  {QStringLiteral("MINIMAX_API_KEY")}},
        {QStringLiteral("xiaomi"),   {QStringLiteral("XIAOMI_API_KEY")}},
        {QStringLiteral("doubao"),   {QStringLiteral("DOUBAO_API_KEY"), QStringLiteral("ARK_API_KEY")}},
    };
    return map.value(provider);
}

// 对应Python: core/ai/secrets.py::get_ai_api_key
QString aiApiKey(const QString &provider)
{
    // 1) provider-specific environment variables
    for (const QString &envVar : providerEnvVars(provider)) {
        const QString val = qEnvironmentVariable(envVar.toUtf8().constData());
        if (!val.isEmpty())
            return val;
    }

    // 2) generic fallback env var
    const QString generic = qEnvironmentVariable("AI_API_KEY");
    if (!generic.isEmpty())
        return generic;

    const QString service = QLatin1String(vars::APP_NAME);

    // 3) keychain, new-format account name
    const QString newKey = retrieveSecret(service,
                                          QStringLiteral("ai_api_key_%1").arg(provider));
    if (!newKey.isEmpty())
        return newKey;

    // 4) legacy account name, zhipuai only
    if (provider == QLatin1String("zhipuai")) {
        const QString oldKey = retrieveSecret(service, QStringLiteral("zai_api_key"));
        if (!oldKey.isEmpty())
            return oldKey;
    }
    return QString();
}

// 对应Python: core/ai/secrets.py::set_ai_api_key
bool setAiApiKey(const QString &apiKey, const QString &provider)
{
    return storeSecret(QLatin1String(vars::APP_NAME),
                       QStringLiteral("ai_api_key_%1").arg(provider), apiKey);
}

} // namespace Secrets

} // namespace cubeshell
