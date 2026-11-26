#ifndef PLAYBACKCONTROLLER_H
#define PLAYBACKCONTROLLER_H

#include "core/PlaylistModel.h"
#include "media/MediaItem.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>

class QAudioOutput;
class QThread;
class QTimer;
class StreamProxy;
class MediaPageResolver;

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

    /// 影视站配置源经常把网页播放页当成“直链”返回；播放前用这个解析器
    /// 把页面里的 m3u8/MP4 取出来。为空时保持原有行为。
    void setPageMediaResolver(MediaPageResolver *resolver);

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

    /// 当前播放速率；默认 1.0。长按快进临时切 2×，松手恢复。
    qreal playbackRate() const;
    /// 设置播放速率；钳制在 0.25~4.0，避免异常值破坏音频输出。
    void setPlaybackRate(qreal rate);

    /// 计算相对跳转目标：先把基准位置夹进媒体范围，再加偏移并夹到总时长。
    /// 纯函数供 UI 与测试复用；总时长未知时原样返回基准位置。
    static qint64 steppedSeekTarget(qint64 position, qint64 duration, qint64 deltaMs);

    qint64 position() const;
    qint64 duration() const;

    QMediaPlayer *mediaPlayer() const { return m_player; }

    /// 播放器最近打开的视频/音频媒体列列表地址；后台缓存用来跟随当前码率。
    /// 代理方法内部加锁，可从主线程安全读取。
    QUrl lastHlsVideoPlaylistUrl() const;
    QUrl lastHlsAudioPlaylistUrl() const;

    /// 当前这一轮播放实际使用的上游地址（不是代理地址）。启动失败换过
    /// 备用线路时，后台缓存和下载必须跟随这里，不能再回到主线路。
    QUrl activeUpstreamUrl() const { return m_activeUpstreamUrl; }

    /// 后台 HLS 整段缓存把单个分片请求交给同一条流转代理，与播放共用分片缓存。
    /// 返回空表示代理尚未就绪，调用方应回退到直连；必须在主线程调用。
    QUrl proxyUrlForHlsCache(const QUrl &upstream,
                             const QHash<QString, QString> &headers,
                             const QString &cachePath, const QString &cacheIdentity,
                             const QUrl &sourceOrigin) const;

    /// 播放区域码率栏需要的快照：可用变体、用户锁定项和实际播放项。
    /// currentIndex/selectedIndex 为 -1 时分别表示“自动选择”和“尚未解析到”。
    struct BitrateOption
    {
        int index = -1;
        int bandwidth = 0;
        QString resolution;
        QString codecs;
    };
    struct BitrateSnapshot
    {
        QList<BitrateOption> options;
        int selectedIndex = -1;
        int currentIndex = -1;
        bool isVideo = false;
    };
    BitrateSnapshot bitrateSnapshot() const;

    /// 用户选择码率：先保存当前进度，再让代理锁定新变体并重载同一视频。
    /// index 为 -1 表示恢复自动码率；切换失败时保留原播放状态。
    void selectBitrate(int index);

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
    /// 统一播放入口：自动切集与用户点播都先解析网页播放页，再决定是否
    /// 同步进入播放器；autoAdvance 只影响连续失败计数，不影响解析。
    void startPlayback(int row, bool autoAdvance);
    void playRowInternal(int row, bool autoAdvance);
    /// 播放前检查网页播放页并异步解析；返回 true 表示本轮播放已交给解析回调。
    bool tryResolvePageMedia(int row, int serial, bool autoAdvance);
    /// 启动阶段失败时切到下一条备用线路；没有备用或已经出声后返回 false。
    bool tryNextFallback();
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
    MediaPageResolver *m_pageResolver = nullptr;
    /// 每次 playRow 递增；异步解析返回时用它丢弃已经过期的点击。
    int m_pageResolveSerial = 0;
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
    /// 用户切换码率时，本条重载必须强行走网络代理，不能因为整片缓存已经
    /// 落盘就继续播放固定码率的本地 MP4；只对紧接着的一次 playRowInternal 生效。
    bool m_forceStreamOnce = false;
    /// 当前条目的备用线路队列。只在启动阶段失败时消费；已经播出声后
    /// 网络抖动/解码失败不换线，避免用户看到“播到一半突然换片源”。
    QVector<QUrl> m_fallbackUrls;
    QString m_fallbackIdentity;
    /// tryNextFallback() 选中的下一条地址，由紧接着的 playRowInternal() 消费。
    QUrl m_forcedNextUrl;
    /// 当前实际送入播放/代理的上游地址；换备用线路时同步更新。
    QUrl m_activeUpstreamUrl;
};

#endif // PLAYBACKCONTROLLER_H
