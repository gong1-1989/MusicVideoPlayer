#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include "core/PlaylistModel.h"
#include "media/MediaItem.h"

#include <QMediaPlayer>
#include <QObject>
#include <QPointer>

class QAudioOutput;

/// 播放状态机：持有 QMediaPlayer，负责"上一首/下一首/循环模式/媒体结束"，
/// 界面只通过信号观察状态，不再自己数列表下标。
class PlaybackController : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        ListLoop,
        SingleLoop,
        Random
    };
    Q_ENUM(Mode)

    explicit PlaybackController(QObject *parent = nullptr);

    void setPlaylist(PlaylistModel *playlist);
    PlaylistModel *playlist() const { return m_playlist.data(); }

    void playRow(int row);
    void togglePlayPause();
    void stop();
    void next();
    void previous();
    void seek(qint64 ms);

    bool hasCurrentItem() const;
    MediaItem currentItem() const;
    int currentRow() const { return m_row; }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    QString modeText() const;

    int volumePercent() const { return m_volume; }
    void setVolumePercent(int percent);

    qint64 position() const;
    qint64 duration() const;

    QMediaPlayer *mediaPlayer() const { return m_player; }

signals:
    void currentItemChanged(const MediaItem &item, int row);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void modeChanged(PlaybackController::Mode mode);
    void volumePercentChanged(int percent);
    void errorOccurred(const QString &message);

private:
    int rowCount() const;
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void updateLoops();

    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    QPointer<PlaylistModel> m_playlist;
    int m_row = -1;
    Mode m_mode = Mode::ListLoop;
    int m_volume = 60;
};

#endif // PLAYBACKCONTROLLER_H
