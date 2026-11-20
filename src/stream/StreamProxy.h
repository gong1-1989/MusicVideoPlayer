#ifndef STREAMPROXY_H
#define STREAMPROXY_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <QUrl>

class HttpClient;
class QTcpServer;
class QTcpSocket;
class QNetworkReply;

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

    /// 登记一个上游地址，返回给 QMediaPlayer 使用的本地 URL。
    /// headers 是该源必需的请求头（Referer、Cookie 等），代理在拉流时补上。
    QUrl proxyUrlFor(const QUrl &upstream,
                     const QHash<QString, QString> &headers = QHash<QString, QString>());

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
    struct CacheEntry;

    void handleClient(QTcpSocket *client, const QByteArray &payload);
    void attachUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream);
    void attachPlaylist(QTcpSocket *client, QNetworkReply *reply, const QUrl &playlistUrl,
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
    void dropCacheEntry(const QString &key);

    QByteArray buildCacheHead(const CacheEntry &entry, int status, qint64 start, qint64 end) const;
    static bool parseRange(const QString &value, qint64 total, qint64 *start, qint64 *end);

    void replyError(QTcpSocket *client, int status, const QString &reason);

    /// 把 m3u8 里的相对/绝对分片、子播放列表、key、init 段改写成代理地址，
    /// 否则绝对地址会绕过代理，丢掉 Referer/Cookie，相对地址则会丢 token。
    QByteArray rewritePlaylist(const QByteArray &body, const QUrl &playlistUrl,
                               const QHash<QString, QString> &headers);
    void pruneTokens();

    QByteArray buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const;
    static Request parseRequest(const QByteArray &payload);
    static QByteArray guessContentType(const QUrl &upstream);

    struct Upstream
    {
        QUrl url;
        QHash<QString, QString> headers;
        qint64 createdAtMs = 0;
    };

    HttpClient *m_http;
    QTcpServer *m_server;
    QHash<QString, Upstream> m_tokens;
    QHash<QString, QSharedPointer<CacheEntry>> m_cache;
};

#endif // STREAMPROXY_H
