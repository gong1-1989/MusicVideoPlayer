#ifndef HLSCACHEBUILDER_H
#define HLSCACHEBUILDER_H

#include "cache/HlsCacheManifest.h"
#include "cache/HlsPreloadIndex.h"
#include "media/MediaItem.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>

class DownloadTask;
class HttpClient;
class HlsPreloadMigrator;
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
    /// 把一个真实 HLS 分片地址换成本机流转代理地址；空返回值表示回退直连。
    using ProxyUrlResolver = std::function<QUrl(
        const QUrl &url, const QHash<QString, QString> &headers,
        const QString &cachePath, const QString &cacheIdentity,
        const QUrl &sourceOrigin)>;

    explicit HlsCacheBuilder(HttpClient *http, QObject *parent = nullptr);
    ~HlsCacheBuilder() override;

    /// 后台缓存选码率时查询播放器最近实际打开的视频/音频播放列表。
    /// 不设置时回退到主列表里的第一个码率，而不是盲选最高码率。
    void setPlaybackTrackResolvers(UrlResolver video, UrlResolver audio);

    /// 让后台整段缓存的 HLS 分片请求走同一条流转代理，和播放共享分片缓存。
    /// 只在播放代理已就绪时设置；未设置或返回空时自动回退为直连下载。
    void setProxyUrlResolver(ProxyUrlResolver resolver);

    /// 缓存完成后是否保留轨道文件与分片索引，直到切换条目/退出。
    /// 主窗口播放会话必须开启，避免 100% 瞬间当前播放重新回源；
    /// 无播放会话的独立缓存任务保持关闭，完成后立即清理中间文件。
    void setRetainCompletedTrackFiles(bool retain);

    /// 开始缓存当前条目；非在线/非影视/非 HLS/已有完整缓存时直接返回 false。
    /// sourceUrl 必须是播放器当前实际打开的上游地址；启动失败换过备用线路时
    /// 不能再用 preferredPlaybackUrl(item) 去缓存已经失败的主线路。
    bool start(const MediaItem &item, const QUrl &sourceUrl = QUrl());
    /// 退出程序：中止任务并保留已下载分片/续传点，下次播放继续缓存。
    void stopForExit();
    /// 用户主动取消（如删除条目）：中止任务并清掉未完成的分片/轨道。
    void cancel();
    /// 删除播放列表条目：中止并移除这个条目的未完成与最终正文。
    void cancelAndRemove();

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
    bool startTrack(bool video, const QUrl &source, const QString &basePath);
    /// 旧版缓存只有合并轨道文件、没有逐分片索引；在指纹一致且轨道未加密、
    /// 非单文件 BYTERANGE 时，先探测每片准确长度，再一次性生成索引。
    void maybeStartMigration(bool video, const QString &fingerprint);
    void onMigrationFinished(bool video, qint64 generation,
                             const QList<HlsPreloadEntry> &entries, const QString &error);
    void stopMigrations();
    void onTrackProgress(bool video, qint64 received, qint64 total);
    void onTrackFinished(bool video, const QString &path);
    void onTrackFailed(bool video, const QString &message);
    void maybeRemux();
    void onRemuxFinished(bool success, const QString &outputPath, const QString &error);
    void finishFailed(const QString &message);
    void abortPlaylistFetch();
    void stopTasksForExit();
    void cancelTasks();
    void cleanupTrackFiles();
    void cleanupTrackFilesAt(const QString &bodyPath, const QString &videoPath,
                             const QString &audioPath);
    /// 缓存完成后当前播放仍可能继续读分片索引；轨道文件和索引延迟到
    /// 切换条目/退出时再清理，避免“缓存 100% 的瞬间当前会话重新回源”。
    void retainCurrentTrackFiles();
    void cleanupRetainedTracks();
    void cleanupPartial(bool removeBody, bool removeTracks = true);
    /// 清空当前条目的会话字段；调用前必须先完成信号/任务/文件清理，
    /// 否则后续日志和完成信号会拿不到条目信息。
    void resetSessionState();
    QString audioGroupForVariant(const QUrl &videoUrl) const;
    void emitProgress();

    /// 单条轨道的续传现场；与 `.mvpdownload` 分开保存，退出/崩溃后都能恢复。
    struct TrackResume
    {
        bool valid = false;
        bool completed = false;
        /// 状态文件可解析，但 targetPath 指向了别的轨道/目录；这类状态
        /// 不能续传，并且要在启动时连同本轨道自己的旧文件一起清掉。
        bool rejectedTarget = false;
        QString targetPath;
        qint64 bytesWritten = 0;
        int segmentIndex = 0;
        int segmentCount = -1;
        QString fingerprint;
    };

    /// 固定按轨道基名保存（`.video/.audio`），避免扩展名变化后找不到状态。
    QString resumeStatePath(const QString &basePath) const;
    TrackResume loadTrackResume(const QString &basePath) const;
    /// 续传目标不可信时，清掉该轨道自己的旧成品/半成品/状态文件。
    /// 只处理 basePath 派生名，不碰 resume.targetPath，避免删到另一条有效轨道。
    void cleanupRejectedTrackFiles(const QString &basePath);
    void saveTrackResume(const QString &basePath, const TrackResume &resume);
    void removeTrackResume(const QString &basePath);

    HttpClient *m_http = nullptr;
    MediaItem m_item;
    QString m_bodyPath;
    bool m_active = false;
    Stage m_stage = Stage::Idle;
    qint64 m_generation = 0;

    UrlResolver m_videoResolver;
    UrlResolver m_audioResolver;
    ProxyUrlResolver m_proxyUrlResolver;
    bool m_retainCompletedTrackFiles = false;

    QUrl m_sourceUrl;
    QUrl m_originUrl;
    QUrl m_videoPlaylistUrl;
    QUrl m_audioPlaylistUrl;
    QUrl m_videoSource;
    QUrl m_audioSource;
    QUrl m_masterBaseUrl;
    QString m_identity;
    bool m_videoDirect = false;
    bool m_audioDirect = false;
    bool m_hasAudio = false;
    /// 分片自身带 EXT-X-BYTERANGE 时旧缓存的分片边界不能只按 URL 探测，
    /// 这类轨道安全起见不迁移。
    bool m_videoHasByteRange = false;
    bool m_audioHasByteRange = false;
    HlsCacheManifest::Master m_master;
    HlsCacheManifest::MediaTrack m_videoTrack;
    HlsCacheManifest::MediaTrack m_audioTrack;
    QList<QUrl> m_videoSegments;
    QList<QUrl> m_audioSegments;

    QPointer<QNetworkReply> m_playlistReply;
    QTimer *m_variantTimer = nullptr;
    qint64 m_variantStartedMs = 0;
    qint64 m_variantStableSinceMs = 0;
    QUrl m_variantCandidate;

    QPointer<DownloadTask> m_videoTask;
    QPointer<DownloadTask> m_audioTask;
    QPointer<HlsPreloadMigrator> m_videoMigrator;
    QPointer<HlsPreloadMigrator> m_audioMigrator;
    QString m_videoPath;
    QString m_audioPath;
    TrackResume m_videoResume;
    TrackResume m_audioResume;
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

    /// 已生成 MP4 但当前播放会话仍可能需要读取的轨道/索引现场。
    QString m_retainedBasePath;
    QString m_retainedVideoPath;
    QString m_retainedAudioPath;
};

#endif // HLSCACHEBUILDER_H
