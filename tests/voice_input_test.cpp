#include "ai_test_support.h"
#include "ai_preferences_test_support.h"
#include "ai/VoiceInput.h"

#include <QBuffer>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

using namespace cubeshell;
using namespace aitest;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { qWarning() << "FAIL" << #x << "line" << __LINE__; ++failures; } } while (0)

// All audio and permission responses are synthetic. No microphone or external ASR is used.
class FakeVoiceInput : public VoiceInput {
public:
    QList<std::function<void(bool)>> permissions;
    QByteArray pcm = QByteArray(2048, '\0');
    QString captureError;
    int captureStarts = 0, captureStops = 0;
    QBuffer audio;

    void answerPermission(bool granted)
    {
        CHECK(!permissions.isEmpty());
        if (!permissions.isEmpty()) permissions.takeFirst()(granted);
    }
protected:
    void requestMicrophonePermission(std::function<void(bool)> completion) override
    {
        permissions.append(std::move(completion));
    }
    QIODevice *startAudioCapture(QString &error) override
    {
        ++captureStarts;
        if (!captureError.isEmpty()) { error = captureError; return nullptr; }
        audio.close(); audio.setData(pcm); audio.open(QIODevice::ReadOnly);
        return &audio;
    }
    void stopAudioCapture() override
    {
        ++captureStops;
        audio.close();
    }
};

static void configure(FakeVoiceInput &voice, const Server &server)
{
    voice.setApiKey(QStringLiteral("synthetic-test-key"));
    voice.setTranscriptionUrl(QStringLiteral("http://127.0.0.1:%1/audio/transcriptions").arg(server.serverPort()));
    auto *network = voice.findChild<QNetworkAccessManager *>();
    CHECK(network);
    if (network) QObject::connect(network, &QNetworkAccessManager::finished, &voice, [](QNetworkReply *reply) {
        const auto http2 = reply->request().attribute(QNetworkRequest::Http2AllowedAttribute);
        CHECK(http2.isValid() && !http2.toBool());
        CHECK(reply->request().rawHeader("Authorization") == "Bearer synthetic-test-key");
        CHECK(reply->request().header(QNetworkRequest::ContentTypeHeader).toString().startsWith("multipart/form-data"));
    });
}

static void recordAndStop(FakeVoiceInput &voice)
{
    voice.startRecording();
    CHECK(voice.state() == VoiceInput::State::RequestingPermission);
    voice.answerPermission(true);
    CHECK(voice.isRecording());
    voice.stopRecording();
    CHECK(!voice.isRecording());
}

static void testWavAndSilence()
{
    const QByteArray pcm(2048, '\x12');
    const auto wav = VoiceInput::buildWavFile(pcm);
    CHECK(wav.size() == pcm.size() + 44);
    CHECK(wav.left(4) == "RIFF" && wav.mid(8, 4) == "WAVE");
    CHECK(qFromLittleEndian<quint32>(wav.constData() + 24) == 16000);
    CHECK(qFromLittleEndian<quint32>(wav.constData() + 40) == quint32(pcm.size()));
    CHECK(wav.mid(44) == pcm);
    CHECK(VoiceInput::isSilenceMarker(QStringLiteral(" 谢谢观看 ")));
    CHECK(!VoiceInput::isSilenceMarker(QStringLiteral("列出文件")));
}

static void testPermissionAndCaptureFailures()
{
    FakeVoiceInput voice; voice.pcm.clear();
    voice.setApiKey(QStringLiteral("synthetic-test-key"));
    QStringList errors; int starts = 0;
    QObject::connect(&voice, &VoiceInput::errorOccurred, &voice, [&](const QString &error) { errors.append(error); });
    QObject::connect(&voice, &VoiceInput::recordingStarted, &voice, [&] { ++starts; });
    voice.startRecording(); voice.startRecording(); voice.toggleRecording();
    CHECK(voice.permissions.size() == 1 && voice.captureStarts == 0);
    CHECK(!voice.isRecording());
    voice.answerPermission(false);
    CHECK(voice.state() == VoiceInput::State::Idle && errors.size() == 1);
    CHECK(errors.last().contains(QStringLiteral("麦克风权限")));
    CHECK(voice.captureStarts == 0);

    voice.captureError = QStringLiteral("未检测到麦克风设备");
    voice.startRecording(); voice.answerPermission(true);
    CHECK(voice.state() == VoiceInput::State::Idle && starts == 0);
    CHECK(errors.size() == 2 && voice.captureStops > 0);
    voice.captureError.clear();
    voice.startRecording(); voice.answerPermission(true);
    CHECK(voice.isRecording() && starts == 1);
    voice.startRecording(); CHECK(voice.permissions.isEmpty());
    voice.stopRecording(); CHECK(voice.state() == VoiceInput::State::Idle);

    voice.startRecording();
    auto stale = voice.permissions.takeFirst();
    voice.stopRecording();
    voice.startRecording();
    const int captures = voice.captureStarts;
    stale(true);
    CHECK(voice.state() == VoiceInput::State::RequestingPermission && voice.captureStarts == captures);
    voice.answerPermission(false);
    CHECK(voice.state() == VoiceInput::State::Idle);

    auto *pending = new FakeVoiceInput;
    pending->setApiKey(QStringLiteral("synthetic-test-key"));
    pending->startRecording();
    auto afterDestruction = pending->permissions.takeFirst();
    delete pending;
    afterDestruction(true); // The callback's QPointer must prevent a use-after-free.
}

static void testRecognitionResponses()
{
    struct Case { Response response; bool fails; QString text; };
    const QList<Case> cases = {
        {{R"({"text":"synthetic transcript"})", "application/json"}, false, "synthetic transcript"},
        {{R"({"error":"unauthorized"})", "application/json", 401}, true, {}},
        {{R"({"error":"server failure"})", "application/json", 500}, true, {}},
        {{R"({"text":"###"})", "application/json"}, false, {}},
    };
    for (const auto &test : cases) {
        Server server; CHECK(server.isListening());
        server.respond = [test](const auto &, int) { return test.response; };
        FakeVoiceInput voice; configure(voice, server);
        QStringList errors, finalText, partials;
        QObject::connect(&voice, &VoiceInput::errorOccurred, &voice, [&](const QString &s) { errors.append(s); });
        QObject::connect(&voice, &VoiceInput::textRecognized, &voice, [&](const QString &s) { finalText.append(s); });
        QObject::connect(&voice, &VoiceInput::partialTextRecognized, &voice, [&](const QString &s) { partials.append(s); });
        recordAndStop(voice);
        CHECK(voice.state() == VoiceInput::State::Processing);
        voice.toggleRecording(); voice.startRecording();
        CHECK(voice.permissions.isEmpty());
        CHECK(waitFor([&] { return voice.state() == VoiceInput::State::Idle; }));
        CHECK(server.requests.size() == 1 && voice.captureStops > 0);
        if (test.fails) {
            CHECK(errors.size() == 1 && finalText.isEmpty() && partials.isEmpty());
            if (test.response.status == 401) {
                CHECK(errors.value(0).contains(QStringLiteral("语音 API Key")));
                CHECK(errors.value(0).contains(QStringLiteral("AI 设置 → 语音识别")));
            }
        } else {
            CHECK(errors.isEmpty() && finalText.size() == 1 && finalText.value(0) == test.text);
            CHECK(test.text.isEmpty() ? partials.isEmpty() : partials.value(0) == test.text);
        }
        drain();
        CHECK(voice.findChildren<QNetworkReply *>().isEmpty());
    }
}

static void testDisconnectAndDestruction()
{
    Server server; CHECK(server.isListening());
    server.respond = [](const auto &, int) { return Response{"{", "application/json", 200, 0, true}; };
    FakeVoiceInput voice; configure(voice, server);
    int errors = 0, finals = 0;
    QObject::connect(&voice, &VoiceInput::errorOccurred, &voice, [&] { ++errors; });
    QObject::connect(&voice, &VoiceInput::textRecognized, &voice, [&] { ++finals; });
    recordAndStop(voice);
    CHECK(waitFor([&] { return !server.requests.isEmpty(); }));
    for (auto *socket : server.findChildren<QTcpSocket *>()) socket->abort();
    CHECK(waitFor([&] { return voice.state() == VoiceInput::State::Idle; }));
    CHECK(errors == 1 && finals == 0);

    auto *pending = new FakeVoiceInput; configure(*pending, server);
    recordAndStop(*pending);
    CHECK(waitFor([&] { return server.requests.size() == 2; }));
    QPointer<VoiceInput> guard(pending);
    QPointer<QNetworkReply> reply(pending->findChild<QNetworkReply *>());
    CHECK(reply);
    delete pending;
    drain();
    CHECK(!guard && !reply);
}

static void testFailureWhileRecordingAndRetry()
{
    Server server; CHECK(server.isListening());
    server.respond = [](const auto &, int index) {
        if (index == 1) return Response{R"({"error":"unauthorized"})", "application/json", 401};
        return Response{R"({"text":"retry succeeded"})", "application/json"};
    };
    FakeVoiceInput voice; configure(voice, server);
    QStringList errors, texts;
    QObject::connect(&voice, &VoiceInput::errorOccurred, &voice, [&](const QString &s) { errors.append(s); });
    QObject::connect(&voice, &VoiceInput::textRecognized, &voice, [&](const QString &s) { texts.append(s); });
    voice.startRecording(); voice.answerPermission(true);
    CHECK(waitFor([&] { return !errors.isEmpty(); }, VoiceInput::kChunkIntervalMs + 2000));
    CHECK(voice.state() == VoiceInput::State::Idle && !voice.audio.isOpen());
    CHECK(texts.isEmpty());
    recordAndStop(voice);
    CHECK(waitFor([&] { return texts.size() == 1; }));
    CHECK(texts.value(0) == "retry succeeded" && voice.state() == VoiceInput::State::Idle);
}

static void testRepeatedUploads()
{
    Server server; CHECK(server.isListening());
    server.respond = [](const auto &, int index) {
        return Response{index % 2 ? R"({"text":"ok"})" : R"({"error":"unauthorized"})",
                        "application/json", index % 2 ? 200 : 401};
    };
    for (int i = 0; i < 20; ++i) {
        FakeVoiceInput voice; configure(voice, server);
        recordAndStop(voice);
        CHECK(waitFor([&] { return voice.state() == VoiceInput::State::Idle; }));
    }
    drain();
    CHECK(server.requests.size() == 20);
}

static void testIndependentVoiceSettings()
{
    Server first, second;
    CHECK(first.isListening() && second.isListening());
    first.respond = second.respond = [](const auto &, int) {
        return Response{R"({"text":"configured service"})", "application/json"};
    };
    AiPreferences prefs;
    prefs.provider = QStringLiteral("deepseek");
    prefs.model = QStringLiteral("chat-only-model");
    prefs.voiceTranscriptionUrl = QStringLiteral("http://127.0.0.1:%1/custom/transcribe").arg(first.serverPort());
    prefs.voiceModel = QStringLiteral("asr-first");
    CHECK(prefs.save());
    CHECK(prefs.setApiKey(QStringLiteral("chat-key-must-not-be-sent")));
    qputenv("AI_API_KEY", "generic-chat-key-must-not-be-sent");
    qputenv("ZAI_API_KEY", "zhipu-chat-key-must-not-be-sent");

    FakeVoiceInput voice;
    QStringList errors;
    QObject::connect(&voice, &VoiceInput::errorOccurred, &voice, [&](const auto &error) { errors.append(error); });
    voice.startRecording();
    CHECK(voice.state() == VoiceInput::State::Idle);
    CHECK(voice.permissions.isEmpty() && voice.captureStarts == 0);
    CHECK(errors.size() == 1 && errors.value(0).contains(QStringLiteral("AI 设置 → 语音识别")));
    CHECK(first.requests.isEmpty());

    CHECK(prefs.setVoiceApiKey(QStringLiteral("first-voice-key")));
    voice.startRecording();
    CHECK(voice.state() == VoiceInput::State::RequestingPermission);
    voice.answerPermission(true);
    CHECK(voice.isRecording());

    // A save mid-recording must not change the destination or key of this recording.
    prefs.voiceTranscriptionUrl = QStringLiteral("http://127.0.0.1:%1/new/transcribe").arg(second.serverPort());
    prefs.voiceModel = QStringLiteral("asr-second");
    CHECK(prefs.setVoiceApiKey(QStringLiteral("second-voice-key")));
    CHECK(prefs.save());
    voice.stopRecording();
    CHECK(waitFor([&] { return voice.state() == VoiceInput::State::Idle; }));
    CHECK(first.rawRequests.size() == 1 && second.rawRequests.isEmpty());
    const auto firstRequest = first.rawRequests.value(0);
    CHECK(firstRequest.startsWith("POST /custom/transcribe "));
    CHECK(firstRequest.contains("Bearer first-voice-key"));
    CHECK(firstRequest.contains("name=\"model\"\r\n\r\nasr-first\r\n"));
    CHECK(!firstRequest.contains("chat-key") && !firstRequest.contains("chat-only-model"));

    // The same panel/VoiceInput instance picks up new preferences on the next click.
    recordAndStop(voice);
    CHECK(waitFor([&] { return voice.state() == VoiceInput::State::Idle; }));
    CHECK(second.rawRequests.size() == 1);
    const auto secondRequest = second.rawRequests.value(0);
    CHECK(secondRequest.startsWith("POST /new/transcribe "));
    CHECK(secondRequest.contains("Bearer second-voice-key"));
    CHECK(secondRequest.contains("name=\"model\"\r\n\r\nasr-second\r\n"));
    CHECK(!secondRequest.contains("chat-key") && !secondRequest.contains("first-voice-key"));
    CHECK(errors.size() == 1);

    prefs.voiceTranscriptionUrl = QStringLiteral("file:///invalid");
    CHECK(prefs.save());
    voice.startRecording();
    CHECK(voice.permissions.isEmpty() && voice.state() == VoiceInput::State::Idle);
    CHECK(errors.size() == 2 && errors.last().contains(QStringLiteral("接口地址无效")));
    CHECK(AiPreferences().save());
    aiprefstest::secrets.clear();
    qunsetenv("AI_API_KEY");
    qunsetenv("ZAI_API_KEY");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    aiprefstest::configRoot = config.path();
    qunsetenv("CUBESHELL_VOICE_API_KEY");
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    testIndependentVoiceSettings();
    testWavAndSilence();
    testPermissionAndCaptureFailures();
    testRecognitionResponses();
    testDisconnectAndDestruction();
    testFailureWhileRecordingAndRetry();
    testRepeatedUploads();
    qInfo() << "Voice input failures:" << failures;
    return failures ? 1 : 0;
}
