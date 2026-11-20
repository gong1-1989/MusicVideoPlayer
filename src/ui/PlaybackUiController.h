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

signals:
    void statusMessage(const QString &message);
    void currentItemChanged(const MediaItem &item, int row);

private:
    void playPressed();
    void stopPressed();
    void modePressed();
    void refreshModeButton();

    PlaybackUi m_ui;
    PlaybackController *m_player;
    PlaylistController *m_playlists;
    VideoSurface *m_surface;
    LyricsController *m_lyrics;
    bool m_userSeeking = false;
};

#endif // PLAYBACKUICONTROLLER_H
