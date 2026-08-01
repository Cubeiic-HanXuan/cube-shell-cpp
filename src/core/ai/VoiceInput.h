#pragma once

// VoiceInput.h — microphone recording + GLM-ASR speech recognition.
//
// 对应Python: core/ai/voice_input.py::VoiceInputManager
//             (+ _SpeechRecognitionThread 的识别/错误映射逻辑)
//
// 实现差异（行为等价）：Python 用 QMediaRecorder 录 M4A 再经 afconvert 转
// WAV；C++ 直接用 QAudioSource 采集 16kHz/Int16/Mono PCM，自行拼 WAV 头，
// 免去外部转换步骤。识别调用改为 QNetworkAccessManager multipart POST
// GLM-ASR HTTP 接口（model=glm-asr-2512），纯异步无工作线程。
//
// 录音过程中每 5s 自动分段识别（partialTextRecognized 实时回显），
// 停止后等全部分段完成再发 textRecognized。
//
// 状态机：Idle → Recording → Processing(等待剩余分段识别) → Idle。

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QAudioSource;
class QIODevice;
class QNetworkAccessManager;
class QNetworkReply;

namespace cubeshell {

// 对应Python: voice_input.py::VoiceInputManager
class VoiceInput : public QObject {
    Q_OBJECT
public:
    // 对应Python: _STATE_IDLE / _STATE_RECORDING（+识别收尾 Processing）
    enum class State {
        Idle,
        Recording,
        Processing,     // 录音已停止，等待剩余分段识别完成
    };
    Q_ENUM(State)

    // 分段间隔。对应Python: VoiceInputManager.CHUNK_INTERVAL_MS
    static constexpr int kChunkIntervalMs = 5000;

    explicit VoiceInput(QObject *parent = nullptr);
    ~VoiceInput() override;

    State state() const { return m_state; }
    // 对应Python: VoiceInputManager.is_recording
    bool isRecording() const { return m_state != State::Idle; }

    // 识别接口地址（默认智谱 GLM-ASR，可覆盖用于测试）。
    void setTranscriptionUrl(const QString &url) { m_transcriptionUrl = url; }
    void setApiKey(const QString &key) { m_apiKeyOverride = key; }

    // --- 纯函数（供单测） ---

    // 16kHz/16bit/Mono PCM → 完整 WAV 文件字节。
    static QByteArray buildWavFile(const QByteArray &pcm);
    // 静音占位符过滤。对应Python: VoiceInputManager._SILENCE_MARKERS
    static bool isSilenceMarker(const QString &text);

public slots:
    // 对应Python: toggle_recording / start_recording / stop_recording
    void toggleRecording();
    void startRecording();
    void stopRecording();

signals:
    // 对应Python: VoiceInputManager 的同名信号集
    void recordingStarted();                    // recording_started
    void recordingStopped();                    // recording_stopped
    void recognitionStarted();                  // recognition_started
    void partialTextRecognized(const QString &text); // partial_text_recognized
    void textRecognized(const QString &text);   // text_recognized
    void errorOccurred(const QString &message); // error_occurred

private:
    void onAudioReadyRead();
    void onChunkTimeout();          // 对应Python: _on_chunk_timeout
    void flushChunk();              // 截取当前 PCM 缓冲送识别
    void recognizeChunk(const QByteArray &pcm); // _start_chunk_recognition
    void onRecognitionFinished(QNetworkReply *reply);
    void tryFinalize();             // 对应Python: _try_finalize
    bool ensureAudioSource();       // 对应Python: _ensure_recorder
    static QString mapHttpError(int status, const QString &body,
                                const QString &fallback);

    State m_state = State::Idle;
    QString m_transcriptionUrl;
    QString m_apiKeyOverride;

    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioIo = nullptr;         // QAudioSource::start() 返回
    QAudioFormat m_format;
    QByteArray m_pcmBuffer;                 // 当前分段累积的原始 PCM

    QTimer m_chunkTimer;
    QNetworkAccessManager *m_network = nullptr;

    QString m_accumulatedText;              // _accumulated_text
    int m_pendingChunks = 0;                // _pending_chunks
};

} // namespace cubeshell
