// AiPreferences.cpp — see AiPreferences.h.
//
// 对应Python: core/ai/prefs.py + core/ai/secrets.py（API Key 部分转发到
// config/Secrets.h 的已有实现）。

#include "AiPreferences.h"

#include "config/GlobalState.h"
#include "config/Secrets.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cubeshell {

namespace {

QString g_providersOverridePath;              // setProvidersConfigPath()
bool g_presetsLoaded = false;
QList<ProviderPreset> g_presets;              // 对应Python: PROVIDER_PRESETS

// Fallback used when llm_providers.json is missing/broken.
// 对应Python: prefs.py::_load_providers 的 except 分支
ProviderPreset customFallbackPreset()
{
    ProviderPreset p;
    p.key = QStringLiteral("custom");
    p.name = QStringLiteral("自定义");
    p.supportsThinking = false;
    return p;
}

// Locate conf/llm_providers.json.
// 对应Python: prefs.py::_load_providers 里相对源码根目录的 ../../conf 定位；
// C++ 侧应用可能被打包，这里按候选路径依次探测。
QString locateProvidersConfig()
{
    if (!g_providersOverridePath.isEmpty())
        return g_providersOverridePath;

    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        candidates << appDir + QStringLiteral("/conf/llm_providers.json")
                   << appDir + QStringLiteral("/../conf/llm_providers.json")
                   << appDir + QStringLiteral("/../Resources/conf/llm_providers.json")
                   << appDir + QStringLiteral("/../../conf/llm_providers.json");
    }
    candidates << QDir::currentPath() + QStringLiteral("/conf/llm_providers.json");
#ifdef CUBESHELL_SOURCE_CONF_DIR
    // Dev tree: cpp/build/... -> repo root conf/ (source layout, CMake 注入).
    candidates << QStringLiteral(CUBESHELL_SOURCE_CONF_DIR "/llm_providers.json");
#endif

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return QFileInfo(path).absoluteFilePath();
    }
    return QString();
}

void ensurePresetsLoaded()
{
    if (g_presetsLoaded)
        return;
    g_presetsLoaded = true;
    g_presets.clear();

    const QString path = locateProvidersConfig();
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        g_presets.append(customFallbackPreset());
        return;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        g_presets.append(customFallbackPreset());
        return;
    }

    // 对应Python: {p["key"]: {...} for p in providers_list}
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        ProviderPreset p;
        p.key = obj.value(QStringLiteral("key")).toString();
        p.name = obj.value(QStringLiteral("name")).toString();
        p.baseUrl = obj.value(QStringLiteral("base_url")).toString();
        p.anthropicBaseUrl =
            obj.value(QStringLiteral("anthropic_base_url")).toString();
        p.supportsThinking =
            obj.value(QStringLiteral("supports_thinking")).toBool(false);
        const QJsonArray models = obj.value(QStringLiteral("models")).toArray();
        for (const QJsonValue &m : models)
            p.models << m.toString();
        if (!p.key.isEmpty())
            g_presets.append(p);
    }
    if (g_presets.isEmpty())
        g_presets.append(customFallbackPreset());
}

} // namespace

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

// 对应Python: prefs.py::get_ai_prefs_path
QString AiPreferences::prefsFilePath()
{
    return GlobalState::configFilePath(QStringLiteral("ai.json"));
}

// 对应Python: prefs.py::load_ai_prefs
AiPreferences AiPreferences::load()
{
    AiPreferences prefs;                      // defaults == Python dataclass
    QFile file(prefsFilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return prefs;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return prefs;
    const QJsonObject data = doc.object();

    // Python 用 `data.get("provider") or "zhipuai"` — 空串也回退默认值。
    const auto strOr = [&data](const char *key, const QString &fallback) {
        const QString v = data.value(QLatin1String(key)).toString();
        return v.isEmpty() ? fallback : v;
    };
    prefs.provider = strOr("provider", prefs.provider);
    prefs.model = strOr("model", prefs.model);
    prefs.baseUrl = data.value(QStringLiteral("base_url")).toString();
    prefs.thinkingEnabled =
        data.value(QStringLiteral("thinking_enabled")).toBool(true);
    prefs.stream = data.value(QStringLiteral("stream")).toBool(true);
    prefs.maxTokens = data.value(QStringLiteral("max_tokens")).toInt(8192);
    prefs.temperature = data.value(QStringLiteral("temperature")).toDouble(1.0);
    prefs.systemPrompt = strOr("system_prompt", prefs.systemPrompt);
    return prefs;
}

// 对应Python: prefs.py::save_ai_prefs
bool AiPreferences::save(QString *errorOut) const
{
    QJsonObject data;
    data.insert(QStringLiteral("provider"), provider);
    data.insert(QStringLiteral("model"), model);
    data.insert(QStringLiteral("base_url"), baseUrl);
    data.insert(QStringLiteral("thinking_enabled"), thinkingEnabled);
    data.insert(QStringLiteral("stream"), stream);
    data.insert(QStringLiteral("max_tokens"), maxTokens);
    data.insert(QStringLiteral("temperature"), temperature);
    data.insert(QStringLiteral("system_prompt"), systemPrompt);

    QSaveFile file(prefsFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    file.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// provider presets
// ---------------------------------------------------------------------------

QList<ProviderPreset> AiPreferences::providerPresets()
{
    ensurePresetsLoaded();
    return g_presets;
}

// 对应Python: prefs.py::get_provider_preset（未知 key 回退 "custom"）
ProviderPreset AiPreferences::providerPreset(const QString &key)
{
    ensurePresetsLoaded();
    for (const ProviderPreset &p : g_presets) {
        if (p.key == key)
            return p;
    }
    for (const ProviderPreset &p : g_presets) {
        if (p.key == QLatin1String("custom"))
            return p;
    }
    return customFallbackPreset();
}

void AiPreferences::setProvidersConfigPath(const QString &path)
{
    g_providersOverridePath = path;
    g_presetsLoaded = false;                  // force reload on next access
}

// ---------------------------------------------------------------------------
// derived helpers
// ---------------------------------------------------------------------------

// 对应Python: worker.py::run 的 `self.prefs.base_url or preset["base_url"]`
QString AiPreferences::effectiveBaseUrl() const
{
    if (!baseUrl.trimmed().isEmpty())
        return baseUrl.trimmed();
    return providerPreset(provider).baseUrl;
}

QString AiPreferences::chatCompletionsUrl() const
{
    QString base = effectiveBaseUrl();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + QStringLiteral("/chat/completions");
}

// ---------------------------------------------------------------------------
// API key
// ---------------------------------------------------------------------------

// 对应Python: secrets.py::get_ai_api_key
QString AiPreferences::apiKey() const
{
    return Secrets::aiApiKey(provider);
}

// 对应Python: secrets.py::set_ai_api_key
bool AiPreferences::setApiKey(const QString &key) const
{
    return Secrets::setAiApiKey(key, provider);
}

} // namespace cubeshell
