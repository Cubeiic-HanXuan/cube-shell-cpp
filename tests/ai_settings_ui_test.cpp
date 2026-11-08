#include "ai_preferences_test_support.h"
#include "ai/AiPreferences.h"
#include "dialogs/AiSettingsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>

using namespace cubeshell;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { qWarning() << "FAIL" << #x << "line" << __LINE__; ++failures; } } while (0)

template<class T> static T *field(AiSettingsDialog &dialog, const char *name)
{
    auto *result = dialog.findChild<T *>(QLatin1String(name));
    Q_ASSERT(result);
    return result;
}

static QByteArray savedJson()
{
    QFile file(AiPreferences::prefsFilePath());
    CHECK(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

static void saveDialog(AiSettingsDialog &dialog)
{
    CHECK(QMetaObject::invokeMethod(&dialog, "save", Qt::DirectConnection));
}

static void testPreferences()
{
    const auto defaults = AiPreferences::load();
    CHECK(defaults.voiceModel == "glm-asr-2512");
    CHECK(defaults.voiceTranscriptionUrl == "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions");
    CHECK(defaults.voiceConfigurationError().isEmpty());

    // A pre-feature ai.json keeps the chat configuration and receives ASR defaults.
    QFile old(AiPreferences::prefsFilePath());
    CHECK(old.open(QIODevice::WriteOnly));
    old.write(R"({"provider":"deepseek","model":"chat-test","base_url":"https://chat.example/v1"})");
    old.close();
    auto prefs = AiPreferences::load();
    CHECK(prefs.provider == "deepseek" && prefs.model == "chat-test");
    CHECK(prefs.voiceModel == defaults.voiceModel);
    CHECK(prefs.voiceTranscriptionUrl == defaults.voiceTranscriptionUrl);
    CHECK(prefs.setApiKey(QStringLiteral("chat-secret")));
    qputenv("AI_API_KEY", "generic-chat-secret");
    qputenv("ZAI_API_KEY", "zhipu-chat-secret");
    CHECK(prefs.voiceApiKey().isEmpty());
    CHECK(prefs.setVoiceApiKey(QStringLiteral("voice-secret")));
    CHECK(prefs.voiceApiKey() == "voice-secret");
    qputenv("CUBESHELL_VOICE_API_KEY", "  voice-env-secret  ");
    CHECK(prefs.voiceApiKey() == "voice-env-secret");
    qunsetenv("CUBESHELL_VOICE_API_KEY");
    qunsetenv("AI_API_KEY");
    qunsetenv("ZAI_API_KEY");
    CHECK(prefs.apiKey() == "chat-secret");

    prefs.voiceModel = QStringLiteral("custom-asr");
    prefs.voiceTranscriptionUrl = QStringLiteral("https://voice.example/v1/audio/transcriptions");
    CHECK(prefs.save());
    const auto loaded = AiPreferences::load();
    CHECK(loaded.voiceModel == prefs.voiceModel && loaded.voiceTranscriptionUrl == prefs.voiceTranscriptionUrl);
    CHECK(loaded.provider == prefs.provider && loaded.model == prefs.model);
    CHECK(!savedJson().contains("secret") && !savedJson().contains("api_key"));

    for (const auto &url : {"", "not-a-url", "file:///tmp/audio", "https://", "https://user:pass@voice.example/asr", "https://voice.example/asr#fragment"}) {
        prefs.voiceTranscriptionUrl = QLatin1String(url);
        CHECK(!prefs.voiceConfigurationError().isEmpty());
    }
    prefs.voiceTranscriptionUrl = defaults.voiceTranscriptionUrl;
    prefs.voiceModel = QStringLiteral("  ");
    CHECK(!prefs.voiceConfigurationError().isEmpty());
}

static void testVoiceOnlySave()
{
    const auto before = AiPreferences::load();
    AiSettingsDialog dialog;
    field<QTabWidget>(dialog, "aiSettingsTabs")->setCurrentIndex(1);
    saveDialog(dialog);
    CHECK(dialog.result() == QDialog::Accepted);
    const auto after = AiPreferences::load();
    CHECK(after.provider == before.provider && after.model == before.model);
    CHECK(after.baseUrl == before.baseUrl); // In particular, do not restore the provider preset.
    CHECK(after.voiceModel == before.voiceModel && after.voiceTranscriptionUrl == before.voiceTranscriptionUrl);
    CHECK(after.apiKey() == "chat-secret" && after.voiceApiKey() == "voice-secret");
}

static void testSettingsForm()
{
    AiSettingsDialog dialog;
    auto *tabs = field<QTabWidget>(dialog, "aiSettingsTabs");
    CHECK(tabs->count() == 2 && tabs->tabText(1) == QStringLiteral("语音识别"));
    auto *url = field<QLineEdit>(dialog, "voiceTranscriptionUrlEdit");
    auto *model = field<QLineEdit>(dialog, "voiceModelEdit");
    auto *key = field<QLineEdit>(dialog, "voiceApiKeyEdit");
    CHECK(url->text() == "https://voice.example/v1/audio/transcriptions");
    CHECK(model->text() == "custom-asr");
    CHECK(key->text().isEmpty() && key->echoMode() == QLineEdit::Password);
    CHECK(key->placeholderText().contains(QStringLiteral("已保存")));
    tabs->setCurrentIndex(1);
    dialog.show();
    QApplication::processEvents();
    CHECK(url->isVisible() && model->isVisible() && key->isVisible());

    // Switching the chat provider must not clear or overwrite voice settings.
    url->setText(QStringLiteral("  https://new-voice.example/transcribe  "));
    model->setText(QStringLiteral("  replacement-asr  "));
    key->setText(QStringLiteral("  replacement-voice-secret  "));
    auto *provider = field<QComboBox>(dialog, "chatProviderCombo");
    provider->setCurrentIndex(provider->findData(QStringLiteral("custom")));
    CHECK(key->text().contains("replacement-voice-secret"));
    field<QLineEdit>(dialog, "chatApiKeyEdit")->setText(QStringLiteral("replacement-chat-secret"));
    saveDialog(dialog);
    CHECK(dialog.result() == QDialog::Accepted);
    const auto prefs = AiPreferences::load();
    CHECK(prefs.provider == "custom");
    CHECK(prefs.voiceTranscriptionUrl == "https://new-voice.example/transcribe");
    CHECK(prefs.voiceModel == "replacement-asr");
    CHECK(prefs.voiceApiKey() == "replacement-voice-secret");
    CHECK(prefs.apiKey() == "replacement-chat-secret");
    CHECK(!savedJson().contains("secret") && !savedJson().contains("api_key"));
    CHECK(key->text().isEmpty());

    AiSettingsDialog reopened;
    CHECK(field<QLineEdit>(reopened, "voiceModelEdit")->text() == prefs.voiceModel);
    CHECK(field<QLineEdit>(reopened, "voiceApiKeyEdit")->text().isEmpty());
    saveDialog(reopened); // Blank input preserves both stored keys.
    CHECK(reopened.result() == QDialog::Accepted);
    CHECK(AiPreferences::load().voiceApiKey() == "replacement-voice-secret");
    CHECK(AiPreferences::load().apiKey() == "replacement-chat-secret");

    const auto before = savedJson();
    AiSettingsDialog cancelled;
    field<QLineEdit>(cancelled, "voiceModelEdit")->setText(QStringLiteral("must-not-save"));
    field<QLineEdit>(cancelled, "voiceApiKeyEdit")->setText(QStringLiteral("must-not-save"));
    cancelled.reject();
    CHECK(savedJson() == before);
    CHECK(AiPreferences::load().voiceApiKey() == "replacement-voice-secret");

    qputenv("CUBESHELL_VOICE_API_KEY", "environment-voice-secret");
    AiSettingsDialog fromEnv;
    auto *envKey = field<QLineEdit>(fromEnv, "voiceApiKeyEdit");
    CHECK(!envKey->isEnabled() && envKey->text().isEmpty());
    CHECK(envKey->placeholderText().contains("CUBESHELL_VOICE_API_KEY"));
    qunsetenv("CUBESHELL_VOICE_API_KEY");
}

static void testRejectedSaves()
{
    for (int mode = 0; mode < 3; ++mode) {
        const auto before = savedJson();
        AiSettingsDialog dialog;
        if (mode == 0)
            field<QLineEdit>(dialog, "voiceTranscriptionUrlEdit")->setText(QStringLiteral("invalid"));
        else if (mode == 1)
            field<QLineEdit>(dialog, "voiceModelEdit")->clear();
        else {
            field<QLineEdit>(dialog, "voiceApiKeyEdit")->setText(QStringLiteral("failed-secret"));
            field<QLineEdit>(dialog, "voiceTranscriptionUrlEdit")->setText(QStringLiteral("https://unsaved.example/asr"));
            aiprefstest::failSecretWrites = true;
        }
        bool warningShown = false;
        QTimer::singleShot(0, &dialog, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                    warningShown = true;
                    CHECK(box->text().contains(QStringLiteral("语音")));
                    box->accept();
                }
            }
        });
        saveDialog(dialog);
        aiprefstest::failSecretWrites = false;
        CHECK(warningShown && dialog.result() != QDialog::Accepted);
        CHECK(savedJson() == before);
        CHECK(AiPreferences::load().voiceApiKey() == "replacement-voice-secret");
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("CUBESHELL_VOICE_API_KEY");
    QApplication app(argc, argv);
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    aiprefstest::configRoot = config.path();
    QFile providers(config.filePath(QStringLiteral("providers.json")));
    if (!providers.open(QIODevice::WriteOnly)) return 1;
    providers.write(R"([{"key":"deepseek","name":"Chat service","base_url":"https://preset.example/v1","models":["chat-test"]},{"key":"custom","name":"Custom","models":[]}])");
    providers.close();
    AiPreferences::setProvidersConfigPath(providers.fileName());
    testPreferences();
    testVoiceOnlySave();
    testSettingsForm();
    testRejectedSaves();
    qInfo() << "AI settings failures:" << failures;
    return failures ? 1 : 0;
}
