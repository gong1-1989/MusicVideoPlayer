#ifndef HLSCACHEBUILDER_H
#define HLSCACHEBUILDER_H

#include "cache/HlsCacheManifest.h"
#include "media/MediaItem.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>

class DownloadTask;
class HttpClient;
class MediaRemux;
class QNetworkReply;
class QTimer;

/// 播放中的 HLS 影视后台整段缓存。
///
/// 只跟随当前播放条目，最终产物仍是 `media-cache/<条目名>/media`
/// （正文无后缀，旁车 JSON 记录 `.mp4`）。两条下载路径：
/// - 普通 `.ts/.m4s` 分片：沿用 DownloadTask 顺序下载并合并；
/// - 单文件 `EXT-X-BYTERANGE`：整段媒体本身就是一个 MP4，直接按 Range 下载，
///   不再发一百多个分片请求。
/// 主列表带独立音轨时，同时缓存对应音轨并在转封装时合并，避免无声 MP4。
/// 播放列表带 AES-128 时，子任务会自动取 KEY 解密；SAMPLE-AES 等仍不生成缓存。
class HlsCacheBuilder : public QObject
{
    Q_OBJECT

public:
    using UrlResolver = std::function<QUrl()>;

    explicit HlsCacheBuilder(HttpClient *http, QObject *parent = nullptr);
    ~HlsCacheBuilder() override;

    /// 后台缓存选码率时查询播放器最近实际打开的视频/音频播放列表。
    /// 不设置时回退到主列表里的第一个码率，而不是盲选最高码率。
    void setPlaybackTrackResolvers(UrlResolver video, UrlResolver audio);

    /// 开始缓存当前条目；非在线/非影视/非 HLS/已有完整缓存时直接返回 false。
    bool start(const MediaItem &item);
    /// 切换播放条目或退出：中止任务并清掉未完成的分片/轨道，保留已完成正文。
    void cancel();
    /// 删除播放列表条目：中止并移除这个条目的未完成与最终正文。
    void cancelAndRemove();
    bool isActive() const { return m_active; }
    MediaItem currentItem() const { return m_item; }

signals:
    void completed(const MediaItem &item, const QString &cachePath);
    void failed(const MediaItem &item, const QString &message);
    /// 后台缓存进度，供状态栏展示；percent 为 -1 表示总长未知。
    void progressChanged(const MediaItem &item, int percent, const QString &detail);

private:
    enum class Stage {
        Idle,
        FetchingMaster,
        WaitingVariant,
        FetchingVideoPlaylist,
        FetchingAudioPlaylist,
        Downloading,
        Remuxing
    };

    using PlaylistCallback =
        std::function<void(const QByteArray &body, const QUrl &finalUrl, const QString &error)>;

    void fetchPlaylist(const QUrl &url, const PlaylistCallback &callback);
    void onMasterReady(const QByteArray &body, const QUrl &finalUrl, const QString &error);
    void beginVariantSelection();
    void onVariantTick();
    void selectVariant(const QUrl &videoUrl, const QString &audioGroup);
    void onVideoPlaylistReady(const QByteArray &body, const QUrl &finalUrl,
                              const QString &error);
    void onAudioPlaylistReady(const QByteArray &body, const QUrl &finalUrl,
                              const QString &error);
    void startDownloads();
    bool startTrack(bool video, const QUrl &source, const QString &targetPath);
    void onTrackProgress(bool video, qint64 received, qint64 total);
    void onTrackFinished(bool video, const QString &path);
    void onTrackFailed(bool video, const QString &message);
    void maybeRemux();
    void onRemuxFinished(bool success, const QString &outputPath, const QString &error);
    void finishFailed(const QString &message);
    void abortPlaylistFetch();
    void cancelTasks();
    void cleanupTrackFiles();
    void cleanupPartial(bool removeBody);
    QString audioGroupForVariant(const QUrl &videoUrl) const;
    void emitProgress();

    HttpClient *m_http = nullptr;
    MediaItem m_item;
    QString m_bodyPath;
    bool m_active = false;
    Stage m_stage = Stage::Idle;
    qint64 m_generation = 0;

    UrlResolver m_videoResolver;
    UrlResolver m_audioResolver;

    QUrl m_sourceUrl;
    QUrl m_originUrl;
    QUrl m_videoPlaylistUrl;
    QUrl m_audioPlaylistUrl;
    QUrl m_videoSource;
    QUrl m_audioSource;
    QUrl m_masterBaseUrl;
    bool m_videoDirect = false;
    bool m_audioDirect = false;
    bool m_hasAudio = false;
    HlsCacheManifest::Master m_master;
    HlsCacheManifest::MediaTrack m_videoTrack;
    HlsCacheManifest::MediaTrack m_audioTrack;

    QPointer<QNetworkReply> m_playlistReply;
    QTimer *m_variantTimer = nullptr;
    qint64 m_variantStartedMs = 0;
    qint64 m_variantStableSinceMs = 0;
    QUrl m_variantCandidate;

    QPointer<DownloadTask> m_videoTask;
    QPointer<DownloadTask> m_audioTask;
    QString m_videoPath;
    QString m_audioPath;
    bool m_videoDone = false;
    bool m_audioDone = false;
    int m_videoPct = -1;
    int m_audioPct = -1;
    /// 双轨总进度按字节加权，避免短视频轨先下完就把整体拉到 50% 的假象。
    qint64 m_videoReceived = 0;
    qint64 m_videoTotal = 0;
    qint64 m_audioReceived = 0;
    qint64 m_audioTotal = 0;
    int m_lastOverallPct = -1;
    qint64 m_lastProgressMs = 0;

    QPointer<MediaRemux> m_remux;
};

#endif // HLSCACHEBUILDER_H
