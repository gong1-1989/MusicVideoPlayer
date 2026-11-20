#ifndef PLAYBACKUICONTROLLER_H
#define PLAYBACKUICONTROLLER_H

#include "core/PlaybackController.h"
#include "media/MediaItem.h"

#include <QObject>

class LyricsController;
class PlaylistController;
class VideoSurface;
class QLabel;
class QPushButton;
class QSlider;

/// 播放条与播放器之间的全部联动：进度、音量、播放/停止/上下曲、模式按钮。
struct PlaybackUi
{
    QSlider *progress = nullptr;
    QSlider *volume = nullptr;
    QLabel *time = nullptr;
    QPushButton *play = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *previous = nullptr;
    QPushButton *next = nullptr;
    QPushButton *mode = nullptr;
};

class PlaybackUiController : public QObject
{
    Q_OBJECT

public:
    PlaybackUiController(PlaybackController *player, PlaylistController *playlists,
                         VideoSurface *surface, LyricsController *lyrics,
                         const PlaybackUi &ui, QObject *parent = nullptr);

    /// 当前播放项失效（如列表行被删除）时，把进度、时间和歌词一起清零
    void clear();

signals:
    void statusMessage(const QString &message);
    void currentItemChanged(const MediaItem &item, int row);

private:
    void playPressed();
    void stopPressed();
    void modePressed();
    void refreshModeButton();
    /// HLS 深拖动时 FFmpeg 要从目标前若干分片顺序读起，这里只负责把“正在定位”说清楚
    void beginSeek(qint64 target);
    bool isHlsItem() const;

    PlaybackUi m_ui;
    PlaybackController *m_player;
    PlaylistController *m_playlists;
    VideoSurface *m_surface;
    LyricsController *m_lyrics;
    bool m_userSeeking = false;
    /// 正在等待画面追上的 seek 目标；小于 0 表示没有待确认的定位
    qint64 m_seekTarget = -1;
    /// 每次定位递增，用来让上一轮的超时回调失效
    quint64 m_seekSerial = 0;
};

#endif // PLAYBACKUICONTROLLER_H
