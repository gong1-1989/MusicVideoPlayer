#ifndef STREAMPROXY_H
#define STREAMPROXY_H

#include "stream/HlsPlaylist.h"
#include "stream/StreamCacheStore.h"
#include "stream/StreamRange.h"
#include "stream/StreamTokenRegistry.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QUrl>

#include <atomic>

class HttpClient;
class QTcpServer;
class QTcpSocket;
class QNetworkReply;
class QTimer;

/// 本地回环 HTTP 代理。
///
/// QMediaPlayer 只能拿到一个 URL，无法附加 Referer/Cookie，因此由本代理
/// 带着统一请求头去拉上游流，再把状态码、Content-Type、Content-Range 和
/// 字节原样透传给播放器。只监听 127.0.0.1，URL 里带随机 token，
/// 局域网内其他机器访问不到。
class StreamProxy : public QObject
{
    Q_OBJECT

public:
    explicit StreamProxy(QObject *parent = nullptr);
    ~StreamProxy() override;

    bool start();
    bool isListening() const;

    /// 诊断用快照：缓存条目、计费字节（含尚未合并的 Range 分段）和预取占用。
    /// 回归测试用它确认缓存淘汰不会漏掉 rangeParts 或遗留预取槽位。
    struct CacheStats
    {
        int entries = 0;
        qint64 bytes = 0;
        int activePrefetches = 0;
        int rangePrefetches = 0;
    };
    CacheStats cacheStats() const;

    /// 播放器最近请求过的视频/音频媒体播放列表地址。
    /// 用于让后台 HLS 缓存跟随当前播放码率，而不是盲选最高码率。
    /// 读取时加锁，可从主线程调用。
    QUrl lastPlaybackVideoPlaylist() const;
    QUrl lastPlaybackAudioPlaylist() const;

    /// 当前 HLS 主列表的码率状态，供播放区域码率栏显示。
    /// variants 来自最近一次成功解析的 master；currentIndex 根据播放器
    /// 最近实际请求的视频子列表反查，selectedIndex 为用户锁定项（-1 自动）。
    struct VariantState
    {
        QUrl masterUrl;
        QList<HlsPlaylist::Variant> variants;
        QUrl currentVideoPlaylist;
        int selectedIndex = -1;
        int currentIndex = -1;
    };
    /// 必须在代理线程调用；PlaybackController 用阻塞队列读取快照。
    VariantState variantState() const;

    /// 锁定指定码率变体；-1 表示恢复自动选择（master 不裁剪）。
    /// 必须在代理线程调用，下一次 master 请求生效。
    void setVariantIndex(int index);

    /// 切换影视条目时清空上一部的主列表变体，避免码率栏显示旧数据。
    void clearVariantState();

    /// 关闭监听、终止所有上游请求并清空缓存。
    /// 只应在代理所属线程调用；PlaybackController 用阻塞队列调用它。
    void shutdown();

    /// 释放指定磁盘缓存文件对应的流转条目句柄（不删除文件本身）。
    /// 删除播放列表条目时调用，避免 QMediaPlayer 已停止、代理仍持有文件句柄
    /// 导致 MediaCache::removeForItem 连续重试后仍删不掉。
    /// 必须在代理所属线程调用。
    void releaseCacheFile(const QString &path);

    /// 登记一个上游地址，返回给 QMediaPlayer 使用的本地 URL。
    /// headers 是该源必需的请求头（Referer、Cookie 等），代理在拉流时补上。
    /// cachePath 非空且是可按字节定位的直链时，代理会把整段正文落到该文件，
    /// 下次播放从已有字节继续请求；HLS 播放列表/分片忽略该参数。
    QUrl proxyUrlFor(const QUrl &upstream,
                     const QHash<QString, QString> &headers = QHash<QString, QString>(),
                     const QString &cachePath = QString(),
                     const QString &cacheIdentity = QString(),
                     const QUrl &sourceOrigin = QUrl());

    /// 深拖动前调用：按播放列表里记录的分片时长，把目标时间映射成分片序号，
    /// 在 FFmpeg 真正 seek 之前先把目标附近的分片排进预取队列。
    /// 必须在代理线程调用；只登记任务，不会阻塞等待下载。
    void prefetchForSeek(const QUrl &playlistUrl, const QHash<QString, QString> &headers,
                         qint64 targetMs);

    /// 正常播放时调用：按当前位置向后预取少量分片，避免下一片才开始拉网络。
    /// 必须在代理线程调用。
    void prefetchForPosition(const QUrl &playlistUrl, const QHash<QString, QString> &headers,
                             qint64 positionMs);

signals:
    /// 上游 302 落到 /404 或返回的不是音视频，通常是版权/付费/登录限制。
    void sourceUnavailable(const QUrl &upstream, const QString &contentType);

private slots:
    void onNewConnection();

private:
    struct Request
    {
        QString method;
        QString path;
        QString range;
        bool isValid = false;
    };

    /// 一条上游 200 响应在本地内存里的副本。
    /// 上游忽略 Range 时，拖动请求由这里的缓冲区切片返回 206。
    using CacheEntry = StreamCacheEntry;

    using PlaylistInfo = HlsPlaylist::Info;

    struct PrefetchJob
    {
        QUrl upstream;
        QHash<QString, QString> headers;
        QString cacheKey;
        qint64 generation = 0;
        int index = -1;
    };

    void handleClient(QTcpSocket *client, const QByteArray &payload);
    void startUpstreamRequest(QTcpSocket *client, const QUrl &upstream,
                              const QHash<QString, QString> &headers,
                              const Request &request, bool hasRange,
                              const QString &cacheKey,
                              const QString &cachePath = QString(),
                              const QString &cacheIdentity = QString(),
                              const QUrl &sourceOrigin = QUrl());
    /// HLS 分片请求命中时，用多路 Range 并发拉取并在缓存里按偏移拼接。
    /// 只对已知媒体分片、`bytes=0-` 且总长受限的请求启用。
    bool tryStartRangeDownload(QTcpSocket *client, const QUrl &upstream,
                               const QHash<QString, QString> &headers,
                               const Request &request, bool hasRange,
                               const QString &cacheKey);
    bool isHlsSegmentUrl(const QUrl &url) const;
    /// 同一分片 URL 在清单里重复出现，说明是 EXT-X-BYTERANGE 单文件 HLS。
    bool isSingleFileHlsSegment(const QUrl &url) const;
    void startRangePart(const QSharedPointer<CacheEntry> &entry, const QString &cacheKey,
                        const QUrl &upstream, const QHash<QString, QString> &headers,
                        qint64 start, qint64 end, bool probe, QTcpSocket *client,
                        const Request &request);
    void onRangePartReply(const QSharedPointer<CacheEntry> &entry, const QString &cacheKey,
                          const QUrl &upstream, const QHash<QString, QString> &headers,
                          QNetworkReply *reply, qint64 requestedStart, qint64 requestedEnd,
                          bool probe, QTcpSocket *client, const Request &request);
    void readRangePartIntoCache(const QSharedPointer<CacheEntry> &entry,
                                QNetworkReply *reply, qint64 start, qint64 end);
    void finishRangePart(const QString &key, const QSharedPointer<CacheEntry> &entry,
                         QNetworkReply *reply, qint64 start, qint64 end,
                         const QUrl &upstream, const QHash<QString, QString> &headers);
    void mergeRangeParts(const QString &key, const QSharedPointer<CacheEntry> &entry);
    void failRangeEntry(const QString &key, const QSharedPointer<CacheEntry> &entry,
                        const QString &reason);
    void attachUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);
    void attachPlaylist(QTcpSocket *client, QNetworkReply *reply,
                        const QHash<QString, QString> &headers,
                        const QUrl &sourceOrigin);
    void flushUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);

    /// 命中缓存时接管该客户端并返回 true；条目本身无效时返回 false，
    /// 让调用方回退到上游透传。无法用单段缓存表达的 Range 会在缓存内
    /// 退化成 200 全量，不再回到上游。
    bool serveFromCache(QTcpSocket *client, const QSharedPointer<CacheEntry> &entry,
                        const QString &cacheKey, const QString &rangeHeader, bool hasRange);
    void pumpCache(const QString &key, const QSharedPointer<CacheEntry> &entry);
    void finishCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                     QNetworkReply *reply, const QUrl &upstream = QUrl(),
                     const QHash<QString, QString> &headers = QHash<QString, QString>());
    void readIntoCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                       QNetworkReply *reply);
    /// 缓存正文可能是内存 QByteArray，也可能是整段落盘的文件。
    /// 统一从这里读写，避免调用方混用 data.size()/data.constData()。
    qint64 cacheDataSize(const CacheEntry &entry) const;
    qint64 cacheSlice(CacheEntry &entry, qint64 offset, qint64 length,
                      QByteArray *out) const;
    bool appendCacheData(CacheEntry &entry, const QByteArray &chunk);
    /// 打开（必要时从头截断）条目文件，返回实际可用的已有字节数；失败返回 -1。
    qint64 openCacheFile(CacheEntry &entry, const QString &path, qint64 existingBytes);
    void closeCacheFile(CacheEntry &entry);
    void persistCacheState(CacheEntry &entry);
    /// 单路上游在总长度未收齐时断流（常见于 CDN 约 2 分钟重置连接），
    /// 从缓存末尾续发 Range，避免播放器直接收到 Demuxing failed。
    void resumeCacheUpstream(const QString &key, const QSharedPointer<CacheEntry> &entry,
                             const QUrl &upstream, const QHash<QString, QString> &headers);
    void pruneCache(const QString &keepKey = QString());
    /// 缓存是否还能容纳新条目：先按空闲/失败条目淘汰，再同时检查条数和总字节。
    /// 内存缓存和磁盘整段缓存分属两个配额池，磁盘条目不能把 256MB 内存上限吃掉。
    bool cacheHasRoom(qint64 bytes, const QString &keepKey, bool diskBacked = false);
    void dropCacheEntry(const QString &key);
    /// 先断开 reply 到代理的所有信号再 abort：QNetworkReply::abort() 可能同步
    /// 发出 finished，回调会重入正在清理的 rangeReplies/m_cache 列表。
    void abortReplySilently(QNetworkReply *reply);

    QByteArray buildCacheHead(const CacheEntry &entry, int status, qint64 start, qint64 end) const;
    static QString cacheKeyFor(const QUrl &upstream, const QHash<QString, QString> &headers);

    /// 解析原始 m3u8，记录每个媒体分片的起始时间和地址；只处理完整分片播放列表。
    void rememberPlaylist(const QByteArray &body, const QUrl &playlistUrl,
                          const QHash<QString, QString> &headers);
    /// master 的变体必须在改写前记录：改写后只剩一个变体，码率栏会丢选项。
    void rememberMasterVariants(const QByteArray &body, const QUrl &playlistUrl);
    const PlaylistInfo *playlistFor(const QUrl &playlistUrl,
                                    const QHash<QString, QString> &headers) const;

    void enqueuePrefetch(const QUrl &playlistUrl, const QHash<QString, QString> &headers,
                         int firstIndex, int lastIndex, qint64 generation);
    void startNextPrefetch();
    void cancelPrefetch();
    void finishPrefetch(const QString &cacheKey, QNetworkReply *reply, qint64 generation);
    void onPrefetchReply(QNetworkReply *reply, const PrefetchJob &job);
    bool startRangePrefetch(const PrefetchJob &job);
    void finishRangePrefetch(const QString &cacheKey,
                             const QSharedPointer<CacheEntry> &entry);

    void replyError(QTcpSocket *client, int status, const QString &reason);

    /// 把 m3u8 里的相对/绝对分片、子播放列表、key、init 段改写成代理地址，
    /// 否则绝对地址会绕过代理，丢掉 Referer/Cookie，相对地址则会丢 token。
    QByteArray rewritePlaylist(const QByteArray &body, const QUrl &playlistUrl,
                               const QHash<QString, QString> &headers, QString *error = nullptr,
                               const QUrl &sourceOrigin = QUrl());

    QByteArray buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const;
    static Request parseRequest(const QByteArray &payload);
    static QByteArray guessContentType(const QUrl &upstream);

    HttpClient *m_http;
    QTcpServer *m_server;
    StreamTokenRegistry m_tokens;
    StreamCacheStore m_cache;
    QHash<QString, QSharedPointer<PlaylistInfo>> m_playlists;
    QString m_lastPlaylistKey;

    /// 当前 master 的完整变体列表。m_variantMasterIdentity 用协议/host/端口/path
    /// 标识同一份主列表，避免 token 查询串变化时误清空用户选择。
    QUrl m_variantMasterUrl;
    QString m_variantMasterIdentity;
    QList<HlsPlaylist::Variant> m_masterVariants;
    int m_selectedVariantIndex = -1;

    QList<PrefetchJob> m_prefetchQueue;
    QHash<QString, QNetworkReply *> m_prefetchReplies;
    QHash<QString, qint64> m_prefetchStartedMs;
    QSet<QString> m_rangePrefetchKeys;
    QTimer *m_seekPrefetchTimer = nullptr;
    QString m_prefetchPlaylistKey;
    qint64 m_prefetchGeneration = 0;
    qint64 m_seekTargetMs = -1;
    int m_prefetchActive = 0;
    int m_prefetchConcurrency = 2;
    bool m_seekPrefetchActive = false;
    qint64 m_prefetchDisabledUntilMs = 0;

    mutable QMutex m_playbackPlaylistMutex;
    QUrl m_lastPlaybackVideoPlaylist;
    QUrl m_lastPlaybackAudioPlaylist;

    std::atomic_bool m_listening{false};
};

#endif // STREAMPROXY_H
