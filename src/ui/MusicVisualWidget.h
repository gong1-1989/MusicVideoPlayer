#ifndef MUSICVISUALWIDGET_H
#define MUSICVISUALWIDGET_H

#include "media/MediaItem.h"

#include <QColor>
#include <QPixmap>
#include <QStringList>
#include <QVector>
#include <QWidget>

class AudioSpectrumAnalyzer;
class QTimer;

/// 音乐播放时的中央视觉层。
/// 音频没有视频帧，原来的 VideoSurface 会一直黑屏；这里用动态渐变、
/// 频谱柱和曲目信息组成“唱片舞台”，有封面时叠加封面，无封面则保持梦幻风格。
/// 频谱优先使用 WASAPI Loopback 采集当前系统音频并做 FFT；采集不可用时
/// 退回低开销的拟真动画，不阻塞或干扰 QMediaPlayer 的播放链路。
class MusicVisualWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MusicVisualWidget(QWidget *parent = nullptr);
    ~MusicVisualWidget() override;

    void setTrack(const MediaItem &item);
    void setPlaying(bool playing);
    void setAccent(const QColor &color);
    void setLyrics(const QStringList &lines);
    void setLyricIndex(int index);
    void clearLyrics();
    /// WASAPI Loopback 的真实频段能量；为空或过期时自动回退到拟真动画。
    void setAudioSpectrum(const QVector<float> &levels);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateCaptureState();
    void drawBackground(QPainter &painter, const QRectF &rect);
    void drawSpectrum(QPainter &painter, const QRectF &rect);
    void drawTrackInfo(QPainter &painter, const QRectF &rect);
    void drawDisc(QPainter &painter, const QRectF &rect);
    void drawLyrics(QPainter &painter, const QRectF &rect);

    MediaItem m_item;
    QPixmap m_cover;
    /// 按当前圆盘半径预处理好的封面；暂停/静止时不必每帧重新 SmoothScaled。
    QPixmap m_coverScaled;
    QTimer *m_timer = nullptr;
    AudioSpectrumAnalyzer *m_spectrum = nullptr;
    QVector<float> m_audioSpectrum;
    /// 以 33ms 定时器计数；约 330ms 没有新帧就认为采集已停，回退拟真动画。
    int m_audioSpectrumAge = 1000;
    QColor m_accent{96, 132, 255};
    QStringList m_lyrics;
    int m_lyricIndex = -1;
    qreal m_phase = 0.0;
    int m_coverScaledRadius = 0;
    bool m_playing = false;
};

#endif // MUSICVISUALWIDGET_H
