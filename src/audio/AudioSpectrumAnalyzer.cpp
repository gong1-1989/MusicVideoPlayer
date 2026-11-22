#include "audio/AudioSpectrumAnalyzer.h"

#include "audio/SpectrumMath.h"

#include <QElapsedTimer>
#include <QString>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#endif

#include <cstring>
#include <utility>

#ifdef Q_OS_WIN
namespace {

// MinGW 的 uuid 库不导出这几个音频 GUID，直接按 Windows SDK 的值定义，
// 避免链接期缺符号；数值与 CLSID_MMDeviceEnumerator/IID_IAudioClient 等一致。
const CLSID kMmDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kImmDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIAudioCaptureClient = {
    0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(quint32(hr), 16).rightJustified(8, QLatin1Char('0')));
}

template <typename T>
void safeRelease(T *&pointer)
{
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

struct CaptureResources
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    WAVEFORMATEX *format = nullptr;
    HANDLE eventHandle = nullptr;

    ~CaptureResources() { release(); }

    void release()
    {
        if (client != nullptr)
            client->Stop();
        safeRelease(capture);
        safeRelease(client);
        if (eventHandle != nullptr)
            CloseHandle(eventHandle);
        eventHandle = nullptr;
        if (format != nullptr)
            CoTaskMemFree(format);
        format = nullptr;
        safeRelease(device);
        safeRelease(enumerator);
    }
};

bool openLoopback(CaptureResources &resources, QString &error)
{
    HRESULT hr = CoCreateInstance(kMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kImmDeviceEnumerator,
                                  reinterpret_cast<void **>(&resources.enumerator));
    if (FAILED(hr)) {
        error = QObject::tr("无法创建音频设备枚举器（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &resources.device);
    if (FAILED(hr)) {
        error = QObject::tr("没有找到默认扬声器输出设备（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.device->Activate(kIAudioClient, CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&resources.client));
    if (FAILED(hr)) {
        error = QObject::tr("无法打开默认输出设备的音频客户端（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.client->GetMixFormat(&resources.format);
    if (FAILED(hr) || resources.format == nullptr) {
        error = QObject::tr("无法读取扬声器混音格式（%1）").arg(hresultText(hr));
        return false;
    }

    resources.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (resources.eventHandle == nullptr) {
        error = QObject::tr("无法创建音频采集事件");
        return false;
    }

    // 共享模式 + LOOPBACK 才能在不装虚拟声卡的情况下抓到系统正在播放的声音。
    hr = resources.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK
                                          | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      0, 0, resources.format, nullptr);
    if (FAILED(hr)) {
        error = QObject::tr("无法启动 WASAPI Loopback（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.client->SetEventHandle(resources.eventHandle);
    if (FAILED(hr)) {
        error = QObject::tr("无法绑定音频采集事件（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.client->GetService(kIAudioCaptureClient,
                                      reinterpret_cast<void **>(&resources.capture));
    if (FAILED(hr) || resources.capture == nullptr) {
        error = QObject::tr("无法取得音频采集接口（%1）").arg(hresultText(hr));
        return false;
    }

    hr = resources.client->Start();
    if (FAILED(hr)) {
        error = QObject::tr("无法开始音频采集（%1）").arg(hresultText(hr));
        return false;
    }
    return true;
}

enum class PcmSampleFormat
{
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
    Unknown
};

struct PcmConverter
{
    PcmSampleFormat format = PcmSampleFormat::Unknown;
    int channels = 0;
    int bytesPerSample = 0;
    int frameStride = 0;

    bool init(const WAVEFORMATEX *waveFormat, QString &error)
    {
        if (waveFormat == nullptr) {
            error = QObject::tr("音频格式为空");
            return false;
        }
        channels = int(waveFormat->nChannels);
        bytesPerSample = int(waveFormat->wBitsPerSample) / 8;
        frameStride = int(waveFormat->nBlockAlign);
        if (channels <= 0 || channels > 16 || bytesPerSample <= 0
            || frameStride < channels * bytesPerSample) {
            error = QObject::tr("音频混音格式不支持（%1 声道 / %2 位）")
                        .arg(channels)
                        .arg(waveFormat->wBitsPerSample);
            return false;
        }

        bool isFloat = waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = waveFormat->wFormatTag == WAVE_FORMAT_PCM;
        if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && waveFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto *extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(waveFormat);
            isFloat = IsEqualGUID(extended->SubFormat, kSubtypeIeeeFloat);
            isPcm = IsEqualGUID(extended->SubFormat, kSubtypePcm);
        }

        if (isFloat && bytesPerSample == 4) {
            format = PcmSampleFormat::Float32;
        } else if (isPcm && bytesPerSample == 2) {
            format = PcmSampleFormat::Pcm16;
        } else if (isPcm && bytesPerSample == 3) {
            format = PcmSampleFormat::Pcm24;
        } else if (isPcm && bytesPerSample == 4) {
            format = PcmSampleFormat::Pcm32;
        } else {
            error = QObject::tr("音频混音格式暂不支持（格式 %1 / %2 位）")
                        .arg(waveFormat->wFormatTag)
                        .arg(waveFormat->wBitsPerSample);
            return false;
        }
        return true;
    }

    float sampleAt(const BYTE *frame, int channel) const
    {
        const BYTE *source = frame + channel * bytesPerSample;
        switch (format) {
        case PcmSampleFormat::Float32: {
            float value = 0.0f;
            std::memcpy(&value, source, sizeof(value));
            return value;
        }
        case PcmSampleFormat::Pcm16: {
            qint16 value = 0;
            std::memcpy(&value, source, sizeof(value));
            return float(value) / 32768.0f;
        }
        case PcmSampleFormat::Pcm24: {
            const quint32 raw = quint32(source[0]) | (quint32(source[1]) << 8)
                                | (quint32(source[2]) << 16);
            qint32 value = qint32(raw);
            if (value & 0x00800000)
                value |= qint32(0xFF000000);
            return float(value) / 8388608.0f;
        }
        case PcmSampleFormat::Pcm32: {
            qint32 value = 0;
            std::memcpy(&value, source, sizeof(value));
            return float(double(value) / 2147483648.0);
        }
        case PcmSampleFormat::Unknown:
            break;
        }
        return 0.0f;
    }
};

void appendFrames(const PcmConverter &converter, const BYTE *data, UINT32 frames,
                  bool silent, QVector<float> &mono)
{
    mono.reserve(mono.size() + int(frames));
    if (silent || data == nullptr) {
        mono.insert(mono.end(), int(frames), 0.0f);
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame) {
        const BYTE *source = data + frame * UINT32(converter.frameStride);
        float sum = 0.0f;
        for (int channel = 0; channel < converter.channels; ++channel)
            sum += converter.sampleAt(source, channel);
        mono.append(sum / float(converter.channels));
    }
}

void trimBuffer(QVector<float> &mono)
{
    const int keep = SpectrumMath::kFftSize * 4;
    if (mono.size() > keep)
        mono.remove(0, mono.size() - keep);
}

} // namespace
#endif // Q_OS_WIN

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer(QObject *parent)
    : QThread(parent)
{
    setObjectName(QStringLiteral("AudioSpectrumAnalyzer"));
}

AudioSpectrumAnalyzer::~AudioSpectrumAnalyzer()
{
    m_stop.store(true, std::memory_order_relaxed);
    requestInterruption();
    // 析构必须等采集线程退出，不能在 COM 对象还活着时销毁 QThread。
    wait();
}

void AudioSpectrumAnalyzer::startCapture()
{
    if (isRunning())
        return;
    m_stop.store(false, std::memory_order_relaxed);
    start(QThread::NormalPriority);
}

void AudioSpectrumAnalyzer::stopCapture()
{
    m_stop.store(true, std::memory_order_relaxed);
    if (!isRunning())
        return;
    if (!wait(1200)) {
        requestInterruption();
        wait(800);
    }
}

void AudioSpectrumAnalyzer::run()
{
#ifdef Q_OS_WIN
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        emit unavailable(tr("无法初始化 WASAPI COM（0x%1）")
                             .arg(QString::number(quint32(comResult), 16)
                                      .rightJustified(8, QLatin1Char('0'))));
        return;
    }

    {
        CaptureResources resources;
        QString error;
        if (!openLoopback(resources, error)) {
            emit unavailable(tr("%1。动感视图继续使用拟真动画。").arg(error));
            resources.release();
            if (shouldUninitialize)
                CoUninitialize();
            return;
        }

        PcmConverter converter;
        if (!converter.init(resources.format, error)) {
            emit unavailable(tr("%1。动感视图继续使用拟真动画。").arg(error));
            resources.release();
            if (shouldUninitialize)
                CoUninitialize();
            return;
        }

        QElapsedTimer frameClock;
        QElapsedTimer lastDataClock;
        frameClock.start();
        lastDataClock.start();

        QVector<float> mono;
        mono.reserve(SpectrumMath::kFftSize * 4);
        QVector<float> smoothed(SpectrumMath::kBandCount, 0.0f);
        QString captureError;
        bool idle = false;

        while (!m_stop.load(std::memory_order_relaxed)) {
            const DWORD waitResult = WaitForSingleObject(resources.eventHandle, 20);
            if (waitResult == WAIT_FAILED) {
                captureError = tr("等待音频采集事件失败");
                break;
            }

            bool gotData = false;
            for (;;) {
                UINT32 packetFrames = 0;
                HRESULT hr = resources.capture->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    captureError = tr("读取音频包大小失败（%1）").arg(hresultText(hr));
                    break;
                }
                if (packetFrames == 0)
                    break;

                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = resources.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    captureError = tr("读取音频数据失败（%1）").arg(hresultText(hr));
                    break;
                }

                if (frames > 0) {
                    appendFrames(converter, data, frames,
                                 (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, mono);
                    trimBuffer(mono);
                    gotData = true;
                }

                hr = resources.capture->ReleaseBuffer(frames);
                if (FAILED(hr)) {
                    captureError = tr("释放音频缓冲区失败（%1）").arg(hresultText(hr));
                    break;
                }
            }

            if (!captureError.isEmpty())
                break;

            if (gotData) {
                lastDataClock.restart();
                idle = false;
            } else if (lastDataClock.elapsed() > 150) {
                // 系统静音或播放器暂停时不产生新包；清空旧窗口并持续发 0，
                // 否则界面会把上一秒的频谱一直冻结在画面上。
                mono.clear();
                idle = true;
            }

            if (frameClock.elapsed() < 33)
                continue;

            QVector<float> raw;
            if (!idle && mono.size() >= SpectrumMath::kFftSize) {
                raw = SpectrumMath::analyze(mono, int(resources.format->nSamplesPerSec),
                                            SpectrumMath::kBandCount);
            } else {
                raw.fill(0.0f, SpectrumMath::kBandCount);
            }

            for (int i = 0; i < smoothed.size(); ++i) {
                const float target = raw.at(i);
                // 起音快、回落慢：鼓点看得见，但不会每帧乱跳。
                const float factor = target > smoothed[i] ? 0.45f : 0.16f;
                smoothed[i] += (target - smoothed[i]) * factor;
            }
            emit levelsChanged(smoothed);
            frameClock.restart();
        }

        if (!captureError.isEmpty())
            emit unavailable(tr("%1。动感视图继续使用拟真动画。").arg(captureError));
        resources.release();
        if (shouldUninitialize)
            CoUninitialize();
    }
#else
    emit unavailable(tr("当前平台没有实现系统音频采集，动感视图继续使用拟真动画。"));
#endif
}
