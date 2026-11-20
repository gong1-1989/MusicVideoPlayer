#include "stream/StreamProxy.h"

#include "net/HttpClient.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

Q_LOGGING_CATEGORY(lcProxy, "mvp.stream")

namespace {

const char *const kHeaderSent = "proxyHeadersSent";
const char *const kHandled = "proxyHandled";
const char *const kBuffer = "proxyRequestBuffer";
const char *const kMethod = "proxyMethod";
const char *const kCacheNext = "proxyCacheNext";
const char *const kCacheEnd = "proxyCacheEnd";
const char *const kCacheRequested = "proxyCacheRequested";

// 上游忽略 Range 时，把整段音视频留在内存里按需切片。
// 单条 128MB、总量 256MB，覆盖常见歌曲和短片，避免无上限吃内存。
constexpr qint64 kMaxCacheEntryBytes = 128LL * 1024 * 1024;
constexpr qint64 kMaxCacheTotalBytes = 256LL * 1024 * 1024;
constexpr int kMaxCacheEntries = 6;
constexpr int kCacheIdleMs = 30 * 60 * 1000;
constexpr int kRangeWaitMs = 20 * 1000;
constexpr qint64 kMaxSocketBacklog = 512LL * 1024;
constexpr qint64 kCacheWriteChunk = 256LL * 1024;

/// 网易云对无版权/付费歌曲不会返回错误码，而是 302 到 /404 再返回整页 HTML。
/// 把 HTML 当音频流喂给 FFmpeg 只会得到含糊的 “Could not open file”，
/// 所以这里先按最终 URL 和 Content-Type 拦一道。
bool looksLikeMedia(QNetworkReply *reply)
{
    if (reply->url().path().endsWith(QLatin1String("/404")))
        return false;

    // 有些源把 m3u8 以 text/plain 返回，不能因为类型就拦掉整个播放列表
    if (reply->url().path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return true;

    const QString type =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    if (type.isEmpty())
        return true; // 类型未知时不武断拒绝，交给播放器自己判断
    return type.startsWith(QLatin1String("audio/"))
        || type.startsWith(QLatin1String("video/"))
        || type.contains(QLatin1String("octet-stream"))
        || type.contains(QLatin1String("mpegurl"))
        || type.startsWith(QLatin1String("text/plain"));
}

bool isPlaylistResponse(QNetworkReply *reply)
{
    const QString type =
        reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    if (type.contains(QLatin1String("mpegurl")))
        return true;
    if (reply->url().path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return true;
    return type.isEmpty()
        && reply->url().toString().contains(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

QByteArray statusReason(int status)
{
    switch (status) {
    case 200:
        return QByteArrayLiteral("OK");
    case 206:
        return QByteArrayLiteral("Partial Content");
    case 301:
        return QByteArrayLiteral("Moved Permanently");
    case 302:
        return QByteArrayLiteral("Found");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 416:
        return QByteArrayLiteral("Range Not Satisfiable");
    case 500:
        return QByteArrayLiteral("Internal Server Error");
    case 502:
        return QByteArrayLiteral("Bad Gateway");
    default:
        return QByteArrayLiteral("Error");
    }
}

} // namespace

struct StreamProxy::CacheEntry
{
    QByteArray data;
    qint64 total = -1;
    bool complete = false;
    bool failed = false;
    QByteArray contentType;
    qint64 lastUsedMs = 0;
    QNetworkReply *reply = nullptr;
    QList<QPointer<QTcpSocket>> clients;
};

StreamProxy::StreamProxy(QObject *parent)
    : QObject(parent)
    , m_http(new HttpClient(this))
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &StreamProxy::onNewConnection);
}

StreamProxy::~StreamProxy()
{
    m_server->close();
    for (const auto &entry : std::as_const(m_cache)) {
        if (entry->reply != nullptr) {
            entry->reply->abort();
            entry->reply->deleteLater();
            entry->reply = nullptr;
        }
    }
    m_cache.clear();
}

bool StreamProxy::start()
{
    if (m_server->isListening())
        return true;

    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(lcProxy) << "流转服务监听失败：" << m_server->errorString();
        return false;
    }
    qCInfo(lcProxy) << "流转服务已启动，监听 127.0.0.1:" << m_server->serverPort();
    return true;
}

bool StreamProxy::isListening() const
{
    return m_server->isListening();
}

int StreamProxy::port() const
{
    return m_server->serverPort();
}

QUrl StreamProxy::proxyUrlFor(const QUrl &upstream, const QHash<QString, QString> &headers)
{
    if (!isListening() && !start())
        return QUrl();
    if (!upstream.isValid() || upstream.isEmpty())
        return QUrl();

    pruneTokens();

    const QString token = QString::number(QRandomGenerator::global()->generate64(), 16);
    Upstream entry;
    entry.url = upstream;
    entry.headers = headers;
    entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    m_tokens.insert(token, entry);
    // 末尾的斜杠很关键：m3u8 里的相对分片会按“目录”解析，
    // 少了它 /stream/<token>/000001.ts 会被播放器解析成 /stream/000001.ts。
    return QUrl(QStringLiteral("http://127.0.0.1:%1/stream/%2/")
                    .arg(m_server->serverPort())
                    .arg(token));
}

void StreamProxy::pruneTokens()
{
    if (m_tokens.size() < 512)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 expireBefore = now - 6LL * 60 * 60 * 1000;
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->createdAtMs > 0 && it->createdAtMs < expireBefore)
            it = m_tokens.erase(it);
        else
            ++it;
    }

    // 极端情况下（一个超长播放列表反复刷新）仍超过上限，丢最老的四分之一。
    if (m_tokens.size() > 4096) {
        QList<QPair<qint64, QString>> byAge;
        byAge.reserve(m_tokens.size());
        for (auto it = m_tokens.cbegin(); it != m_tokens.cend(); ++it)
            byAge.append({it->createdAtMs, it.key()});
        std::sort(byAge.begin(), byAge.end(),
                  [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                      return a.first < b.first;
                  });
        const int removeCount = m_tokens.size() / 4;
        for (int i = 0; i < removeCount && i < byAge.size(); ++i)
            m_tokens.remove(byAge.at(i).second);
    }
}

void StreamProxy::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *client = m_server->nextPendingConnection();
        client->setProperty(kHeaderSent, false);
        client->setProperty(kHandled, false);
        client->setProperty(kBuffer, QByteArray());
        client->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        connect(client, &QAbstractSocket::disconnected, client, &QObject::deleteLater);
        connect(client, &QAbstractSocket::readyRead, this, [this, client] {
            if (client->property(kHandled).toBool()) {
                client->readAll(); // 请求体不关心，丢弃避免缓冲增长
                return;
            }

            QByteArray buffer = client->property(kBuffer).toByteArray();
            buffer += client->readAll();
            if (!buffer.contains("\r\n\r\n")) {
                if (buffer.size() > 16 * 1024)
                    replyError(client, 400, QStringLiteral("Request header too large"));
                else
                    client->setProperty(kBuffer, buffer);
                return;
            }

            client->setProperty(kHandled, true);
            handleClient(client, buffer);
        });
    }
}

void StreamProxy::handleClient(QTcpSocket *client, const QByteArray &payload)
{
    const Request request = parseRequest(payload);
    if (!request.isValid) {
        replyError(client, 400, QStringLiteral("Bad Request"));
        return;
    }

    // path 里可能带查询串；token 只取 /stream/ 后的第一段，
    // 其余部分才是分片/子列表的相对路径。旧实现取最后一段，
    // 相对分片会变成 "000001.ts" 因而查不到 token。
    QString path = request.path;
    QString query;
    const int queryPos = path.indexOf(QLatin1Char('?'));
    if (queryPos >= 0) {
        query = path.mid(queryPos + 1);
        path.truncate(queryPos);
    }

    const QString prefix = QStringLiteral("/stream/");
    if (!path.startsWith(prefix)) {
        replyError(client, 404, QStringLiteral("Not Found"));
        return;
    }

    const QString rest = path.mid(prefix.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    const QString token = slash < 0 ? rest : rest.left(slash);
    const QString suffix = slash < 0 ? QString() : rest.mid(slash + 1);

    const Upstream entry = m_tokens.value(token);
    if (entry.url.isEmpty() || !entry.url.isValid()) {
        qCWarning(lcProxy) << "流转令牌未知：" << token;
        replyError(client, 404, QStringLiteral("Not Found"));
        return;
    }

    QUrl upstream = entry.url;
    if (!suffix.isEmpty()) {
        QUrl relative = QUrl::fromEncoded(suffix.toUtf8());
        if (!query.isEmpty())
            relative.setQuery(QUrl::fromPercentEncoding(query.toUtf8()));
        upstream = entry.url.resolved(relative);
    }
    if (upstream.isEmpty() || !upstream.isValid()) {
        replyError(client, 400, QStringLiteral("Bad stream path"));
        return;
    }

    client->setProperty(kMethod, request.method);
    const QByteArray range = request.range.toUtf8();
    qCDebug(lcProxy) << "流转请求" << request.method << "上游"
                     << HttpClient::redactUrl(upstream) << "分段" << request.range;

    const QHash<QString, QString> headers = entry.headers;
    const bool isHead = request.method == QLatin1String("HEAD");
    const bool hasRange = !request.range.isEmpty();

    // 缓存键必须同时包含地址和鉴权头，避免同一 CDN 地址在不同 Referer/Cookie
    // 下共用缓存，把 A 源的响应喂给 B 源。
    QStringList headerKeys = headers.keys();
    headerKeys.sort();
    QString cacheKey = upstream.toString(QUrl::FullyEncoded);
    for (const QString &key : std::as_const(headerKeys))
        cacheKey += QLatin1Char('\x1f') + key + QLatin1Char('\x1e') + headers.value(key);

    // 先查缓存：完整或已下载部分都能立刻回 206，拖动无需等上游。
    if (!isHead) {
        const QSharedPointer<CacheEntry> cached = m_cache.value(cacheKey);
        if (!cached.isNull() && cached->total >= 0 && !cached->failed) {
            if (serveFromCache(client, cached, cacheKey, request.range, hasRange)) {
                qCDebug(lcProxy) << "流转缓存命中" << HttpClient::redactUrl(upstream)
                                 << "已缓存" << cached->data.size() << "/" << cached->total;
                return;
            }
        }
    }

    // 由 HttpClient 手动跟随 302（含 https→http 降级），拿到可读的上游 reply。
    // 用 stream() 而不是 fetch()：响应头一到就能开始转发，不会先缓冲整段音视频。
    QPointer<QTcpSocket> guard(client);
    m_http->stream(upstream, range, headers,
                   [this, guard, upstream, headers, isHead, hasRange, cacheKey, request](QNetworkReply *reply) {
        if (guard.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        QTcpSocket *client = guard.data();
        if (reply == nullptr) {
            replyError(client, 502, QStringLiteral("Upstream redirect rejected"));
            return;
        }
        if (client->state() != QAbstractSocket::ConnectedState) {
            reply->abort();
            reply->deleteLater();
            return;
        }
        if (!looksLikeMedia(reply)) {
            const QString type =
                reply->header(QNetworkRequest::ContentTypeHeader).toString();
            qCWarning(lcProxy) << "上游返回的不是音视频：" << HttpClient::redactUrl(reply->url())
                              << type;
            emit sourceUnavailable(upstream, type);
            replyError(client, 404, QStringLiteral("no audio source"));
            reply->abort();
            reply->deleteLater();
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // HEAD/带 Range 的播放列表不做正文改写，避免破坏 206 语义；
        // 正常 GET 播放列表则把每个 URI 都映射到代理，绝对地址也不会绕过鉴权头。
        if (!isHead && isPlaylistResponse(reply)) {
            if (!hasRange) {
                attachPlaylist(client, reply, upstream, headers);
                return;
            }
        } else if (!isHead && status == 200) {
            // 只有“上游忽略 Range、返回完整 200”才值得缓存。上游已经回 206
            // 说明它自己支持分段，保持原样透传即可，不额外占内存。
            const QVariant lengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
            const qint64 total =
                lengthHeader.isValid() ? lengthHeader.toLongLong() : -1;
            if (total > 0 && total <= kMaxCacheEntryBytes) {
                const QSharedPointer<CacheEntry> existing = m_cache.value(cacheKey);
                if (!existing.isNull() && existing->total >= 0 && !existing->failed) {
                    if (serveFromCache(client, existing, cacheKey, request.range, hasRange)) {
                        reply->abort();
                        reply->deleteLater();
                        return;
                    }
                }

                auto entry = QSharedPointer<CacheEntry>::create();
                entry->total = total;
                entry->contentType =
                    reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
                entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
                entry->reply = reply;
                m_cache.insert(cacheKey, entry);
                qCInfo(lcProxy) << "流转缓存建立" << HttpClient::redactUrl(upstream)
                                << total << "字节";

                QPointer<QTcpSocket> clientGuard(client);
                connect(reply, &QNetworkReply::readyRead, this,
                        [this, cacheKey, entry, reply] {
                            readIntoCache(cacheKey, entry, reply);
                        });
                connect(reply, &QNetworkReply::finished, this,
                        [this, cacheKey, entry, reply] {
                            finishCache(cacheKey, entry, reply);
                        });
                readIntoCache(cacheKey, entry, reply);
                if (reply->isFinished())
                    finishCache(cacheKey, entry, reply);
                if (!clientGuard.isNull())
                    serveFromCache(clientGuard.data(), entry, cacheKey, request.range, hasRange);
                pruneCache(cacheKey);
                return;
            }

            if (total > kMaxCacheEntryBytes) {
                qCInfo(lcProxy) << "流转缓存跳过：文件过大" << HttpClient::redactUrl(upstream)
                                << total << "字节";
            }
        }

        attachUpstream(client, reply, upstream);
        flushUpstream(client, reply, upstream);
        if (reply->isFinished()) {
            client->disconnectFromHost();
            reply->deleteLater();
        }
    });
}

void StreamProxy::attachPlaylist(QTcpSocket *client, QNetworkReply *reply, const QUrl &playlistUrl,
                                 const QHash<QString, QString> &headers)
{
    QPointer<QTcpSocket> guard(client);

    auto finish = [this, guard, reply, playlistUrl, headers] {
        if (guard.isNull()) {
            reply->abort();
            reply->deleteLater();
            return;
        }

        QTcpSocket *socket = guard.data();
        if (!HttpClient::isHttpSuccess(reply)) {
            const QString message = HttpClient::describeError(reply);
            qCWarning(lcProxy) << "播放列表拉取失败：" << message;
            replyError(socket, 404, QStringLiteral("playlist unavailable"));
            reply->deleteLater();
            return;
        }

        const QByteArray body = reply->readAll();
        const QUrl finalUrl = reply->url();
        if (!body.contains("#EXTM3U")) {
            qCWarning(lcProxy) << "播放列表正文不是 m3u8：" << HttpClient::redactUrl(finalUrl);
            replyError(socket, 404, QStringLiteral("playlist unavailable"));
            reply->deleteLater();
            return;
        }
        QByteArray rewritten = rewritePlaylist(body, finalUrl, headers);

        QByteArray contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (contentType.isEmpty())
            contentType = QByteArrayLiteral("application/vnd.apple.mpegurl");

        QByteArray head = QByteArrayLiteral("HTTP/1.1 200 OK\r\n");
        head += QByteArrayLiteral("Content-Type: ");
        head += contentType;
        head += QByteArrayLiteral("\r\n");
        head += "Content-Length: " + QByteArray::number(rewritten.size()) + "\r\n";
        head += "Accept-Ranges: bytes\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "Connection: close\r\n\r\n";

        socket->write(head + rewritten);
        socket->disconnectFromHost();
        reply->deleteLater();
    };

    connect(reply, &QNetworkReply::finished, this, finish);
    if (reply->isFinished())
        finish();
}

QByteArray StreamProxy::rewritePlaylist(const QByteArray &body, const QUrl &playlistUrl,
                                        const QHash<QString, QString> &headers)
{
    const QString source = QString::fromUtf8(body);
    const bool crlf = source.contains(QLatin1String("\r\n"));
    QStringList lines = source.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    QUrl base = playlistUrl;
    QHash<QString, QString> tokenCache;

    auto proxyFor = [this, &base, &headers, &tokenCache](const QString &raw) -> QString {
        const QString uri = raw.trimmed();
        if (uri.isEmpty())
            return raw;

        const QUrl absolute = base.resolved(QUrl(uri));
        if (!absolute.isValid()
            || (absolute.scheme() != QLatin1String("http")
                && absolute.scheme() != QLatin1String("https"))) {
            return raw; // data:、skd: 等非 HTTP 资源原样保留
        }

        const QString key = absolute.toString(QUrl::FullyEncoded);
        if (tokenCache.contains(key))
            return tokenCache.value(key);

        const QUrl proxy = proxyUrlFor(absolute, headers);
        if (!proxy.isValid())
            return raw;

        const QString value = proxy.toString(QUrl::FullyEncoded);
        tokenCache.insert(key, value);
        return value;
    };

    static const QRegularExpression uriAttribute(QStringLiteral("URI=\"([^\"]*)\""));
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        if (!trimmed.startsWith(QLatin1Char('#'))) {
            line = proxyFor(trimmed);
            continue;
        }

        if (trimmed.startsWith(QLatin1String("#EXT-X-BASE:"))) {
            const QString value =
                trimmed.mid(QStringLiteral("#EXT-X-BASE:").size()).trimmed();
            const QUrl next = base.resolved(QUrl(value));
            if (next.isValid())
                base = next;
            line = trimmed;
            continue;
        }

        if (!trimmed.contains(QLatin1String("URI=")))
            continue;

        // #EXT-X-KEY / #EXT-X-MAP / #EXT-X-MEDIA / LL-HLS 的 PART 等都带 URI 属性，
        // 不重写的话播放器会直接去 CDN 取 key/init 段，丢掉 Referer/Cookie。
        QString rewritten;
        int last = 0;
        QRegularExpressionMatchIterator it = uriAttribute.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            rewritten += line.mid(last, match.capturedStart(1) - last);
            rewritten += proxyFor(match.captured(1));
            last = match.capturedEnd(1);
        }
        rewritten += line.mid(last);
        line = rewritten;
    }

    return lines.join(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n")).toUtf8();
}

void StreamProxy::attachUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream)
{
    // 播放器断开（切歌、停止）时立刻掐掉上游，避免继续吃带宽
    connect(client, &QAbstractSocket::disconnected, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::readyRead, client, [this, client, reply, upstream] {
        flushUpstream(client, reply, upstream);
    });

    connect(reply, &QNetworkReply::finished, client, [this, client, reply, upstream] {
        flushUpstream(client, reply, upstream);
        if (client->state() == QAbstractSocket::ConnectedState)
            client->disconnectFromHost();
        reply->deleteLater();
    });
}

void StreamProxy::flushUpstream(QTcpSocket *client, QNetworkReply *reply, const QUrl &upstream)
{
    if (client->state() != QAbstractSocket::ConnectedState)
        return;

    if (!client->property(kHeaderSent).toBool()) {
        client->setProperty(kHeaderSent, true);
        if (reply->error() != QNetworkReply::NoError)
            qCWarning(lcProxy) << "上游请求出错：" << reply->errorString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        client->write(buildResponseHead(reply, upstream, status));

        if (client->property(kMethod).toString() == QLatin1String("HEAD")) {
            reply->abort();
            client->disconnectFromHost();
            return;
        }
    }

    if (!reply->isOpen())
        return;

    const QByteArray chunk = reply->readAll();
    if (!chunk.isEmpty())
        client->write(chunk);
}

bool StreamProxy::parseRange(const QString &value, qint64 total, qint64 *start, qint64 *end)
{
    if (start == nullptr || end == nullptr || total <= 0)
        return false;

    const QString trimmed = value.trimmed();
    if (!trimmed.startsWith(QLatin1String("bytes="), Qt::CaseInsensitive))
        return false;

    const QString spec = trimmed.mid(6).trimmed();
    // 多段 Range（bytes=0-1,5-6）无法用单个切片表达，返回 false 让
    // serveFromCache 退化成 200 全量。
    if (spec.contains(QLatin1Char(',')))
        return false;

    const int dash = spec.indexOf(QLatin1Char('-'));
    if (dash < 0)
        return false;

    const QString left = spec.left(dash).trimmed();
    const QString right = spec.mid(dash + 1).trimmed();

    qint64 first = 0;
    qint64 last = 0;
    if (left.isEmpty()) {
        // bytes=-N：最后 N 个字节
        if (right.isEmpty())
            return false;
        bool ok = false;
        const qint64 suffix = right.toLongLong(&ok);
        if (!ok || suffix <= 0)
            return false;
        first = qMax<qint64>(0, total - suffix);
        last = total - 1;
    } else {
        bool ok = false;
        first = left.toLongLong(&ok);
        if (!ok || first < 0)
            return false;
        if (right.isEmpty()) {
            last = total - 1;
        } else {
            last = right.toLongLong(&ok);
            if (!ok || last < first)
                return false;
            last = qMin(last, total - 1);
        }
    }

    *start = first;
    *end = last;
    return true;
}

bool StreamProxy::serveFromCache(QTcpSocket *client, const QSharedPointer<CacheEntry> &entry,
                                 const QString &cacheKey, const QString &rangeHeader,
                                 bool hasRange)
{
    if (entry.isNull() || entry->total < 0)
        return false;
    if (client->state() != QAbstractSocket::ConnectedState)
        return true;

    qint64 start = 0;
    qint64 end = entry->total - 1;
    // 多段 Range 等无法用单段缓存表达的请求，退回“忽略 Range 的 200 全量”，
    // 总比把客户端挂住好；播放器实际只会发单段。
    if (hasRange && !parseRange(rangeHeader, entry->total, &start, &end)) {
        hasRange = false;
        start = 0;
        end = entry->total - 1;
    }

    if (start >= entry->total || start > end) {
        const QByteArray head = buildCacheHead(*entry, 416, start, end);
        client->write(head);
        client->disconnectFromHost();
        return true;
    }

    if (entry->failed) {
        replyError(client, 502, QStringLiteral("Cached stream unavailable"));
        return true;
    }

    client->setProperty(kHeaderSent, false);
    client->setProperty(kCacheNext, start);
    client->setProperty(kCacheEnd, end);
    client->setProperty(kCacheRequested, hasRange);
    entry->clients.append(QPointer<QTcpSocket>(client));

    connect(client, &QTcpSocket::bytesWritten, this,
            [this, cacheKey, entry, client](qint64) {
                if (client->state() != QAbstractSocket::ConnectedState)
                    return;
                const QSharedPointer<CacheEntry> current = m_cache.value(cacheKey);
                if (!current.isNull() && current == entry)
                    pumpCache(cacheKey, entry);
            });
    connect(client, &QTcpSocket::disconnected, this,
            [entry, client] {
                for (int i = entry->clients.size() - 1; i >= 0; --i) {
                    if (entry->clients.at(i).isNull() || entry->clients.at(i).data() == client)
                        entry->clients.removeAt(i);
                }
            });

    // 缓存已经覆盖起始位置才发响应头；否则让播放器等数据到齐。
    // 不在“无数据”时先发 206 头，是为了超时还能合法地回 504。
    if (entry->data.size() > start || entry->complete) {
        const int status = hasRange ? 206 : 200;
        client->write(buildCacheHead(*entry, status, start, end));
        client->setProperty(kHeaderSent, true);
    }

    QPointer<QTcpSocket> guard(client);
    QTimer::singleShot(kRangeWaitMs, client, [this, guard, cacheKey, start] {
        if (guard.isNull())
            return;
        QTcpSocket *socket = guard.data();
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
        if (socket->property(kHeaderSent).toBool())
            return;

        const QSharedPointer<CacheEntry> current = m_cache.value(cacheKey);
        if (!current.isNull() && current->data.size() > start) {
            // 刚好在超时前拿到数据，补一次 pump 再让客户端继续读
            pumpCache(cacheKey, current);
            return;
        }
        if (!current.isNull() && current->complete && start >= current->total) {
            socket->write(buildCacheHead(*current, 416, start, start));
            socket->disconnectFromHost();
            return;
        }

        qCWarning(lcProxy) << "流转缓存等待超时，起始字节" << start;
        replyError(socket, 504, QStringLiteral("Range wait timeout"));
    });

    pumpCache(cacheKey, entry);
    return true;
}

void StreamProxy::pumpCache(const QString &key, const QSharedPointer<CacheEntry> &entry)
{
    if (entry.isNull() || key.isEmpty())
        return;
    const QSharedPointer<CacheEntry> current = m_cache.value(key);
    if (current.isNull() || current != entry)
        return;

    for (int i = entry->clients.size() - 1; i >= 0; --i) {
        QTcpSocket *client = entry->clients.at(i).data();
        if (client == nullptr || client->state() != QAbstractSocket::ConnectedState) {
            entry->clients.removeAt(i);
            continue;
        }

        if (entry->failed) {
            if (!client->property(kHeaderSent).toBool())
                replyError(client, 502, QStringLiteral("Cached stream unavailable"));
            else
                client->disconnectFromHost();
            entry->clients.removeAt(i);
            continue;
        }

        const qint64 next = client->property(kCacheNext).toLongLong();
        const qint64 end = client->property(kCacheEnd).toLongLong();
        const bool requested = client->property(kCacheRequested).toBool();

        if (!client->property(kHeaderSent).toBool()) {
            if (entry->data.size() <= next && !entry->complete)
                continue;
            const int status = requested ? 206 : 200;
            client->write(buildCacheHead(*entry, status, next, end));
            client->setProperty(kHeaderSent, true);
        }

        if (client->bytesToWrite() > kMaxSocketBacklog)
            continue; // 播放器读得慢，等 bytesWritten 再续写

        const qint64 remaining = end + 1 - next;
        if (remaining <= 0) {
            client->disconnectFromHost();
            entry->clients.removeAt(i);
            continue;
        }

        const qint64 available = entry->data.size();
        if (available <= next) {
            if (entry->complete) {
                client->disconnectFromHost();
                entry->clients.removeAt(i);
            }
            continue;
        }

        const qint64 chunk = qMin(qMin(remaining, available - next), kCacheWriteChunk);
        client->write(entry->data.constData() + next, chunk);
        client->setProperty(kCacheNext, next + chunk);
        if (next + chunk > end) {
            client->disconnectFromHost();
            entry->clients.removeAt(i);
        } else if (entry->complete && next + chunk >= available) {
            client->disconnectFromHost();
            entry->clients.removeAt(i);
        }
    }
}

void StreamProxy::readIntoCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                                QNetworkReply *reply)
{
    if (entry.isNull() || reply == nullptr || entry->reply != reply)
        return;
    if (entry->total < 0 || entry->data.size() >= entry->total)
        return;

    QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;

    const qint64 remain = entry->total - entry->data.size();
    if (chunk.size() > remain)
        chunk.truncate(static_cast<int>(remain));
    entry->data.append(chunk);
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    if (entry->data.size() >= entry->total)
        entry->complete = true;
    pumpCache(key, entry);
}

void StreamProxy::finishCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                              QNetworkReply *reply)
{
    if (entry.isNull() || reply == nullptr || entry->reply != reply)
        return;

    readIntoCache(key, entry, reply);
    if (entry->data.size() >= entry->total)
        entry->complete = true;
    if (!entry->complete) {
        // 连接提前结束（断流、被 CDN 掐断）时不要把残缺数据当完整文件，
        // 否则后续 Range 会拿到错误的 Content-Range 和 416。
        qCWarning(lcProxy) << "流转缓存中断：" << reply->errorString()
                           << "已缓存" << entry->data.size() << "/" << entry->total;
        entry->failed = true;
    }

    entry->reply = nullptr;
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    reply->deleteLater();
    pumpCache(key, entry);
    pruneCache(key);
}

void StreamProxy::pruneCache(const QString &keepKey)
{
    if (m_cache.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList drop;
    qint64 totalBytes = 0;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
        const QSharedPointer<CacheEntry> &entry = it.value();
        bool active = false;
        for (const QPointer<QTcpSocket> &client : std::as_const(entry->clients)) {
            if (!client.isNull() && client->state() == QAbstractSocket::ConnectedState) {
                active = true;
                break;
            }
        }
        if (entry->failed && !active) {
            drop.append(it.key());
            continue;
        }
        if (!active && entry->lastUsedMs > 0 && entry->lastUsedMs < now - kCacheIdleMs)
            drop.append(it.key());
        totalBytes += entry->data.size();
    }
    for (const QString &key : std::as_const(drop)) {
        const QSharedPointer<CacheEntry> entry = m_cache.value(key);
        if (!entry.isNull())
            totalBytes -= entry->data.size();
        dropCacheEntry(key);
    }

    while (m_cache.size() > kMaxCacheEntries || totalBytes > kMaxCacheTotalBytes) {
        QString oldestKey;
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
            if (it.key() == keepKey)
                continue;
            bool active = false;
            for (const QPointer<QTcpSocket> &client : std::as_const(it.value()->clients)) {
                if (!client.isNull() && client->state() == QAbstractSocket::ConnectedState) {
                    active = true;
                    break;
                }
            }
            if (active)
                continue;
            if (it.value()->lastUsedMs < oldest) {
                oldest = it.value()->lastUsedMs;
                oldestKey = it.key();
            }
        }
        if (oldestKey.isEmpty())
            break;
        totalBytes -= m_cache.value(oldestKey)->data.size();
        dropCacheEntry(oldestKey);
    }
}

void StreamProxy::dropCacheEntry(const QString &key)
{
    const QSharedPointer<CacheEntry> entry = m_cache.take(key);
    if (entry.isNull())
        return;

    if (entry->reply != nullptr) {
        entry->reply->abort();
        entry->reply->deleteLater();
        entry->reply = nullptr;
    }
    for (const QPointer<QTcpSocket> &client : std::as_const(entry->clients)) {
        if (!client.isNull() && client->state() == QAbstractSocket::ConnectedState)
            client->disconnectFromHost();
    }
    entry->clients.clear();
    entry->data.clear();
    qCDebug(lcProxy) << "流转缓存淘汰" << key.left(32);
}

QByteArray StreamProxy::buildCacheHead(const CacheEntry &entry, int status, qint64 start,
                                       qint64 end) const
{
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' '
                      + statusReason(status) + "\r\n";
    QByteArray contentType = entry.contentType;
    if (contentType.isEmpty())
        contentType = QByteArrayLiteral("application/octet-stream");
    head += "Content-Type: " + contentType + "\r\n";

    if (status == 416) {
        head += "Content-Range: bytes */" + QByteArray::number(entry.total) + "\r\n";
        head += "Content-Length: 0\r\n";
    } else {
        const qint64 length =
            status == 206 ? qMax<qint64>(0, end - start + 1) : qMax<qint64>(0, entry.total);
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
        if (status == 206) {
            head += "Content-Range: bytes " + QByteArray::number(start) + '-'
                    + QByteArray::number(end) + '/' + QByteArray::number(entry.total) + "\r\n";
        }
    }

    head += "Accept-Ranges: bytes\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n\r\n";
    return head;
}

QByteArray StreamProxy::buildResponseHead(QNetworkReply *reply, const QUrl &upstream, int status) const
{
    if (status <= 0)
        status = 502;

    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    const bool hasLength = length.isValid() && length.toLongLong() >= 0;

    // 长度未知时用 HTTP/1.0 + Connection: close，让客户端以连接关闭作为正文结束，
    // 避免 HTTP/1.1 既无 Content-Length 又无 chunked 的歧义。
    QByteArray head = hasLength ? QByteArrayLiteral("HTTP/1.1 ") : QByteArrayLiteral("HTTP/1.0 ");
    head += QByteArray::number(status) + ' ' + statusReason(status) + "\r\n";

    QByteArray contentType;
    const QVariant typeHeader = reply->header(QNetworkRequest::ContentTypeHeader);
    if (typeHeader.isValid())
        contentType = typeHeader.toString().toUtf8();
    if (contentType.isEmpty())
        contentType = guessContentType(upstream);
    head += "Content-Type: " + contentType + "\r\n";

    if (hasLength)
        head += "Content-Length: " + QByteArray::number(length.toLongLong()) + "\r\n";

    const QByteArray contentRange = reply->rawHeader("Content-Range");
    if (!contentRange.isEmpty())
        head += "Content-Range: " + contentRange + "\r\n";

    head += "Accept-Ranges: bytes\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "Connection: close\r\n\r\n";
    return head;
}

void StreamProxy::replyError(QTcpSocket *client, int status, const QString &reason)
{
    const QByteArray body = reason.toUtf8();
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusReason(status) + "\r\n";
    head += "Content-Type: text/plain; charset=utf-8\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    client->write(head + body);
    client->disconnectFromHost();
}

StreamProxy::Request StreamProxy::parseRequest(const QByteArray &payload)
{
    Request request;

    const QList<QByteArray> lines = payload.split('\n');
    if (lines.isEmpty())
        return request;

    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2)
        return request;

    request.method = QString::fromLatin1(parts.at(0));
    request.path = QString::fromLatin1(parts.at(1));

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            break;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        if (key == QLatin1String("range"))
            request.range = QString::fromLatin1(line.mid(colon + 1)).trimmed();
    }

    request.isValid = request.method == QLatin1String("GET")
                   || request.method == QLatin1String("HEAD");
    return request;
}

QByteArray StreamProxy::guessContentType(const QUrl &upstream)
{
    static const QHash<QString, QByteArray> types{
        {QStringLiteral("mp3"), QByteArrayLiteral("audio/mpeg")},
        {QStringLiteral("wav"), QByteArrayLiteral("audio/wav")},
        {QStringLiteral("m4a"), QByteArrayLiteral("audio/mp4")},
        {QStringLiteral("aac"), QByteArrayLiteral("audio/aac")},
        {QStringLiteral("flac"), QByteArrayLiteral("audio/flac")},
        {QStringLiteral("ogg"), QByteArrayLiteral("audio/ogg")},
        {QStringLiteral("opus"), QByteArrayLiteral("audio/ogg")},
        {QStringLiteral("mp4"), QByteArrayLiteral("video/mp4")},
        {QStringLiteral("m4v"), QByteArrayLiteral("video/mp4")},
        {QStringLiteral("mkv"), QByteArrayLiteral("video/x-matroska")},
        {QStringLiteral("webm"), QByteArrayLiteral("video/webm")},
        {QStringLiteral("avi"), QByteArrayLiteral("video/x-msvideo")},
        {QStringLiteral("mov"), QByteArrayLiteral("video/quicktime")},
    };

    const QString suffix = upstream.path().section(QLatin1Char('.'), -1).toLower();
    return types.value(suffix, QByteArrayLiteral("application/octet-stream"));
}
