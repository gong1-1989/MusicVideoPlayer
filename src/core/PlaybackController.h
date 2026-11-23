#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include "core/PlaylistModel.h"
#include "media/MediaItem.h"

#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QAudioOutput;
class QThread;
class QTimer;
class StreamProxy;

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
    ~PlaybackController() override;

    void setPlaylist(PlaylistModel *playlist);
    PlaylistModel *playlist() const { return m_playlist.data(); }
    /// 播放前从 SourceRegistry 恢复内存态 Cookie；只影响播放副本，
    /// 不回写播放列表模型，避免凭据再次落盘。
    void setMediaItemResolver(std::function<MediaItem(const MediaItem &)> resolver);

    void playRow(int row);
    void togglePlayPause();
    void stop();
    void next();
    void previous();
    void seek(qint64 ms);

    /// 不停止播放，只把当前视频进度写回播放列表模型。
    /// 退出前先调用它再保存，避免“先保存、后 stop”导致最后几秒进度丢失。
    void persistCurrentPosition();

    /// 列表里删掉了一行后调用：正在播的那首被删则停下，删的是前面的行则下标前移
    void onPlaylistRowRemoved(int row);

    /// 删除条目前释放代理线程对该条目磁盘缓存的句柄。
    /// 代理为支持拖动会长期保留打开的文件；不先释放，
    /// MediaCache::removeForItem 在 Windows 上会因占用而删不掉。
    void releaseStreamCacheForItem(const MediaItem &item);

    bool hasCurrentItem() const;
    MediaItem currentItem() const;
    int currentRow() const { return m_row; }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    /// 纯函数版模式文案，供 UI/测试复用，不依赖播放器实例
    static QString modeDisplayText(Mode mode, bool compact);

    int volumePercent() const { return m_volume; }
    void setVolumePercent(int percent);

    qint64 position() const;
    qint64 duration() const;

    QMediaPlayer *mediaPlayer() const { return m_player; }

    /// 播放器最近打开的视频/音频媒体列列表地址；后台缓存用来跟随当前码率。
    /// 代理方法内部加锁，可从主线程安全读取。
    QUrl lastHlsVideoPlaylistUrl() const;
    QUrl lastHlsAudioPlaylistUrl() const;

signals:
    void currentItemChanged(const MediaItem &item, int row);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void positionChanged(qint64 position);
    void durationChanged(qint64 duration);
    void modeChanged(PlaybackController::Mode mode);
    void volumePercentChanged(int percent);
    void errorOccurred(const QString &message);
    /// 视频进度写回了播放列表条目，列表控制器据此立即持久化列表。
    void currentItemPositionUpdated(int row);

private:
    void playRowInternal(int row, bool autoAdvance);
    int rowCount() const;
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void updateLoops();
    /// 切换视频/停止/退出前写入当前视频位置；音乐不参与。
    void saveCurrentVideoPosition();
    /// 新媒体加载完成后恢复上次保存的位置，只对同一视频生效。
    void applyPendingResume();
    void clearPendingResume();
    void schedulePendingResume(int delayMs);

    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    StreamProxy *m_proxy;
    QThread *m_proxyThread;
    bool m_proxyReady = false;
    QPointer<PlaylistModel> m_playlist;
    std::function<MediaItem(const MediaItem &)> m_itemResolver;
    int m_row = -1;
    Mode m_mode = Mode::ListLoop;
    int m_volume = 60;
    /// 代理发现上游不是音视频时记下的原因，用来替代播放器含糊的报错
    QString m_unavailableReason;
    int m_consecutiveErrors = 0;
    /// 当前这一轮播放是否已经真正开始出声/出画面。
    /// 启动阶段的连续失败可以自动跳过；已经播到中途的错误不能偷偷切歌。
    bool m_playAttemptStarted = false;
    /// 播放器最后一次上报的播放位置。EndOfMedia 时 position 可能已被清零，
    /// 需要靠它判断“真的播完”还是“源在中间被截断”。
    qint64 m_lastKnownPosition = 0;
    /// 同一次播放里已经尝试从提前结束处续播的次数，避免坏源无限重试。
    int m_earlyEndRecoveries = 0;
    /// 上一次向代理提交正常播放预取的位置，避免每个 positionChanged 都重复排队
    qint64 m_lastPrefetchPosition = -1;
    /// 待恢复的视频位置；-1 表示没有待恢复任务
    qint64 m_pendingResumeMs = -1;
    QString m_pendingResumeIdentity;
    /// 已下发 seek 的次数；用于区分“新 source 尚未定位”与“已请求过定位”。
    int m_resumeAttemptCount = 0;
    /// 等待播放器真正可 seek 的定时器；不能用固定次数，否则 HLS/代理源
    /// 在 seekable 变 true 之前就会把次数耗尽。
    QTimer *m_resumeTimer = nullptr;
    QElapsedTimer m_resumeClock;
    /// 媒体是否已经加载到可定位状态。未就绪时只记最后一次目标，
    /// 避免源站还在拉首片时连续 setPosition 打断预取连接。
    bool m_mediaReady = false;
    /// setSource() 可能同步触发 Loaded/Buffered；此时还没调用 play()，
    /// 提前 seek 会被随后的 play() 重置回 0，必须等播放请求真正发出。
    bool m_sourcePlayRequested = false;
    qint64 m_deferredSeekMs = -1;
};

#endif // PLAYBACKCONTROLLER_H
