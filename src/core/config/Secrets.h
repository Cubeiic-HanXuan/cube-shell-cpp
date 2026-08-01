#pragma once

// Secrets.h — platform keychain abstraction (service + account -> secret).
//
// 对应Python: core/ai/secrets.py (keyring 用法) — the Python side stores
// secrets via the `keyring` package, which on macOS writes generic passwords
// into the login Keychain with service/account attributes. The macOS backend
// here uses the same item class and attributes, so keys written by the Python
// app are readable from C++ and vice versa.
//
// Backends:
//   macOS   Security.framework (SecItemAdd/SecItemCopyMatching/SecItemDelete)
//           — fully implemented.
//   Windows DPAPI (CryptProtectData) + file storage — TODO(win32) stub.
//   Linux   libsecret — TODO(linux) stub.

#include <QString>

namespace cubeshell {

namespace Secrets {

// 对应Python: keyring.set_password(service, account, secret)
// Creates the item or updates it in place when it already exists.
bool storeSecret(const QString &service, const QString &account,
                 const QString &secret, QString *errorOut = nullptr);

// 对应Python: keyring.get_password(service, account)
// Returns an empty string when the item does not exist (Python returns None).
QString retrieveSecret(const QString &service, const QString &account,
                       QString *errorOut = nullptr);

// 对应Python: keyring.delete_password(service, account)
// Returns false when the item does not exist or deletion fails.
bool deleteSecret(const QString &service, const QString &account,
                  QString *errorOut = nullptr);

// 对应Python: core/ai/secrets.py::get_ai_api_key
// Priority: provider-specific env vars -> generic AI_API_KEY env var ->
// keychain "ai_api_key_{provider}" -> legacy "zai_api_key" (zhipuai only).
QString aiApiKey(const QString &provider = QStringLiteral("zhipuai"));

// 对应Python: core/ai/secrets.py::set_ai_api_key
// Stores under service "cube-shell", account "ai_api_key_{provider}".
bool setAiApiKey(const QString &apiKey,
                 const QString &provider = QStringLiteral("zhipuai"));

} // namespace Secrets

} // namespace cubeshell
