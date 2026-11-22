#include "audio/AudioSpectrumAnalyzer.h"

#include "audio/SpectrumMath.h"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QMediaPlayer>
#include <QTimer>

#include <cmath>

namespace {

void trimBuffer(QVector<float> &mono)
{
    const int keep = SpectrumMath::kFftSize * 4;
    if (mono.size() > keep)
        mono.remove(0, mono.size() - keep);
}

} // namespace

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer(QObject *parent)
    : QObject(parent)
{
    setObjectName(QStringLiteral("AudioSpectrumAnalyzer"));

    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(33);
    connect(m_idleTimer, &QTimer::timeout, this, &AudioSpectrumAnalyzer::emitIdleLevels);
}

AudioSpectrumAnalyzer::~AudioSpectrumAnalyzer()
{
    stopCapture();
    if (m_player != nullptr)
        m_player->setAudioBufferOutput(nullptr);
}

void AudioSpectrumAnalyzer::attachPlayer(QMediaPlayer *player)
{
    if (m_player == player)
        return;
    stopCapture();
    if (m_player != nullptr) {
        m_player->setAudioBufferOutput(nullptr);
        disconnect(m_player, nullptr, this, nullptr);
    }

    m_player = player;
    if (m_player == nullptr) {
        emit unavailable(tr("播放器音频缓冲尚未就绪，动感视图继续使用拟真动画。"));
        return;
    }

    if (m_bufferOutput == nullptr) {
        m_bufferOutput = new QAudioBufferOutput(this);
        connect(m_bufferOutput, &QAudioBufferOutput::audioBufferReceived,
                this, &AudioSpectrumAnalyzer::onAudioBuffer);
    }
    // QAudioBufferOutput 与 QAudioOutput 可以并存：播放链路不变，只是额外
    // 拿到同一份解码后的 PCM。这里只绑定播放器自身，不接触系统混音。
    m_player->setAudioBufferOutput(m_bufferOutput);
    connect(m_player, &QObject::destroyed, this, [this] {
        m_player = nullptr;
        stopCapture();
    });
}

void AudioSpectrumAnalyzer::startCapture()
{
    if (m_enabled)
        return;
    if (m_player == nullptr || m_bufferOutput == nullptr) {
        emit unavailable(tr("播放器音频缓冲尚未就绪，动感视图继续使用拟真动画。"));
        return;
    }
    m_enabled = true;
    m_mono.clear();
    m_smoothed.fill(0.0f, SpectrumMath::kBandCount);
    m_format = QAudioFormat();
    m_frameClock.start();
    m_lastDataClock.start();
    m_idleTimer->start();
}

void AudioSpectrumAnalyzer::stopCapture()
{
    if (!m_enabled)
        return;
    m_enabled = false;
    m_idleTimer->stop();
    m_mono.clear();
    m_smoothed.fill(0.0f, SpectrumMath::kBandCount);
    m_format = QAudioFormat();
}

void AudioSpectrumAnalyzer::resetAnalysis(const QAudioFormat &format)
{
    m_format = format;
    m_mono.clear();
    m_smoothed.fill(0.0f, SpectrumMath::kBandCount);
    m_frameClock.restart();
}

void AudioSpectrumAnalyzer::onAudioBuffer(const QAudioBuffer &buffer)
{
    if (!m_enabled)
        return;
    if (!buffer.isValid()) {
        // 音频流结束时会收到空 buffer；停止旧窗口，避免把尾部数据继续分析。
        m_mono.clear();
        return;
    }
    if (buffer.frameCount() <= 0)
        return;

    const QAudioFormat format = buffer.format();
    if (!format.isValid() || format.sampleRate() <= 0 || format.channelCount() <= 0
        || format.bytesPerSample() <= 0 || format.bytesPerFrame() <= 0) {
        return;
    }
    if (format != m_format)
        resetAnalysis(format);

    const quint8 *data = buffer.constData<quint8>();
    if (data == nullptr)
        return;

    const int channels = format.channelCount();
    const int bytesPerSample = format.bytesPerSample();
    const int bytesPerFrame = format.bytesPerFrame();
    const qsizetype frames = buffer.frameCount();
    m_mono.reserve(m_mono.size() + int(frames));
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const quint8 *sampleBase = data + frame * bytesPerFrame;
        float sum = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            const float value =
                format.normalizedSampleValue(sampleBase + channel * bytesPerSample);
            sum += std::isfinite(value) ? value : 0.0f;
        }
        m_mono.append(sum / float(channels));
    }
    trimBuffer(m_mono);
    m_lastDataClock.restart();

    if (m_mono.size() < SpectrumMath::kFftSize || m_frameClock.elapsed() < 33)
        return;

    emitLevels(SpectrumMath::analyze(m_mono, format.sampleRate(),
                                     SpectrumMath::kBandCount));
    m_frameClock.restart();
}

void AudioSpectrumAnalyzer::emitIdleLevels()
{
    if (!m_enabled || m_lastDataClock.elapsed() < 150)
        return;
    // 播放器缓冲停顿时持续送 0，避免界面把上一帧能量冻结成“还在播放”。
    emitLevels(QVector<float>(SpectrumMath::kBandCount, 0.0f));
}

void AudioSpectrumAnalyzer::emitLevels(const QVector<float> &raw)
{
    if (m_smoothed.size() != raw.size())
        m_smoothed = QVector<float>(raw.size(), 0.0f);
    for (int i = 0; i < m_smoothed.size(); ++i) {
        const float target = raw.at(i);
        // 起音快、回落慢：鼓点看得见，但不会每帧乱跳。
        const float factor = target > m_smoothed.at(i) ? 0.45f : 0.16f;
        m_smoothed[i] += (target - m_smoothed.at(i)) * factor;
    }
    emit levelsChanged(m_smoothed);
}
