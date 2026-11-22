#ifndef AUDIOSPECTRUMANALYZER_H
#define AUDIOSPECTRUMANALYZER_H

#include <QAudioFormat>
#include <QElapsedTimer>
#include <QObject>
#include <QVector>

class QAudioBufferOutput;
class QAudioBuffer;
class QMediaPlayer;
class QTimer;

/// 播放器音频缓冲频谱分析器。
///
/// 通过 QMediaPlayer 的 QAudioBufferOutput 直接读取播放器解码后的 PCM，
/// 因此只分析本播放器的声音，不会把其他应用的输出混进频谱；也不会为了
/// 可视化再开一条系统录音链路。部分音频格式由 Qt 统一转换成归一化样本。
class AudioSpectrumAnalyzer : public QObject
{
    Q_OBJECT

public:
    explicit AudioSpectrumAnalyzer(QObject *parent = nullptr);
    ~AudioSpectrumAnalyzer() override;

    bool isCapturing() const { return m_enabled; }

    /// 绑定播放器；重复调用同一个播放器无副作用。
    void attachPlayer(QMediaPlayer *player);

public slots:
    void startCapture();
    void stopCapture();

signals:
    /// 0..1 的 46 个对数频段能量，已经做过 attack/release 平滑。
    void levelsChanged(const QVector<float> &levels);
    /// 播放器音频缓冲无法使用时只发一次；界面据此继续使用拟真动画。
    void unavailable(const QString &reason);

private slots:
    void onAudioBuffer(const QAudioBuffer &buffer);
    void emitIdleLevels();

private:
    void resetAnalysis(const QAudioFormat &format);
    void emitLevels(const QVector<float> &raw);

    QMediaPlayer *m_player = nullptr;
    QAudioBufferOutput *m_bufferOutput = nullptr;
    QTimer *m_idleTimer = nullptr;
    QElapsedTimer m_frameClock;
    QElapsedTimer m_lastDataClock;
    QVector<float> m_mono;
    QVector<float> m_smoothed;
    /// 播放器缓冲停顿时复用的全零频段，避免每 33ms 重新分配一个向量。
    QVector<float> m_silenceLevels;
    QAudioFormat m_format;
    bool m_enabled = false;
};

#endif // AUDIOSPECTRUMANALYZER_H
