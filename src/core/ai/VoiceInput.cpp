// VoiceInput.cpp — see VoiceInput.h.
//
// 对应Python: core/ai/voice_input.py

#include "VoiceInput.h"

#include "AiPreferences.h"

#include <QAudioSource>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtEndian>

namespace cubeshell {

namespace {

// 默认智谱 GLM-ASR 转写接口（OpenAI 兼容路径）。
// 对应Python: zai SDK client.audio.transcriptions.create
const char kDefaultTranscriptionUrl[] =
    "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions";

// 对应Python: _SpeechRecognitionThread 里 model="glm-asr-2512"
const char kAsrModel[] = "glm-asr-2512";

// PCM 段小于该字节数视为无有效语音，跳过识别。
// 对应Python: os.path.getsize(...) < 512 则跳过
constexpr int kMinChunkBytes = 512;

} // namespace

// ---------------------------------------------------------------------------
// static helpers
// ---------------------------------------------------------------------------

// 对应Python: afconvert -f WAVE -d LEI16@16000 -c 1（此处直接生成同规格 WAV）
QByteArray VoiceInput::buildWavFile(const QByteArray &pcm)
{
    constexpr quint32 sampleRate = 16000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    constexpr quint32 byteRate = sampleRate * channels * bitsPerSample / 8;
    constexpr quint16 blockAlign = channels * bitsPerSample / 8;

    const quint32 dataSize = static_cast<quint32>(pcm.size());

    QByteArray wav;
    wav.reserve(44 + pcm.size());

    auto appendLe32 = [&wav](quint32 v) {
        char buf[4];
        qToLittleEndian(v, buf);
        wav.append(buf, 4);
    };
    auto appendLe16 = [&wav](quint16 v) {
        char buf[2];
        qToLittleEndian(v, buf);
        wav.append(buf, 2);
    };

    wav.append("RIFF", 4);
    appendLe32(36 + dataSize);          // RIFF chunk size
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    appendLe32(16);                     // fmt chunk size (PCM)
    appendLe16(1);                      // audio format = PCM
    appendLe16(channels);
    appendLe32(sampleRate);
    appendLe32(byteRate);
    appendLe16(blockAlign);
    appendLe16(bitsPerSample);
    wav.append("data", 4);
    appendLe32(dataSize);
    wav.append(pcm);
    return wav;
}

// 对应Python: _SILENCE_MARKERS = {"#", "##", "###", "...", "。", ""}
// （任务要求额外过滤 ASR 静音时常见的"谢谢观看"类占位输出）
bool VoiceInput::isSilenceMarker(const QString &text)
{
    static const QStringList markers = {
        QStringLiteral("#"), QStringLiteral("##"), QStringLiteral("###"),
        QStringLiteral("..."), QStringLiteral("。"), QString(),
        QStringLiteral("谢谢观看"), QStringLiteral("谢谢大家"),
    };
    return markers.contains(text.trimmed());
}

// 对应Python: _SpeechRecognitionThread.run 的异常分类文案
QString VoiceInput::mapHttpError(int status, const QString &body,
                                 const QString &fallback)
{
    const QString lower = body.toLower();
    if (status == 401 || lower.contains(QStringLiteral("api_key"))
        || lower.contains(QStringLiteral("unauthorized")))
        return QStringLiteral("API Key 无效或已过期，请更新配置");
    if (lower.contains(QStringLiteral("timeout")))
        return QStringLiteral("网络请求超时，请检查网络连接");
    if (status == 413 || lower.contains(QStringLiteral("too large")))
        return QStringLiteral("音频文件过大，请录制不超过 30 秒");
    return QStringLiteral("语音识别失败: %1").arg(fallback);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

VoiceInput::VoiceInput(QObject *parent)
    : QObject(parent)
    , m_transcriptionUrl(QString::fromLatin1(kDefaultTranscriptionUrl))
    , m_network(new QNetworkAccessManager(this))
{
    // 分段定时器。对应Python: self._chunk_timer
    m_chunkTimer.setInterval(kChunkIntervalMs);
    connect(&m_chunkTimer, &QTimer::timeout,
            this, &VoiceInput::onChunkTimeout);
}

VoiceInput::~VoiceInput()
{
    if (m_audioSource) {
        m_audioSource->stop();
        m_audioSource = nullptr;
        m_audioIo = nullptr;
    }
}

// 对应Python: VoiceInputManager._ensure_recorder
bool VoiceInput::ensureAudioSource()
{
    if (m_audioSource)
        return true;

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        emit errorOccurred(QStringLiteral("未检测到麦克风设备"));
        return false;
    }

    // 直接以 GLM-ASR 需要的规格采集，免转换：16kHz / Int16 / Mono
    m_format.setSampleRate(16000);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(m_format)) {
        // 设备不支持时退回其首选格式（多数桌面设备均支持 16k/Int16）
        m_format = device.preferredFormat();
        m_format.setChannelCount(1);
        m_format.setSampleFormat(QAudioFormat::Int16);
    }

    m_audioSource = new QAudioSource(device, m_format, this);
    return true;
}

// ---------------------------------------------------------------------------
// recording control
// ---------------------------------------------------------------------------

// 对应Python: VoiceInputManager.toggle_recording
void VoiceInput::toggleRecording()
{
    if (m_state != State::Idle)
        stopRecording();
    else
        startRecording();
}

// 对应Python: VoiceInputManager.start_recording
void VoiceInput::startRecording()
{
    if (m_state != State::Idle)
        return;
    if (!ensureAudioSource())
        return;

    m_accumulatedText.clear();
    m_pendingChunks = 0;
    m_pcmBuffer.clear();

    m_audioIo = m_audioSource->start();
    if (!m_audioIo) {
        emit errorOccurred(QStringLiteral("初始化录音设备失败: %1")
                               .arg(int(m_audioSource->error())));
        return;
    }
    connect(m_audioIo, &QIODevice::readyRead,
            this, &VoiceInput::onAudioReadyRead, Qt::UniqueConnection);

    m_state = State::Recording;
    m_chunkTimer.start();
    emit recordingStarted();
}

// 对应Python: VoiceInputManager.stop_recording
void VoiceInput::stopRecording()
{
    if (m_state != State::Recording)
        return;

    m_chunkTimer.stop();
    emit recordingStopped();

    // 冲刷设备中剩余样本后停止采集
    if (m_audioIo)
        m_pcmBuffer.append(m_audioIo->readAll());
    m_audioSource->stop();
    m_audioIo = nullptr;

    m_state = State::Processing;
    flushChunk();       // 最终段送识别
    tryFinalize();      // 若无任何待识别分段，立即收尾
}

void VoiceInput::onAudioReadyRead()
{
    if (m_audioIo)
        m_pcmBuffer.append(m_audioIo->readAll());
}

// 对应Python: VoiceInputManager._on_chunk_timeout（分段边界）
void VoiceInput::onChunkTimeout()
{
    if (m_state != State::Recording)
        return;
    if (m_audioIo)
        m_pcmBuffer.append(m_audioIo->readAll());
    flushChunk();
}

// 截取当前 PCM 缓冲送识别（等价 Python 的"停段→取文件→开新段"）
void VoiceInput::flushChunk()
{
    const QByteArray pcm = m_pcmBuffer;
    m_pcmBuffer.clear();
    if (pcm.size() < kMinChunkBytes)
        return;
    recognizeChunk(pcm);
}

// ---------------------------------------------------------------------------
// recognition (HTTP multipart POST)
// ---------------------------------------------------------------------------

// 对应Python: _start_chunk_recognition + _SpeechRecognitionThread.run
void VoiceInput::recognizeChunk(const QByteArray &pcm)
{
    QString apiKey = m_apiKeyOverride;
    if (apiKey.isEmpty())
        apiKey = AiPreferences::load().apiKey();
    if (apiKey.isEmpty()) {
        emit errorOccurred(QStringLiteral(
            "未配置 API Key，请在「设置 -> AI 设置」中配置"));
        tryFinalize();
        return;
    }

    ++m_pendingChunks;
    emit recognitionStarted();

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"model\""));
    modelPart.setBody(QByteArray(kAsrModel));
    multiPart->append(modelPart);

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral(
                           "form-data; name=\"file\"; filename=\"voice.wav\""));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("audio/wav"));
    filePart.setBody(buildWavFile(pcm));
    multiPart->append(filePart);

    QNetworkRequest request{QUrl(m_transcriptionUrl)};
    request.setRawHeader("Authorization",
                         QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setTransferTimeout(30000);

    QNetworkReply *reply = m_network->post(request, multiPart);
    multiPart->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onRecognitionFinished(reply);
    });
}

// 对应Python: _on_chunk_recognized / _on_recognition_error /
//             _on_chunk_finished 三个回调的合并处理
void VoiceInput::onRecognitionFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    --m_pendingChunks;

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();

    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        emit errorOccurred(mapHttpError(
            status, QString::fromUtf8(body),
            reply->error() != QNetworkReply::NoError
                ? reply->errorString()
                : QStringLiteral("HTTP %1").arg(status)));
        tryFinalize();
        return;
    }

    // 兼容多种响应格式：{"text": ...} 或 choices[0].message.content
    // 对应Python: response.text / response.choices[0].message.content
    QString text;
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    if (obj.contains(QStringLiteral("text"))) {
        text = obj.value(QStringLiteral("text")).toString();
    } else if (obj.contains(QStringLiteral("choices"))) {
        const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
        if (!choices.isEmpty()) {
            text = choices.first().toObject()
                       .value(QStringLiteral("message")).toObject()
                       .value(QStringLiteral("content")).toString();
        }
    }
    text = text.trimmed();

    // 静音占位符过滤。对应Python: _on_chunk_recognized
    if (!text.isEmpty() && !isSilenceMarker(text)) {
        m_accumulatedText += text;
        emit partialTextRecognized(m_accumulatedText);
    }

    tryFinalize();
}

// 对应Python: VoiceInputManager._try_finalize
void VoiceInput::tryFinalize()
{
    if (m_state != State::Processing || m_pendingChunks > 0)
        return;
    m_state = State::Idle;
    emit textRecognized(m_accumulatedText);
}

} // namespace cubeshell
