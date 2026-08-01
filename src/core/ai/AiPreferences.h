#pragma once

// AiPreferences.h — AI user preferences + provider presets + API key access.
//
// 对应Python: core/ai/prefs.py (AIUserPrefs / load_ai_prefs / save_ai_prefs /
//             PROVIDER_PRESETS / get_provider_preset)
//           + core/ai/secrets.py (get_ai_api_key / set_ai_api_key — 通过
//             config/Secrets.h 已有实现转发，env 变量优先级链在其中)
//
// Persistence: <configDir>/ai.json — same file and keys as the Python side,
// so both versions share user settings. API keys are NEVER written to
// ai.json (keychain / env vars only), matching the Python design.

#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {

// One entry of conf/llm_providers.json.
// 对应Python: prefs.py::PROVIDER_PRESETS 的元素
struct ProviderPreset {
    QString key;                 // "zhipuai", "deepseek", ...
    QString name;                // 显示名（如 "智谱 GLM"）
    QString baseUrl;             // OpenAI 兼容 base_url
    QString anthropicBaseUrl;    // Anthropic 兼容 base_url（Claude Code 用）
    QStringList models;
    bool supportsThinking = false;
};

// 对应Python: prefs.py::AIUserPrefs (dataclass)
class AiPreferences {
public:
    // Defaults mirror the Python dataclass defaults exactly.
    QString provider = QStringLiteral("zhipuai");
    QString model = QStringLiteral("glm-4.7");
    QString baseUrl;                       // 为空时用 preset 的 base_url
    bool thinkingEnabled = true;
    bool stream = true;
    int maxTokens = 8192;
    double temperature = 1.0;
    QString systemPrompt = QStringLiteral(
        "你是一个资深 Linux 运维与终端助手。输出尽量可执行、可复制。");

    // --- persistence (ai.json, key 名与 Python 完全一致) ---

    // 对应Python: prefs.py::load_ai_prefs（失败/缺文件返回默认值）
    static AiPreferences load();
    // 对应Python: prefs.py::save_ai_prefs（indent=2, UTF-8, 不含 API Key）
    bool save(QString *errorOut = nullptr) const;
    // 对应Python: prefs.py::get_ai_prefs_path
    static QString prefsFilePath();

    // --- provider presets (conf/llm_providers.json) ---

    // 对应Python: prefs.py::_load_providers / PROVIDER_PRESETS
    static QList<ProviderPreset> providerPresets();
    // 对应Python: prefs.py::get_provider_preset（未知 key 回退 "custom"）
    static ProviderPreset providerPreset(const QString &key);
    // Override the providers file location (tests / non-standard installs).
    static void setProvidersConfigPath(const QString &path);

    // --- derived helpers ---

    // baseUrl 为空时回退到 preset 的 base_url。
    // 对应Python: worker.py::run 里的 `self.prefs.base_url or preset["base_url"]`
    QString effectiveBaseUrl() const;

    // "<base>/chat/completions"，容忍 base 尾部斜杠。
    QString chatCompletionsUrl() const;

    // --- API key（钥匙串/环境变量，绝不落盘到 ai.json） ---

    // 对应Python: secrets.py::get_ai_api_key（env 链 -> keychain）
    QString apiKey() const;
    // 对应Python: secrets.py::set_ai_api_key
    bool setApiKey(const QString &key) const;
};

} // namespace cubeshell
