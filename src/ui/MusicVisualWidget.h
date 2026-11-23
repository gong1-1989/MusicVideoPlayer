#ifndef MUSICVISUALWIDGET_H
#define MUSICVISUALWIDGET_H

#include "media/MediaItem.h"

#include <QColor>
#include <QPixmap>
#include <QStringList>
#include <QVector>
#include <QWidget>

class AudioSpectrumAnalyzer;
class QMediaPlayer;
class QTimer;

/// 音乐播放时的中央视觉层。
/// 音频没有视频帧，原来的 VideoSurface 会一直黑屏；这里用动态渐变、
/// 频谱柱和曲目信息组成“唱片舞台”，有封面时叠加封面，无封面则保持梦幻风格。
/// 频谱直接读取 QMediaPlayer 的 QAudioBufferOutput（仅播放器自身音频），
/// 只做轻量 FFT；播放器缓冲不可用时退回低开销的拟真动画。
class MusicVisualWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MusicVisualWidget(QWidget *parent = nullptr);
    ~MusicVisualWidget() override;

    void setTrack(const MediaItem &item);
    /// 来源徽标显示源配置里的名称，而不是内部 sourceId。
    void setSourceDisplayName(const QString &name);
    /// 主题强调色；有封面时会用封面主色覆盖，复位时回落到这里。
    void setAccentColor(const QColor &color);
    /// 界面字号变化时由窗口推入，避免 30fps 重绘每帧构造 QSettings。
    void setUiFontPx(int pixels);
    /// 删除当前播放条目后整轨复位：曲目文案、封面、频谱、歌词和动画状态一起清空
    void reset();
    void setPlaying(bool playing);
    void setMediaPlayer(QMediaPlayer *player);
    void setLyrics(const QStringList &lines);
    void setLyricIndex(int index);
    void clearLyrics();
    /// 播放器解码音频的真实频段能量；为空或过期时自动回退到拟真动画。
    void setAudioSpectrum(const QVector<float> &levels);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateCaptureState();
    QColor spectrumColor(int bandIndex) const;
    void drawBackground(QPainter &painter, const QRectF &rect);
    void drawSpectrum(QPainter &painter, const QRectF &rect);
    void drawTrackInfo(QPainter &painter, const QRectF &rect, int basePx);
    void drawDisc(QPainter &painter, const QRectF &rect);
    void drawLyrics(QPainter &painter, const QRectF &rect, int basePx);

    MediaItem m_item;
    QString m_sourceDisplayName;
    QPixmap m_cover;
    /// 按当前圆盘半径预处理好的封面；暂停/静止时不必每帧重新 SmoothScaled。
    QPixmap m_coverScaled;
    QTimer *m_timer = nullptr;
    AudioSpectrumAnalyzer *m_spectrum = nullptr;
    QVector<float> m_audioSpectrum;
    /// 以 33ms 定时器计数；约 300ms 没有新帧就认为播放器缓冲暂停，回退拟真动画。
    int m_audioSpectrumAge = 1000;
    /// 歌词强调色；没有真实频谱时也用于唱片外圈光晕的默认冷色。
    QColor m_accent{96, 132, 255};
    QColor m_themeAccent{96, 132, 255};
    QStringList m_lyrics;
    int m_lyricIndex = -1;
    int m_uiFontPx = 13;
    qreal m_phase = 0.0;
    int m_coverScaledRadius = 0;
    bool m_playing = false;
};

#endif // MUSICVISUALWIDGET_H
