#ifndef STREAMPROXY_H
#define STREAMPROXY_H

#include "stream/HlsPlaylist.h"
#include "stream/StreamCacheStore.h"
#include "stream/StreamRange.h"
#include "stream/StreamTokenRegistry.h"

#include <QByteArray>
#include <QHash>
#include <QList>
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
    int port() const;

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

    /// 关闭监听、终止所有上游请求并清空缓存。
    /// 只应在代理所属线程调用；PlaybackController 用阻塞队列调用它。
    void shutdown();

    /// 登记一个上游地址，返回给 QMediaPlayer 使用的本地 URL。
    /// headers 是该源必需的请求头（Referer、Cookie 等），代理在拉流时补上。
    QUrl proxyUrlFor(const QUrl &upstream,
                     const QHash<QString, QString> &headers = QHash<QString, QString>());

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
                              const QString &cacheKey);
    /// HLS 分片请求命中时，用多路 Range 并发拉取并在缓存里按偏移拼接。
    /// 只对已知媒体分片、`bytes=0-` 且总长受限的请求启用。
    bool tryStartRangeDownload(QTcpSocket *client, const QUrl &upstream,
                               const QHash<QString, QString> &headers,
                               const Request &request, bool hasRange,
                               const QString &cacheKey);
    bool isHlsSegmentUrl(const QUrl &url) const;
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
                        const QHash<QString, QString> &headers);
    void flushUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);

    /// 命中缓存时接管该客户端并返回 true；条目本身无效时返回 false，
    /// 让调用方回退到上游透传。无法用单段缓存表达的 Range 会在缓存内
    /// 退化成 200 全量，不再回到上游。
    bool serveFromCache(QTcpSocket *client, const QSharedPointer<CacheEntry> &entry,
                        const QString &cacheKey, const QString &rangeHeader, bool hasRange);
    void pumpCache(const QString &key, const QSharedPointer<CacheEntry> &entry);
    void readIntoCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                       QNetworkReply *reply);
    void finishCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                     QNetworkReply *reply);
    void pruneCache(const QString &keepKey = QString());
    /// 缓存是否还能容纳新条目：先按空闲/失败条目淘汰，再同时检查条数和总字节。
    /// 全部条目都在服务客户端时无法淘汰，此时返回 false，调用方应放弃缓存、直接透传，
    /// 避免缓存总量突破 256 MB 上限。
    bool cacheHasRoom(qint64 bytes, const QString &keepKey);
    void dropCacheEntry(const QString &key);
    /// 先断开 reply 到代理的所有信号再 abort：QNetworkReply::abort() 可能同步
    /// 发出 finished，回调会重入正在清理的 rangeReplies/m_cache 列表。
    void abortReplySilently(QNetworkReply *reply);

    QByteArray buildCacheHead(const CacheEntry &entry, int status, qint64 start, qint64 end) const;
    static QString cacheKeyFor(const QUrl &upstream, const QHash<QString, QString> &headers);

    /// 解析原始 m3u8，记录每个媒体分片的起始时间和地址；只处理完整分片播放列表。
    void rememberPlaylist(const QByteArray &body, const QUrl &playlistUrl,
                          const QHash<QString, QString> &headers);
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
                               const QHash<QString, QString> &headers, QString *error = nullptr);

    QByteArray buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const;
    static Request parseRequest(const QByteArray &payload);
    static QByteArray guessContentType(const QUrl &upstream);

    HttpClient *m_http;
    QTcpServer *m_server;
    StreamTokenRegistry m_tokens;
    StreamCacheStore m_cache;
    QHash<QString, QSharedPointer<PlaylistInfo>> m_playlists;
    QString m_lastPlaylistKey;

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

    std::atomic_bool m_listening{false};
    std::atomic<int> m_port{0};
};

#endif // STREAMPROXY_H
