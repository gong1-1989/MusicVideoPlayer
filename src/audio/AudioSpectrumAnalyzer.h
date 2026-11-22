#ifndef AUDIOSPECTRUMANALYZER_H
#define AUDIOSPECTRUMANALYZER_H

#include <QThread>
#include <QVector>

#include <atomic>

/// Windows WASAPI Loopback 采集器：把系统正在播放的音频取一份 PCM，
/// 交给 SpectrumMath 做 FFT，再以约 30Hz 发出 46 个频段能量。
/// 不改变现有 QMediaPlayer 播放链路；采集失败时由界面回退到拟真动画。
class AudioSpectrumAnalyzer : public QThread
{
    Q_OBJECT

public:
    explicit AudioSpectrumAnalyzer(QObject *parent = nullptr);
    ~AudioSpectrumAnalyzer() override;

    bool isCapturing() const { return isRunning(); }

public slots:
    void startCapture();
    void stopCapture();

signals:
    /// 0..1 的 46 个对数频段能量，已经做过 attack/release 平滑。
    void levelsChanged(const QVector<float> &levels);
    /// 无法采集时只发一次；界面据此继续使用原来的正弦动画。
    void unavailable(const QString &reason);

protected:
    void run() override;

private:
    std::atomic_bool m_stop{false};
};

#endif // AUDIOSPECTRUMANALYZER_H
