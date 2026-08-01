// Secrets.cpp — platform keychain abstraction. See Secrets.h.

#include "Secrets.h"

#include <QByteArray>
#include <QHash>
#include <QStringList>

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

#elif defined(Q_OS_WIN)

// --- Windows: DPAPI (CryptProtectData) + file storage ---
// TODO(win32): encrypt with CryptProtectData, persist the blob under
// GlobalState::dataDir()/secrets/<service>_<account>.bin, decrypt with
// CryptUnprotectData on retrieval. Matches keyring's Windows behaviour of
// per-user protection without prompting.

bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account); Q_UNUSED(secret);
    if (errorOut) *errorOut = QStringLiteral("Secrets: Windows DPAPI backend not implemented yet");
    return false;
}

QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account);
    if (errorOut) *errorOut = QStringLiteral("Secrets: Windows DPAPI backend not implemented yet");
    return QString();
}

bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account);
    if (errorOut) *errorOut = QStringLiteral("Secrets: Windows DPAPI backend not implemented yet");
    return false;
}

#else

// --- Linux: libsecret ---
// TODO(linux): use libsecret's secret_password_store_sync /
// secret_password_lookup_sync with a schema of {service, account}, matching
// the SecretService keyring backend used by the Python side.

bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account); Q_UNUSED(secret);
    if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret backend not implemented yet");
    return false;
}

QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account);
    if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret backend not implemented yet");
    return QString();
}

bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut)
{
    Q_UNUSED(service); Q_UNUSED(account);
    if (errorOut) *errorOut = QStringLiteral("Secrets: libsecret backend not implemented yet");
    return false;
}

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
