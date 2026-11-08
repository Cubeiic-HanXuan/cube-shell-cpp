#pragma once

// Test-only link seams. These targets compile the real preferences/dialog/voice
// sources without cube_core: no real configuration directory or OS keychain is used.
#include "config/GlobalState.h"
#include "config/Secrets.h"
#include <QHash>

namespace aiprefstest {
QString configRoot;
QHash<QString, QString> secrets;
bool failSecretWrites = false;
}

namespace cubeshell {
QString GlobalState::configFilePath(const QString &name)
{
    Q_ASSERT(!aiprefstest::configRoot.isEmpty());
    return aiprefstest::configRoot + QLatin1Char('/') + name;
}

namespace Secrets {
QString retrieveSecret(const QString &service, const QString &account, QString *)
{
    return aiprefstest::secrets.value(service + QLatin1Char('/') + account);
}
bool storeSecret(const QString &service, const QString &account, const QString &key, QString *)
{
    if (aiprefstest::failSecretWrites)
        return false;
    aiprefstest::secrets.insert(service + QLatin1Char('/') + account, key);
    return true;
}
QString aiApiKey(const QString &provider)
{
    const auto generic = qEnvironmentVariable("AI_API_KEY");
    return generic.isEmpty()
        ? retrieveSecret(QLatin1String(vars::APP_NAME), QStringLiteral("ai_api_key_%1").arg(provider))
        : generic;
}
bool setAiApiKey(const QString &key, const QString &provider)
{
    return storeSecret(QLatin1String(vars::APP_NAME), QStringLiteral("ai_api_key_%1").arg(provider), key);
}
} // namespace Secrets
} // namespace cubeshell
