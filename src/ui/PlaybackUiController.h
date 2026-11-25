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
class QTimer;

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
    QPushButton *seekBack = nullptr;
    QPushButton *seekForward = nullptr;
    QPushButton *mode = nullptr;
};

class PlaybackUiController : public QObject
{
    Q_OBJECT

public:
    PlaybackUiController(PlaybackController *player, PlaylistController *playlists,
                         VideoSurface *surface, LyricsController *lyrics,
                         const PlaybackUi &ui, QObject *parent = nullptr);
    ~PlaybackUiController() override;

    /// 当前播放项失效（如列表行被删除）时，把进度、时间和歌词一起清零
    void clear();

    /// 相对当前位置快进/快退指定秒数；目标会夹到 0 与总时长之间。
    /// 按钮单击、连续长按和快捷键共用这一入口，保证 HLS 定位提示与防抖一致。
    bool seekBySeconds(int seconds, bool announce = true);

    /// 主题切换后重绘内置图标；外部自定义图标会重新按当前目录加载。
    void refreshIcons();

signals:
    void statusMessage(const QString &message);
    void currentItemChanged(const MediaItem &item, int row);

protected:
    /// 监听应用/窗口失焦：按住按钮时 Alt+Tab 也要恢复 1× 播放速率。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void playPressed();
    void stopPressed();
    void modePressed();
    void refreshModeButton();
    void refreshPlayButton(QMediaPlayer::PlaybackState state);
    /// HLS 深拖动时 FFmpeg 要从目标前若干分片顺序读起，这里只负责把“正在定位”说清楚
    void beginSeek(qint64 target);
    /// 防抖后再真正下发，连续点击只执行最后一次，避免反复打断代理预取
    void flushSeek();
    bool isHlsItem() const;

    /// 长按判定与 2× 快进；松手/切歌/停止/失焦都必须走 endForwardHold()
    void beginForwardHold();
    void endForwardHold(bool announce = true);
    void beginRewindHold();
    void advanceRewindHold();
    void endRewindHold();
    /// 统一取消两个长按并恢复原速率，避免状态残留到下一首
    void cancelHolds(bool announce = true);
    void refreshSeekButtons();
    qint64 seekBasePosition() const;

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
    /// 连续点击进度条时只保留最后一次目标
    qint64 m_seekPendingTarget = -1;
    QTimer *m_seekTimer = nullptr;

    QTimer *m_forwardHoldTimer = nullptr;
    QTimer *m_rewindHoldTimer = nullptr;
    QTimer *m_rewindRepeatTimer = nullptr;
    bool m_forwardHoldActive = false;
    bool m_rewindHoldActive = false;
    /// 长按已经接管本次按下后，松手不能再补一次单击跳转
    bool m_forwardPressConsumed = false;
    bool m_rewindPressConsumed = false;
    /// 进入长按前的速率；松手后原样恢复，兼容未来可能加入的其他倍速入口
    qreal m_rateBeforeHold = 1.0;
};

#endif // PLAYBACKUICONTROLLER_H
