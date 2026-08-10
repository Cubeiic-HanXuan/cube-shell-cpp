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
//   Windows DPAPI (CryptProtectData) + 密文文件（dataDir()/secrets/），链 Crypt32。
//   Linux   libsecret 经 dlopen 运行期加载（不 link-time，见 Secrets.cpp 注释）。
//   OHOS    沙箱内混淆文件（QSettings IniFormat；一期，二期接 HUKS）。

#include <QString>

namespace cubeshell {

namespace Secrets {

// 本平台的后端是否真的能存取机密。
//
// 存在的理由：Windows/Linux 后端曾经（现在仍可能在某些环境下）是空壳，
// 调用 storeSecret 只会 return false。把设备密码从明文 JSON 迁进钥匙串之前
// 必须先问这个问题——后端不可用却照迁不误，等于把 21 条密码删干净。
// 返回 false 时调用方应当**保持明文、不迁移**，而不是迁一半。
bool isAvailable();

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
