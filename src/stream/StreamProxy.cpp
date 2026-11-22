#include "stream/StreamProxy.h"
#include "cache/MediaCache.h"
#include "stream/HlsSeekWindow.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "util/HeaderPolicy.h"
#include "util/PlaylistSniffer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include <memory>
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
// 整段落盘面向音乐和可直接 Range 的影视直链。磁盘正文跨会话保留，
// 数量与总容量交给用户管理；这里只保留内存缓存预算，避免 HLS 分片吃光内存。
// HLS 深拖动时 FFmpeg 会先回退几十秒，再顺序读取目标前的分片；
// 缓存条目要能同时放下这些分片，不能只有 6 条。
constexpr int kMaxCacheEntries = 24;
constexpr int kCacheIdleMs = 30 * 60 * 1000;
constexpr int kRangeWaitMs = 20 * 1000;
// 网易云等 CDN 会在大约 2 分钟后重置长连接。整段缓存拿到一半就断流时，
// 用已下载长度续发 Range；保留少量次数，防止源站持续 4xx/断流无限重连。
constexpr int kCacheResumeMaxAttempts = 4;
constexpr int kCacheResumeDelayMs = 600;
constexpr qint64 kMaxSocketBacklog = 512LL * 1024;
constexpr qint64 kCacheWriteChunk = 256LL * 1024;
// HLS 分片多连接 Range：目标片通常几百 KB。首片 16KB 只用于探测总长，
// 剩余按 4 路均分。实测从 4 路加到 8 路，10 片总耗时只从 11.58s 降到 11.53s，
// 说明瓶颈是每片的请求往返与顺序打开，而不是单片刻度；多开连接只会增加源站压力。
constexpr int kRangeMaxConnections = kStreamCacheDefaultRangeConnections;
constexpr qint64 kRangeProbeBytes = 16LL * 1024;
constexpr qint64 kRangeMinPartBytes = 24LL * 1024;
constexpr qint64 kRangeMaxSegmentBytes = 8LL * 1024 * 1024;

// 预取窗口：用播放列表索引而不是文件名推算。实测索引 152 对应分片文件
// 0000143.ts，FFmpeg 深拖动后会打开索引 152..161（文件 143..152），
// 所以必须预取 targetIndex..targetIndex+9，只预取目标前后任意一侧都会错位。
// 每片 4 路 Range，同时跑 4 片，峰值 20 条连接（含播放器目标片 4 条），
// 让 9 个后续分片在 FFmpeg 顺序打开到它们之前全部落缓存。
// 实测 3 片并发时最后一波仍只赶上 16KB 探测头，恢复约 5.0 秒；4 片才能覆盖。
constexpr int kSeekPrefetchAheadSegments = 9;
constexpr int kSeekPrefetchConcurrency = 4;
constexpr int kSeekPrefetchDelayMs = 100;
constexpr int kPlaybackPrefetchSegments = 3;
constexpr int kPrefetchConcurrencyMax = 3;
// 连续失败或单次超过该时长时，降低并发并暂停预取，优先保证前台播放。
constexpr int kPrefetchSlowMs = 10 * 1000;
constexpr int kPrefetchDisableMs = 30 * 1000;
constexpr int kPrefetchSeekGuardMs = 30 * 1000;
// 播放器已经挂在预取占位上时，允许等更久，避免慢分片刚过 20 秒就被 504 打断。
constexpr int kPrefetchWaitMs = 45 * 1000;

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
    if (PlaylistSniffer::metadataIndicatesPlaylist(reply->url(), type))
        return true;
    // 签名 HLS 常见：地址没有 .m3u8、Content-Type 是 text/plain。
    // 先按播放列表读取正文，再由 #EXTM3U 做最终确认；不是播放列表会明确失败，
    // 不会把网页/错误文本缓存成媒体。
    return type.startsWith(QLatin1String("text/plain"));
}

/// FFmpeg 对播放列表会带 `Range: bytes=0-`，只是想拿全文。
/// 其它明确区间不能按播放列表处理，否则会拿到半截 m3u8。
bool isOpenEndedRange(const QString &spec)
{
    return spec.trimmed().compare(QLatin1String("bytes=0-"), Qt::CaseInsensitive) == 0;
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

StreamProxy::StreamProxy(QObject *parent)
    : QObject(parent)
    , m_http(new HttpClient(this))
    , m_server(new QTcpServer(this))
    , m_seekPrefetchTimer(new QTimer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &StreamProxy::onNewConnection);
    m_seekPrefetchTimer->setSingleShot(true);
    connect(m_seekPrefetchTimer, &QTimer::timeout, this, [this] {
        m_seekPrefetchActive = false;
        m_seekTargetMs = -1;
    });
}

StreamProxy::~StreamProxy()
{
    shutdown();
}

qint64 StreamProxy::cacheDataSize(const CacheEntry &entry) const
{
    return entry.diskBacked ? entry.dataSize : entry.data.size();
}

qint64 StreamProxy::cacheSlice(CacheEntry &entry, qint64 offset, qint64 length,
                               QByteArray *out) const
{
    if (out == nullptr || offset < 0 || length <= 0)
        return 0;
    if (!entry.diskBacked) {
        if (offset >= entry.data.size())
            return 0;
        *out = entry.data.mid(static_cast<int>(offset), static_cast<int>(length));
        return out->size();
    }

    if (entry.file == nullptr || !entry.file->isOpen())
        return 0;
    if (!entry.file->seek(offset)) {
        qCWarning(lcProxy) << "流转缓存文件定位失败" << entry.filePath << offset;
        return 0;
    }
    out->resize(static_cast<int>(length));
    const qint64 read = entry.file->read(out->data(), length);
    if (read <= 0) {
        out->clear();
        return 0;
    }
    out->resize(static_cast<int>(read));
    return read;
}

bool StreamProxy::appendCacheData(CacheEntry &entry, const QByteArray &chunk)
{
    if (chunk.isEmpty())
        return true;
    if (!entry.diskBacked) {
        entry.data.append(chunk);
        entry.dataSize = entry.data.size();
        return true;
    }

    if (entry.file == nullptr || !entry.file->isOpen())
        return false;
    if (!entry.file->seek(entry.dataSize)) {
        qCWarning(lcProxy) << "流转缓存文件追加定位失败" << entry.filePath;
        return false;
    }
    const qint64 written = entry.file->write(chunk);
    if (written != chunk.size()) {
        qCWarning(lcProxy) << "流转缓存文件写入失败" << entry.filePath << written
                           << "/" << chunk.size();
        return false;
    }
    // 断点续传依赖“文件里已经有这些字节”，每块写完就刷盘；256KB 一块，
    // 对本地 SSD/机械盘都不会形成明显 I/O 压力。
    entry.file->flush();
    entry.dataSize += written;
    return true;
}

qint64 StreamProxy::openCacheFile(CacheEntry &entry, const QString &path,
                                  qint64 existingBytes)
{
    if (path.isEmpty())
        return -1;
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return -1;

    std::unique_ptr<QFile> file(new QFile(path));
    if (!file->open(QIODevice::ReadWrite)) {
        qCWarning(lcProxy) << "流转缓存文件无法打开" << path << file->errorString();
        return -1;
    }

    qint64 actual = file->size();
    if (existingBytes <= 0) {
        if (!file->resize(0)) {
            qCWarning(lcProxy) << "流转缓存文件截断失败" << path;
            return -1;
        }
        actual = 0;
    } else if (actual > existingBytes) {
        if (!file->resize(existingBytes)) {
            qCWarning(lcProxy) << "流转缓存文件尾部截断失败" << path;
            return -1;
        }
        actual = existingBytes;
    } else if (actual < existingBytes) {
        // 元数据记录的长度比真实文件大（上次崩溃在写入与刷盘之间），
        // 以真实文件长度为准，避免在空洞后面追加。
        existingBytes = actual;
    }

    entry.file = file.release();
    entry.filePath = path;
    entry.diskBacked = true;
    entry.dataSize = actual;
    return actual;
}

void StreamProxy::closeCacheFile(CacheEntry &entry)
{
    if (entry.file == nullptr)
        return;
    persistCacheState(entry);
    entry.file->flush();
    entry.file->close();
    delete entry.file;
    entry.file = nullptr;
}

void StreamProxy::persistCacheState(CacheEntry &entry)
{
    if (!entry.diskBacked || entry.filePath.isEmpty())
        return;
    MediaCache::writeState(entry.filePath, entry.cacheIdentity, entry.upstream,
                           entry.contentType, entry.total, entry.complete);
}

void StreamProxy::abortReplySilently(QNetworkReply *reply)
{
    if (reply == nullptr)
        return;
    // abort() 会同步触发 finished；这里先断掉本代理的回调，
    // 避免回调在 shutdown/dropCacheEntry/failRangeEntry 的清理循环里
    // 再改 rangeReplies、rangeParts 或 m_cache，造成迭代器失效/重入。
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}

void StreamProxy::shutdown()
{
    m_listening.store(false);
    m_server->close();
    cancelPrefetch();

    for (const auto &entry : std::as_const(m_cache)) {
        if (entry->reply != nullptr) {
            abortReplySilently(entry->reply);
            entry->reply = nullptr;
        }
        for (const QPointer<QNetworkReply> &reply : std::as_const(entry->rangeReplies)) {
            if (!reply.isNull())
                abortReplySilently(reply.data());
        }
        entry->rangeReplies.clear();
        closeCacheFile(*entry);
    }
    m_cache.clear();
    m_tokens.clear();
    m_playlists.clear();
    m_lastPlaylistKey.clear();
}

bool StreamProxy::start()
{
    if (m_server->isListening()) {
        m_listening.store(true);
        return true;
    }

    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qCWarning(lcProxy) << "流转服务监听失败：" << m_server->errorString();
        m_listening.store(false);
        return false;
    }
    m_listening.store(true);
    qCInfo(lcProxy) << "流转服务已启动，监听 127.0.0.1:" << m_server->serverPort();
    return true;
}

bool StreamProxy::isListening() const
{
    return m_listening.load();
}

QUrl StreamProxy::proxyUrlFor(const QUrl &upstream, const QHash<QString, QString> &headers,
                              const QString &cachePath, const QString &cacheIdentity)
{
    if (!isListening() && !start())
        return QUrl();
    return m_tokens.registerFor(upstream, headers, m_server->serverPort(), cachePath,
                                cacheIdentity);
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

    StreamTokenRegistry::Upstream entry;
    if (!m_tokens.lookup(token, &entry) || entry.url.isEmpty() || !entry.url.isValid()) {
        qCWarning(lcProxy) << "流转令牌未知，前缀：" << token.left(6);
        replyError(client, 404, QStringLiteral("Not Found"));
        return;
    }

    QUrl upstream = entry.url;
    if (!suffix.isEmpty()) {
        // suffix/query 都来自 HTTP 请求行，已经是百分号编码形态。query 不能
        // 再做一次 fromPercentEncoding：签名参数里的 %26、%3D 会被还原成
        // &、=，拼出的上游地址与签名不一致，分片会被 CDN 拒绝。
        const QByteArray relativeTarget =
            query.isEmpty()
                ? suffix.toUtf8()
                : (suffix + QLatin1Char('?') + query).toUtf8();
        QUrl relative = QUrl::fromEncoded(relativeTarget);
        upstream = entry.url.resolved(relative);

        // suffix 只能在同一 host/端口/协议内换路径或文件名。否则
        // `//evil/`、`http://evil/` 会把带 Referer/Cookie 的 token 请求
        // 切到第三方；即使 token 随机，也不该保留这个越权入口。
        const auto effectivePort = [](const QUrl &url) {
            if (url.port() >= 0)
                return url.port();
            return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0
                ? 443 : 80;
        };
        const bool sameAuthority =
            upstream.scheme().compare(entry.url.scheme(), Qt::CaseInsensitive) == 0
            && upstream.host().compare(entry.url.host(), Qt::CaseInsensitive) == 0
            && effectivePort(upstream) == effectivePort(entry.url)
            && upstream.userInfo() == entry.url.userInfo();
        if (!sameAuthority) {
            qCWarning(lcProxy) << "流转令牌试图切换 host：" << HttpClient::redactUrl(upstream)
                               << "原地址" << HttpClient::redactUrl(entry.url);
            replyError(client, 400, QStringLiteral("Bad stream path"));
            return;
        }

        // token 只应对它登记的那个目录树生效。否则本机其他程序拿到 token 后，
        // 可以用 `../../` 在同一 host 上访问任意路径，并复带 Cookie/Referer。
        const QString entryPath = entry.url.path();
        const int entrySlash = entryPath.lastIndexOf(QLatin1Char('/'));
        const QString allowedDir =
            entrySlash >= 0 ? entryPath.left(entrySlash + 1) : QStringLiteral("/");
        if (allowedDir != QLatin1String("/")
            && !upstream.path().startsWith(allowedDir, Qt::CaseSensitive)) {
            qCWarning(lcProxy) << "流转令牌越出作用域：" << HttpClient::redactUrl(upstream)
                               << "作用域" << allowedDir;
            replyError(client, 400, QStringLiteral("Bad stream path"));
            return;
        }
    }
    if (upstream.isEmpty() || !upstream.isValid()) {
        replyError(client, 400, QStringLiteral("Bad stream path"));
        return;
    }

    client->setProperty(kMethod, request.method);
    QByteArray range = request.range.toUtf8();
    bool hasRange = !range.isEmpty();
    // FFmpeg 对 m3u8 也会带 `Range: bytes=0-`。播放列表必须先拿全文才能建分片索引，
    // 带着 Range 会被上游回 206 并绕过 attachPlaylist，预取就永远没有数据源。
    if (hasRange && upstream.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive)) {
        qCDebug(lcProxy) << "播放列表忽略 Range" << request.range;
        range.clear();
        hasRange = false;
    }
    qCDebug(lcProxy) << "流转请求" << request.method << "上游"
                     << HttpClient::redactUrl(upstream) << "分段" << request.range;

    const QHash<QString, QString> headers = entry.headers;
    const bool isHead = request.method == QLatin1String("HEAD");
    // 条目缓存只对应播放列表里那一整条媒体地址；HLS 分片是同一 token 下的
    // 相对子资源，不能把整段视频文件当成某一个分片来写。
    const QString cachePath = suffix.isEmpty() ? entry.cachePath : QString();
    const QString cacheIdentity = suffix.isEmpty() ? entry.cacheIdentity : QString();

    // 缓存键必须同时包含地址和鉴权头，避免同一 CDN 地址在不同 Referer/Cookie
    // 下共用缓存，把 A 源的响应喂给 B 源。
    const QString cacheKey = cacheKeyFor(upstream, headers);

    // 先查缓存：完整或已下载部分都能立刻回 206，拖动无需等上游。
    if (!isHead) {
        const QSharedPointer<CacheEntry> cached = m_cache.value(cacheKey);
        if (!cached.isNull() && !cached->failed
            && (cached->total >= 0 || cached->pending)) {
            if (serveFromCache(client, cached, cacheKey, request.range, hasRange)) {
                qCDebug(lcProxy) << "流转缓存命中" << HttpClient::redactUrl(upstream)
                                 << "已缓存" << cacheDataSize(*cached) << "/" << cached->total;
                return;
            }
        }
    }

    // 已知 HLS 分片且是整段请求时，先走多连接 Range；源站不支持或异常时
    // 在异步回调里回退到原来的单路透传。
    if (!isHead && hasRange && tryStartRangeDownload(client, upstream, headers, request,
                                                     hasRange, cacheKey)) {
        return;
    }

    startUpstreamRequest(client, upstream, headers, request, hasRange, cacheKey, cachePath,
                         cacheIdentity);
}

void StreamProxy::startUpstreamRequest(QTcpSocket *client, const QUrl &upstream,
                                       const QHash<QString, QString> &headers,
                                       const Request &request, bool hasRange,
                                       const QString &cacheKey, const QString &cachePath,
                                       const QString &cacheIdentity)
{
    const bool isHead = request.method == QLatin1String("HEAD");
    QByteArray range = request.range.toUtf8();

    // 整段落盘只在“主地址本身就是一个可按字节读取的文件”时启用。
    // HLS 主地址是播放列表，必须留给 attachPlaylist 改写，不能写入媒体文件。
    const bool diskRequested = !cachePath.isEmpty() && !isHlsUrl(upstream);
    qint64 resumeOffset = 0;
    if (diskRequested && !isHead) {
        MediaCache::State state;
        const bool hasState = MediaCache::readState(cachePath, &state);
        const qint64 size = MediaCache::fileSize(cachePath);
        // 完整缓存已经在磁盘上（例如控制器还没来得及切本地文件，或播放器
        // 重新请求了代理地址）。直接把这个文件当只读缓存供流，绝不能再用
        // 上游 200 打开并截断它，否则辛苦下完的整段缓存会被清空重下。
        if (size > 0 && hasState && state.totalBytes == size && state.complete) {
            auto cachedEntry = QSharedPointer<CacheEntry>::create();
            cachedEntry->total = size;
            cachedEntry->reservedBytes = size;
            cachedEntry->contentType = state.contentType.toLatin1();
            cachedEntry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            cachedEntry->upstream = upstream;
            cachedEntry->complete = true;
            if (openCacheFile(*cachedEntry, cachePath, size) >= 0
                && cacheHasRoom(size, cacheKey, true)) {
                m_cache.insert(cacheKey, cachedEntry);
                if (serveFromCache(client, cachedEntry, cacheKey, request.range, hasRange)) {
                    qCInfo(lcProxy) << "流转缓存直接命中磁盘完整文件"
                                    << HttpClient::redactUrl(upstream) << size << "字节";
                    return;
                }
                m_cache.remove(cacheKey);
            }
            closeCacheFile(*cachedEntry);
        } else if (size > 0 && (!hasState || state.totalBytes <= 0 || size < state.totalBytes)) {
            resumeOffset = size;
            // 播放器通常先发 `bytes=0-`；已有半截文件时要让上游从断点续，
            // 已经下载的前缀继续由本地缓存供播放器顺序读取。
            if (!hasRange || isOpenEndedRange(range)) {
                range = "bytes=" + QByteArray::number(resumeOffset) + "-";
                qCInfo(lcProxy) << "流转缓存断点续传，已有" << resumeOffset << "字节"
                                << HttpClient::redactUrl(upstream);
            }
        }
    }

    // 由 HttpClient 手动跟随 302（含 https→http 降级），拿到可读的上游 reply。
    // 用 stream() 而不是 fetch()：响应头一到就能开始转发，不会先缓冲整段音视频。
    QPointer<QTcpSocket> guard(client);
    m_http->stream(upstream, range, headers,
                   [this, guard, upstream, headers, isHead, hasRange, cacheKey, request,
                    cachePath, cacheIdentity, diskRequested, resumeOffset](QNetworkReply *reply) {
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
        qCDebug(lcProxy) << "流转响应判定：播放列表" << isPlaylistResponse(reply)
                         << "类型" << reply->header(QNetworkRequest::ContentTypeHeader)
                         << "地址" << HttpClient::redactUrl(reply->url());

        // 网易云等 CDN 对播放器的 `Range: bytes=0-` 会回 206，而不是 200。
        // 这仍然是“从文件头开始的整段请求”，必须纳入本地整段缓存；否则
        // 上游约两分钟重置连接时只能直通透传，播放器会收到 Demuxing failed。
        qint64 cacheTotal = -1;
        qint64 initialBytes = 0;
        if (status == 200) {
            const QVariant lengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
            if (lengthHeader.isValid())
                cacheTotal = lengthHeader.toLongLong();
            if (diskRequested && resumeOffset > 0) {
                // 源站忽略了断点 Range，只能把半截文件丢掉从来，不能用 200
                // 的正文去追加，否则文件中间会拼出一段重复内容。
                qCInfo(lcProxy) << "流转缓存续传被源站忽略，从头重建"
                                << HttpClient::redactUrl(upstream);
                initialBytes = 0;
            }
        } else if (status == 206) {
            qint64 rangeStart = -1;
            qint64 rangeEnd = -1;
            qint64 rangeTotal = -1;
            const bool parsed =
                StreamRange::parseContentRange(reply->rawHeader("Content-Range"),
                                               &rangeStart, &rangeEnd, &rangeTotal);
            const qint64 expectedStart = diskRequested ? resumeOffset : 0;
            if (parsed && rangeStart == expectedStart && rangeEnd >= rangeStart
                && rangeTotal > 0) {
                cacheTotal = rangeTotal;
                initialBytes = rangeStart;
            } else if (diskRequested && resumeOffset > 0 && parsed
                       && rangeStart != resumeOffset) {
                // 已有半截缓存但源站返回了别的起点，先清掉这条条目的文件，
                // 再用无 Range 请求完整重建，避免把不同偏移的数据拼在一起。
                qCWarning(lcProxy) << "流转缓存续传起点不一致" << rangeStart
                                   << "期望" << resumeOffset
                                   << HttpClient::redactUrl(upstream);
                reply->abort();
                reply->deleteLater();
                QFile::remove(cachePath);
                QFile::remove(MediaCache::metaPathForFile(cachePath));
                Request fresh = request;
                fresh.range.clear();
                startUpstreamRequest(client, upstream, headers, fresh, false, cacheKey,
                                     cachePath, cacheIdentity);
                return;
            }
        }

        // HEAD/带 Range 的播放列表不做正文改写，避免破坏 206 语义；
        // 正常 GET 播放列表则把每个 URI 都映射到代理，绝对地址也不会绕过鉴权头。
        // FFmpeg 的 `bytes=0-` 等价于要全文，可以安全改写；明确区间则拒绝。
        if (!isHead && isPlaylistResponse(reply)) {
            if (!hasRange || isOpenEndedRange(request.range)) {
                attachPlaylist(client, reply, headers);
            } else {
                qCWarning(lcProxy) << "播放列表不支持指定区间" << request.range
                                   << HttpClient::redactUrl(upstream);
                replyError(client, 400, QStringLiteral("playlist range unsupported"));
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        const qint64 cacheLimit =
            diskRequested ? std::numeric_limits<qint64>::max() : kMaxCacheEntryBytes;
        if (!isHead && cacheTotal > 0 && cacheTotal <= cacheLimit) {
            // 200 全量和“从 0/断点开始、未指定结束”的 206 都能缓存整段。
            // 上游已经回明确区间（例如拖动后的 bytes=500000-500100）时保持原样
            // 透传，不额外占缓存，也不能冒充完整文件。
            const QSharedPointer<CacheEntry> existing = m_cache.value(cacheKey);
            if (!existing.isNull() && existing->total >= 0 && !existing->failed) {
                if (serveFromCache(client, existing, cacheKey, request.range, hasRange)) {
                    reply->abort();
                    reply->deleteLater();
                    return;
                }
            }

            auto entry = QSharedPointer<CacheEntry>::create();
            entry->total = cacheTotal;
            entry->reservedBytes = cacheTotal;
            entry->contentType =
                reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
            entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            entry->reply = reply;
            entry->upstream = upstream;
            entry->diskBacked = diskRequested;
            entry->filePath = diskRequested ? cachePath : QString();
            entry->cacheIdentity = diskRequested ? cacheIdentity : QString();
            if (diskRequested) {
                const qint64 opened = openCacheFile(*entry, cachePath, initialBytes);
                if (opened < 0) {
                    qCWarning(lcProxy) << "流转缓存文件打开失败，退回内存缓存"
                                       << cachePath;
                    entry->diskBacked = false;
                    entry->filePath.clear();
                } else {
                    entry->dataSize = opened;
                }
            }
            entry->complete = cacheDataSize(*entry) >= entry->total;
            if (!cacheHasRoom(cacheTotal, cacheKey, entry->diskBacked)) {
                closeCacheFile(*entry);
                qCInfo(lcProxy) << "流转缓存跳过：配额已满" << HttpClient::redactUrl(upstream)
                                << cacheTotal << "字节";
                attachUpstream(client, reply, upstream);
                flushUpstream(client, reply, upstream);
                if (reply->isFinished()) {
                    client->disconnectFromHost();
                    reply->deleteLater();
                }
                return;
            }
            m_cache.insert(cacheKey, entry);
            persistCacheState(*entry);
            qCInfo(lcProxy) << "流转缓存建立" << HttpClient::redactUrl(upstream)
                            << cacheTotal << "字节，已缓存" << cacheDataSize(*entry)
                            << "字节" << (entry->diskBacked ? "磁盘" : "内存");

            QPointer<QTcpSocket> clientGuard(client);
            connect(reply, &QNetworkReply::readyRead, this,
                    [this, cacheKey, entry, reply] {
                        readIntoCache(cacheKey, entry, reply);
                    });
            connect(reply, &QNetworkReply::finished, this,
                    [this, cacheKey, entry, reply, upstream, headers] {
                        finishCache(cacheKey, entry, reply, upstream, headers);
                    });
            readIntoCache(cacheKey, entry, reply);
            if (reply->isFinished())
                finishCache(cacheKey, entry, reply, upstream, headers);
            if (!clientGuard.isNull())
                serveFromCache(clientGuard.data(), entry, cacheKey, request.range, hasRange);
            pruneCache(cacheKey);
            return;
        }

        if (cacheTotal > kMaxCacheEntryBytes) {
            qCInfo(lcProxy) << "流转缓存跳过：文件过大" << HttpClient::redactUrl(upstream)
                            << cacheTotal << "字节";
        }

        attachUpstream(client, reply, upstream);
        flushUpstream(client, reply, upstream);
        if (reply->isFinished()) {
            client->disconnectFromHost();
            reply->deleteLater();
        }
    });
}

bool StreamProxy::isHlsSegmentUrl(const QUrl &url) const
{
    const QString path = url.path().toLower();
    if (path.endsWith(QLatin1String(".ts")) || path.endsWith(QLatin1String(".m4s"))
        || path.endsWith(QLatin1String(".aac"))) {
        return true;
    }
    for (const auto &info : m_playlists) {
        if (info.isNull())
            continue;
        for (const QUrl &segment : info->segments) {
            if (segment == url)
                return true;
        }
    }
    return false;
}

bool StreamProxy::tryStartRangeDownload(QTcpSocket *client, const QUrl &upstream,
                                        const QHash<QString, QString> &headers,
                                        const Request &request, bool hasRange,
                                        const QString &cacheKey)
{
    if (!hasRange || client == nullptr
        || client->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    // 只拆 FFmpeg 请求整段的 `bytes=0-`。带明确末字节的请求是播放器
    // 自己限定范围，贸然下载整片会越界，也会浪费带宽。
    const QString spec = request.range.trimmed();
    if (!spec.startsWith(QLatin1String("bytes="), Qt::CaseInsensitive))
        return false;
    if (spec.mid(6).trimmed() != QLatin1String("0-"))
        return false;
    if (!isHlsSegmentUrl(upstream))
        return false;

    const QSharedPointer<CacheEntry> existing = m_cache.value(cacheKey);
    if (!existing.isNull()) {
        if (!existing->failed)
            return false;
        dropCacheEntry(cacheKey);
    }

    auto entry = QSharedPointer<CacheEntry>::create();
    entry->pending = true;
    entry->multiRange = true;
    // 探测阶段只知道首片大小；按整片上限预留会让 2 条预取就吃满 256MB 配额，
    // 把后面排队的 HLS 分片全部挡住。未知总长时只登记真实在途的首片。
    entry->reservedBytes = kRangeProbeBytes;
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    if (!cacheHasRoom(kRangeProbeBytes, cacheKey)) {
        qCInfo(lcProxy) << "HLS 分片多连接 Range 跳过：缓存配额已满"
                        << HttpClient::redactUrl(upstream);
        return false;
    }
    m_cache.insert(cacheKey, entry);

    qCInfo(lcProxy) << "HLS 分片多连接 Range 开始" << HttpClient::redactUrl(upstream)
                    << "首片" << kRangeProbeBytes << "字节";
    startRangePart(entry, cacheKey, upstream, headers, 0, kRangeProbeBytes - 1, true,
                   client, request);
    return true;
}

void StreamProxy::startRangePart(const QSharedPointer<CacheEntry> &entry,
                                 const QString &cacheKey, const QUrl &upstream,
                                 const QHash<QString, QString> &headers,
                                 qint64 start, qint64 end, bool probe,
                                 QTcpSocket *client, const Request &request)
{
    if (entry.isNull() || entry->failed || !entry->multiRange || start > end)
        return;

    entry->rangePending.insert(start);
    const QByteArray range = "bytes=" + QByteArray::number(start) + '-'
                             + QByteArray::number(end);
    QPointer<StreamProxy> self(this);
    QPointer<QTcpSocket> clientGuard(client);
    m_http->stream(upstream, range, headers,
                   [self, entry, cacheKey, upstream, headers, start, end, probe, clientGuard,
                    request](QNetworkReply *reply) {
        if (self.isNull()) {
            if (reply != nullptr) {
                reply->abort();
                reply->deleteLater();
            }
            return;
        }
        self->onRangePartReply(entry, cacheKey, upstream, headers, reply, start, end, probe,
                               clientGuard.data(), request);
    });
}

void StreamProxy::onRangePartReply(const QSharedPointer<CacheEntry> &entry,
                                   const QString &cacheKey, const QUrl &upstream,
                                   const QHash<QString, QString> &headers,
                                   QNetworkReply *reply, qint64 requestedStart,
                                   qint64 requestedEnd, bool probe, QTcpSocket *client,
                                   const Request &request)
{
    if (entry.isNull() || m_cache.value(cacheKey) != entry) {
        if (reply != nullptr) {
            reply->abort();
            reply->deleteLater();
        }
        return;
    }

    entry->rangePending.remove(requestedStart);

    if (reply == nullptr) {
        if (probe) {
            qCWarning(lcProxy) << "HLS 分片首片请求失败，回退单路";
            entry->pending = false;
            entry->failed = true;
            finishRangePrefetch(cacheKey, entry);
            m_cache.remove(cacheKey);
            if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
                startUpstreamRequest(client, upstream, headers, request, true, cacheKey);
            return;
        }
        if (!entry->rangeRetried.contains(requestedStart)) {
            entry->rangeRetried.insert(requestedStart);
            entry->rangeParts.remove(requestedStart);
            entry->rangePartEnds.remove(requestedStart);
            startRangePart(entry, cacheKey, upstream, headers, requestedStart, requestedEnd,
                           false, nullptr, Request());
            return;
        }
        failRangeEntry(cacheKey, entry, QStringLiteral("分片请求失败"));
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!HttpClient::isHttpSuccess(reply) || status != 206) {
        if (probe) {
            qCWarning(lcProxy) << "HLS 分片首片未返回 206，回退单路"
                               << HttpClient::redactUrl(upstream) << "状态" << status
                               << reply->errorString();
            if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
                startUpstreamRequest(client, upstream, headers, request, true, cacheKey);
            reply->abort();
            reply->deleteLater();
            entry->pending = false;
            entry->failed = true;
            finishRangePrefetch(cacheKey, entry);
            m_cache.remove(cacheKey);
            return;
        }
        failRangeEntry(cacheKey, entry, QStringLiteral("分片未返回 206"));
        reply->deleteLater();
        return;
    }

    qint64 partStart = -1;
    qint64 partEnd = -1;
    qint64 total = -1;
    if (!StreamRange::parseContentRange(reply->rawHeader("Content-Range"), &partStart, &partEnd,
                                        &total)
        || partStart != requestedStart) {
        if (probe) {
            qCWarning(lcProxy) << "HLS 分片首片 Content-Range 无效，回退单路"
                               << reply->rawHeader("Content-Range");
            if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
                startUpstreamRequest(client, upstream, headers, request, true, cacheKey);
            reply->abort();
            reply->deleteLater();
            entry->pending = false;
            entry->failed = true;
            finishRangePrefetch(cacheKey, entry);
            m_cache.remove(cacheKey);
            return;
        }
        failRangeEntry(cacheKey, entry, QStringLiteral("分片 Content-Range 无效"));
        reply->deleteLater();
        return;
    }

    if (probe) {
        if (total > kRangeMaxSegmentBytes) {
            qCInfo(lcProxy) << "HLS 分片过大，回退单路" << total << "字节";
            if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
                startUpstreamRequest(client, upstream, headers, request, true, cacheKey);
            reply->abort();
            reply->deleteLater();
            entry->pending = false;
            entry->failed = true;
            finishRangePrefetch(cacheKey, entry);
            m_cache.remove(cacheKey);
            return;
        }

        entry->total = total;
        entry->reservedBytes = total;
        entry->pending = false;
        entry->contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        const int connections = qMax(2, entry->rangeConnections);
        qCInfo(lcProxy) << "HLS 分片总长" << total << "字节，按"
                        << connections << "路拆分";
        if (client != nullptr && client->state() == QAbstractSocket::ConnectedState)
            serveFromCache(client, entry, cacheKey, request.range, true);

        const QList<StreamRange::Part> parts =
            StreamRange::split(total, partEnd + 1, connections, kRangeMinPartBytes);
        for (const StreamRange::Part &part : parts) {
            startRangePart(entry, cacheKey, upstream, headers, part.start, part.end, false,
                           nullptr, Request());
        }
        pruneCache(cacheKey);
    } else if (partEnd > requestedEnd || total != entry->total) {
        failRangeEntry(cacheKey, entry, QStringLiteral("分片范围与总长不一致"));
        reply->deleteLater();
        return;
    }

    entry->rangeReplies.append(QPointer<QNetworkReply>(reply));
    connect(reply, &QNetworkReply::readyRead, this,
            [this, entry, reply, partStart, partEnd] {
                readRangePartIntoCache(entry, reply, partStart, partEnd);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, cacheKey, entry, reply, partStart, partEnd, upstream, headers] {
                finishRangePart(cacheKey, entry, reply, partStart, partEnd, upstream, headers);
            });

    readRangePartIntoCache(entry, reply, partStart, partEnd);
    if (reply->isFinished())
        finishRangePart(cacheKey, entry, reply, partStart, partEnd, upstream, headers);
}

void StreamProxy::readRangePartIntoCache(const QSharedPointer<CacheEntry> &entry,
                                         QNetworkReply *reply, qint64 start, qint64 end)
{
    if (entry.isNull() || reply == nullptr || !entry->multiRange || start > end)
        return;
    const qint64 expected = end - start + 1;
    if (entry->rangeParts.value(start).size() >= expected)
        return;

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;
    StreamRange::append(*entry, start, end, chunk);
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
}

void StreamProxy::mergeRangeParts(const QString &key,
                                  const QSharedPointer<CacheEntry> &entry)
{
    if (entry.isNull() || m_cache.value(key) != entry)
        return;

    StreamRange::mergeConsecutive(*entry);
    pumpCache(key, entry);
    if (entry->complete)
        finishRangePrefetch(key, entry);
}

void StreamProxy::finishRangePart(const QString &key,
                                  const QSharedPointer<CacheEntry> &entry,
                                  QNetworkReply *reply, qint64 start, qint64 end,
                                  const QUrl &upstream,
                                  const QHash<QString, QString> &headers)
{
    if (entry.isNull() || reply == nullptr)
        return;
    entry->rangeReplies.removeAll(QPointer<QNetworkReply>(reply));
    entry->rangePending.remove(start);
    readRangePartIntoCache(entry, reply, start, end);

    const qint64 expected = end - start + 1;
    const QByteArray buffer = entry->rangeParts.value(start);
    // 前一个分片收尾时 mergeRangeParts 可能已经把后面已完整的分片并进
    // data，并把它从 rangeParts 删除；此时这里看到“空 buffer”是正常的，
    // 只要连续前缀已经覆盖该段就不要再重试。
    const bool alreadyMerged = entry->data.size() >= start + expected;
    if (buffer.size() < expected && !alreadyMerged) {
        if (!entry->rangeRetried.contains(start)) {
            entry->rangeRetried.insert(start);
            entry->rangeParts.remove(start);
            entry->rangePartEnds.remove(start);
            qCWarning(lcProxy) << "HLS 分片段不完整，重试一次" << start << buffer.size()
                               << "/" << expected;
            startRangePart(entry, key, upstream, headers, start, end, false, nullptr,
                           Request());
        } else {
            failRangeEntry(key, entry, QStringLiteral("分片段重试后仍不完整"));
        }
        reply->deleteLater();
        return;
    }

    mergeRangeParts(key, entry);
    reply->deleteLater();
}

void StreamProxy::failRangeEntry(const QString &key,
                                 const QSharedPointer<CacheEntry> &entry,
                                 const QString &reason)
{
    if (entry.isNull())
        return;
    qCWarning(lcProxy) << "HLS 分片多连接失败" << reason
                       << "已缓存" << entry->data.size() << "/" << entry->total;
    entry->failed = true;
    entry->pending = false;
    for (const QPointer<QNetworkReply> &reply : std::as_const(entry->rangeReplies)) {
        if (!reply.isNull())
            abortReplySilently(reply.data());
    }
    entry->rangeReplies.clear();
    entry->rangePending.clear();
    pumpCache(key, entry);
    finishRangePrefetch(key, entry);
}

QString StreamProxy::cacheKeyFor(const QUrl &upstream,
                                 const QHash<QString, QString> &headers)
{
    return StreamTokenRegistry::cacheKeyFor(upstream, headers);
}

void StreamProxy::rememberPlaylist(const QByteArray &body, const QUrl &playlistUrl,
                                   const QHash<QString, QString> &headers)
{
    qCDebug(lcProxy) << "HLS 分片索引输入" << body.size() << "字节"
                     << HttpClient::redactUrl(playlistUrl);

    const QSharedPointer<PlaylistInfo> info = HlsPlaylist::parse(body, playlistUrl);
    if (info.isNull())
        return;

    const QString key = cacheKeyFor(playlistUrl, headers);
    m_playlists.insert(key, info);
    if (info->prefetchable)
        m_lastPlaylistKey = key;

    // 一个进程看很多部片子时，分片索引不能无限累积；只保留最近几份。
    if (m_playlists.size() > 8) {
        QString oldestKey;
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (auto it = m_playlists.cbegin(); it != m_playlists.cend(); ++it) {
            if (it.key() == key || it.key() == m_lastPlaylistKey)
                continue;
            if (it.value()->lastUsedMs < oldest) {
                oldest = it.value()->lastUsedMs;
                oldestKey = it.key();
            }
        }
        if (!oldestKey.isEmpty())
            m_playlists.remove(oldestKey);
    }

    qCDebug(lcProxy) << "HLS 分片索引" << info->segments.size() << "片，总时长"
                     << info->totalMs << "ms，可预取" << info->prefetchable;
    if (info->segments.size() > 143) {
        qCDebug(lcProxy) << "HLS 分片抽样：0" << info->segments.at(0)
                         << "143" << info->segments.at(143) << "起始" << info->starts.at(143)
                         << "152" << info->segments.at(152) << "起始" << info->starts.at(152);
    }
}

const StreamProxy::PlaylistInfo *StreamProxy::playlistFor(
    const QUrl &playlistUrl, const QHash<QString, QString> &headers) const
{
    const QString key = cacheKeyFor(playlistUrl, headers);
    const auto it = m_playlists.constFind(key);
    if (it != m_playlists.cend() && it.value()->prefetchable)
        return it.value().data();

    // 很多站点的 item.url 是 master playlist，真正的分片列表是 FFmpeg 后续请求的
    // 子列表；这里回退到最近一次解析出的媒体列表，保证深拖动仍能预取。
    if (!m_lastPlaylistKey.isEmpty()) {
        const auto last = m_playlists.constFind(m_lastPlaylistKey);
        if (last != m_playlists.cend() && last.value()->prefetchable
            && HlsPlaylist::sameScope(last.value()->source, playlistUrl))
            return last.value().data();
    }
    return nullptr;
}

void StreamProxy::prefetchForSeek(const QUrl &playlistUrl,
                                  const QHash<QString, QString> &headers, qint64 targetMs)
{
    if (targetMs < 0 || QDateTime::currentMSecsSinceEpoch() < m_prefetchDisabledUntilMs)
        return;

    const PlaylistInfo *info = playlistFor(playlistUrl, headers);
    if (info == nullptr)
        return;

    const int targetIndex = HlsPlaylist::segmentIndexAt(*info, targetMs);
    if (targetIndex < 0)
        return;

    // 这里必须用播放列表索引，不能用文件名反推：索引 152 实际是文件 143。
    // FFmpeg 从目标索引开始顺序打开 10 片，窗口统一由 HlsSeekWindow 计算。
    const auto window = HlsSeekWindow::forTarget(targetIndex, info->segments.size(),
                                                 kSeekPrefetchAheadSegments);
    if (!window.isValid())
        return;

    const int first = window.first;
    const int last = window.last;

    cancelPrefetch();
    const qint64 generation = m_prefetchGeneration;
    m_prefetchPlaylistKey = cacheKeyFor(playlistUrl, headers);
    m_prefetchConcurrency = kSeekPrefetchConcurrency;
    m_seekPrefetchActive = true;
    m_seekTargetMs = targetMs;
    m_seekPrefetchTimer->start(kPrefetchSeekGuardMs);

    qCInfo(lcProxy) << "HLS 定位后续预取：目标" << targetMs << "ms，目标片" << targetIndex
                    << "，预取" << first << "到" << last;
    QPointer<StreamProxy> self(this);
    QTimer::singleShot(kSeekPrefetchDelayMs, this,
                       [self, playlistUrl, headers, first, last, generation] {
        if (self.isNull() || generation != self->m_prefetchGeneration
            || !self->m_seekPrefetchActive) {
            return;
        }
        self->enqueuePrefetch(playlistUrl, headers, first, last, generation);
    });
}

void StreamProxy::prefetchForPosition(const QUrl &playlistUrl,
                                      const QHash<QString, QString> &headers, qint64 positionMs)
{
    if (positionMs < 0 || QDateTime::currentMSecsSinceEpoch() < m_prefetchDisabledUntilMs)
        return;

    if (m_seekPrefetchActive) {
        // 深拖动还没真正出画时，不要让正常播放预取插队。
        if (m_seekTargetMs < 0 || positionMs < m_seekTargetMs + 5000)
            return;
        m_seekPrefetchActive = false;
        m_seekTargetMs = -1;
        m_seekPrefetchTimer->stop();
    }

    const PlaylistInfo *info = playlistFor(playlistUrl, headers);
    if (info == nullptr)
        return;

    // 切到另一部片子时，队列里上一部的分片已经没用了，不能让它先占带宽。
    const QString playlistKey = cacheKeyFor(playlistUrl, headers);
    if (playlistKey != m_prefetchPlaylistKey) {
        cancelPrefetch();
        m_prefetchPlaylistKey = playlistKey;
    }

    // 正常播放只预取后面几片，单路即可；和前台抢带宽的收益不如稳定出画。
    m_prefetchConcurrency = 1;

    const int current = HlsPlaylist::segmentIndexAt(*info, positionMs);
    if (current < 0)
        return;

    // 旧位置的预取任务已经没用了，队列里最多保留一轮，避免越积越多。
    if (m_prefetchQueue.size() > kPlaybackPrefetchSegments * 2)
        m_prefetchQueue.clear();

    const int first = current + 1;
    const int last = qMin(info->segments.size() - 1, current + kPlaybackPrefetchSegments);
    if (first <= last)
        enqueuePrefetch(playlistUrl, headers, first, last, m_prefetchGeneration);
}

void StreamProxy::enqueuePrefetch(const QUrl &playlistUrl,
                                  const QHash<QString, QString> &headers,
                                  int firstIndex, int lastIndex, qint64 generation)
{
    const PlaylistInfo *info = playlistFor(playlistUrl, headers);
    if (info == nullptr)
        return;

    for (int index = qMax(0, firstIndex); index <= lastIndex; ++index) {
        if (index >= info->segments.size())
            break;

        const QUrl segment = info->segments.at(index);
        const QString key = cacheKeyFor(segment, headers);
        const QSharedPointer<CacheEntry> cached = m_cache.value(key);
        if (!cached.isNull() && (cached->pending || cached->reply != nullptr
                                 || cached->total >= 0))
            continue;

        bool queued = false;
        for (const PrefetchJob &job : std::as_const(m_prefetchQueue)) {
            if (job.cacheKey == key) {
                queued = true;
                break;
            }
        }
        if (queued)
            continue;

        PrefetchJob job;
        job.upstream = segment;
        job.headers = headers;
        job.cacheKey = key;
        job.generation = generation;
        job.index = index;
        m_prefetchQueue.append(job);
    }
    startNextPrefetch();
}

void StreamProxy::startNextPrefetch()
{
    if (m_prefetchActive >= m_prefetchConcurrency)
        return;
    if (QDateTime::currentMSecsSinceEpoch() < m_prefetchDisabledUntilMs) {
        m_prefetchQueue.clear();
        return;
    }

    while (m_prefetchActive < m_prefetchConcurrency && !m_prefetchQueue.isEmpty()) {
        const PrefetchJob job = m_prefetchQueue.takeFirst();
        if (job.cacheKey.isEmpty() || m_prefetchReplies.contains(job.cacheKey))
            continue;

        const QSharedPointer<CacheEntry> cached = m_cache.value(job.cacheKey);
        if (!cached.isNull() && (cached->pending || cached->reply != nullptr
                                 || cached->total >= 0))
            continue;

        // 预取也要先占配额；未知总长时只按真实在途的探测片登记，
        // 收到 Content-Range 后再改成整片总长，避免 4 条预取互相挤占。
        if (cached.isNull() && !cacheHasRoom(kRangeProbeBytes, job.cacheKey)) {
            qCDebug(lcProxy) << "HLS 预取跳过：缓存配额已满"
                             << HttpClient::redactUrl(job.upstream);
            continue;
        }

        // 拖动后的后续分片用多连接 Range 逐片流水预取：单连接每片约 0.5 秒，
        // 播放器顺序打开 10 片就要 5 秒；4 路 Range 每片约 0.4 秒且与前台重叠，
        // 才能真正缩短恢复时间。正常播放仍走单连接，避免长期多占源站连接。
        if (m_seekPrefetchActive && isHlsSegmentUrl(job.upstream)) {
            if (startRangePrefetch(job))
                continue;
        }

        ++m_prefetchActive;
        m_prefetchStartedMs.insert(job.cacheKey, QDateTime::currentMSecsSinceEpoch());

        // 先占位再发请求：播放器几乎同时会来请求同一片，占位能让它挂在
        // 这条预取连接上，而不是自己再发一条重复请求。
        if (cached.isNull()) {
            auto placeholder = QSharedPointer<CacheEntry>::create();
            placeholder->pending = true;
            placeholder->reservedBytes = kRangeProbeBytes;
            placeholder->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            m_cache.insert(job.cacheKey, placeholder);
        } else {
            cached->pending = true;
        }

        qCDebug(lcProxy) << "HLS 预取开始" << job.index
                         << HttpClient::redactUrl(job.upstream);

        QPointer<StreamProxy> self(this);
        m_http->stream(job.upstream, QByteArray(), job.headers,
                       [self, job](QNetworkReply *reply) {
            if (self.isNull()) {
                if (reply != nullptr) {
                    reply->abort();
                    reply->deleteLater();
                }
                return;
            }
            self->onPrefetchReply(reply, job);
        });
    }
}

bool StreamProxy::startRangePrefetch(const PrefetchJob &job)
{
    if (!m_seekPrefetchActive || job.cacheKey.isEmpty()
        || m_rangePrefetchKeys.contains(job.cacheKey))
        return false;

    auto entry = QSharedPointer<CacheEntry>::create();
    entry->pending = true;
    entry->multiRange = true;
    entry->prefetch = true;
    entry->reservedBytes = kRangeProbeBytes;
    entry->rangeConnections = qMin(kRangeMaxConnections, 4);
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    m_cache.insert(job.cacheKey, entry);
    m_rangePrefetchKeys.insert(job.cacheKey);
    ++m_prefetchActive;
    m_prefetchStartedMs.insert(job.cacheKey, QDateTime::currentMSecsSinceEpoch());

    qCDebug(lcProxy) << "HLS 拖动预取开始（多连接）" << job.index
                     << HttpClient::redactUrl(job.upstream);
    startRangePart(entry, job.cacheKey, job.upstream, job.headers, 0,
                   kRangeProbeBytes - 1, true, nullptr, Request());
    pruneCache(job.cacheKey);
    return true;
}

void StreamProxy::finishRangePrefetch(const QString &cacheKey,
                                      const QSharedPointer<CacheEntry> &entry)
{
    if (entry.isNull() || !entry->prefetch)
        return;
    entry->prefetch = false;
    m_rangePrefetchKeys.remove(cacheKey);
    m_prefetchStartedMs.remove(cacheKey);
    m_prefetchActive = qMax(0, m_prefetchActive - 1);
    startNextPrefetch();
}

void StreamProxy::onPrefetchReply(QNetworkReply *reply, const PrefetchJob &job)
{
    if (reply == nullptr) {
        m_prefetchActive = qMax(0, m_prefetchActive - 1);
        m_prefetchStartedMs.remove(job.cacheKey);
        const QSharedPointer<CacheEntry> pending = m_cache.value(job.cacheKey);
        if (!pending.isNull() && pending->pending && pending->reply == nullptr) {
            pending->pending = false;
            pending->failed = true;
            pumpCache(job.cacheKey, pending);
        }
        startNextPrefetch();
        return;
    }

    // cancelPrefetch 已经把这一轮计入完成数并中止了请求；迟到的回调只清尾，
    // 不能再按慢请求处理，否则一次正常 seek 会被误判成源站变慢。
    if (job.generation != m_prefetchGeneration) {
        reply->abort();
        reply->deleteLater();
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QVariant lengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
    const qint64 total = lengthHeader.isValid() ? lengthHeader.toLongLong() : -1;
    if (!HttpClient::isHttpSuccess(reply) || status != 200 || !looksLikeMedia(reply)
        || total <= 0 || total > kMaxCacheEntryBytes) {
        qCWarning(lcProxy) << "HLS 预取跳过" << HttpClient::redactUrl(job.upstream)
                           << "状态" << status << "长度" << total
                           << reply->errorString();
        reply->abort();
        reply->deleteLater();
        m_prefetchActive = qMax(0, m_prefetchActive - 1);
        m_prefetchStartedMs.remove(job.cacheKey);
        // 占位条目可能已经挂着播放器请求；标记失败并唤醒它，
        // 否则播放器会一直等到缓存等待超时才收到 504。
        const QSharedPointer<CacheEntry> pending = m_cache.value(job.cacheKey);
        if (!pending.isNull() && pending->pending && pending->reply == nullptr) {
            pending->pending = false;
            pending->failed = true;
            pumpCache(job.cacheKey, pending);
        }
        if (networkError != QNetworkReply::OperationCanceledError) {
            m_prefetchConcurrency = 1;
            m_prefetchDisabledUntilMs =
                QDateTime::currentMSecsSinceEpoch() + kPrefetchDisableMs;
            m_prefetchQueue.clear();
        }
        startNextPrefetch();
        return;
    }

    QSharedPointer<CacheEntry> entry = m_cache.value(job.cacheKey);
    if (!entry.isNull() && (entry->reply != nullptr || entry->total >= 0)) {
        reply->abort();
        reply->deleteLater();
        m_prefetchActive = qMax(0, m_prefetchActive - 1);
        m_prefetchStartedMs.remove(job.cacheKey);
        startNextPrefetch();
        return;
    }

    // 复用预取占位条目，不能新建：播放器可能已经挂在这个占位条目的
    // clients 列表上，换对象会让它一直等到 20 秒超时。
    if (entry.isNull())
        entry = QSharedPointer<CacheEntry>::create();
    entry->total = total;
    entry->reservedBytes = total;
    entry->contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    entry->reply = reply;
    entry->pending = false;
    entry->failed = false;
    entry->complete = false;
    m_cache.insert(job.cacheKey, entry);
    m_prefetchReplies.insert(job.cacheKey, reply);

    connect(reply, &QNetworkReply::readyRead, this,
            [this, key = job.cacheKey, entry, reply] {
                readIntoCache(key, entry, reply);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, key = job.cacheKey, entry, reply] {
                finishCache(key, entry, reply);
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, key = job.cacheKey, reply, generation = job.generation] {
                finishPrefetch(key, reply, generation);
            });

    readIntoCache(job.cacheKey, entry, reply);
    if (reply->isFinished()) {
        finishCache(job.cacheKey, entry, reply);
        finishPrefetch(job.cacheKey, reply, job.generation);
    }

    pruneCache(job.cacheKey);
    startNextPrefetch();
}

void StreamProxy::finishPrefetch(const QString &cacheKey, QNetworkReply *reply,
                                 qint64 generation)
{
    const auto it = m_prefetchReplies.find(cacheKey);
    if (it == m_prefetchReplies.end() || it.value() != reply)
        return;
    m_prefetchReplies.erase(it);

    const qint64 started = m_prefetchStartedMs.take(cacheKey);
    const qint64 elapsed = started > 0
        ? QDateTime::currentMSecsSinceEpoch() - started
        : 0;
    m_prefetchActive = qMax(0, m_prefetchActive - 1);

    if (generation != m_prefetchGeneration) {
        qCDebug(lcProxy) << "HLS 预取结束（已过期）" << cacheKey.left(24);
        startNextPrefetch();
        return;
    }

    const QSharedPointer<CacheEntry> entry = m_cache.value(cacheKey);
    const bool failed = entry.isNull() || entry->failed || !entry->complete;
    if (failed || elapsed > kPrefetchSlowMs) {
        // 预取把源站拖慢时立刻降到单路并暂停一段时间，前台播放优先。
        m_prefetchConcurrency = 1;
        m_prefetchDisabledUntilMs =
            QDateTime::currentMSecsSinceEpoch() + kPrefetchDisableMs;
        m_prefetchQueue.clear();
    } else if (!m_seekPrefetchActive && elapsed > 0 && elapsed < 3000
               && m_prefetchConcurrency < kPrefetchConcurrencyMax) {
        ++m_prefetchConcurrency;
    }

    qCDebug(lcProxy) << "HLS 预取结束" << cacheKey.left(24) << "耗时" << elapsed
                     << "ms，并发" << m_prefetchConcurrency;
    startNextPrefetch();
}

void StreamProxy::cancelPrefetch()
{
    ++m_prefetchGeneration;
    m_prefetchQueue.clear();
    m_seekPrefetchActive = false;
    m_seekTargetMs = -1;
    m_seekPrefetchTimer->stop();

    // 只清队列不够：上一轮已经在下载的预取请求还会继续占带宽，
    // 新 seek 开始时必须把它们中止，否则新旧窗口会互相抢源站连接。
    const QStringList keys = m_prefetchReplies.keys();
    for (const QString &key : keys) {
        QNetworkReply *reply = m_prefetchReplies.take(key);
        m_prefetchStartedMs.remove(key);
        if (reply != nullptr)
            abortReplySilently(reply);
    }

    // 多连接 Range 预取不在 m_prefetchReplies 里，必须按 key 单独中止；
    // 否则旧 seek 的 4 路请求会继续和新的目标片抢带宽。
    const QStringList rangeKeys = m_rangePrefetchKeys.values();
    for (const QString &key : rangeKeys) {
        const QSharedPointer<CacheEntry> entry = m_cache.value(key);
        if (entry.isNull())
            continue;
        // 关键保护：这条缓存已经有播放器客户端挂上（FFmpeg 正在顺序读），
        // 此时中止 Range 连接会让客户端收到断流/502 并直接崩溃。
        // 不再当它是预取：保留数据与在途请求，交给播放器正常读完，
        // 只把预取计数摘掉，避免旧一代的 finished 回调误减新 seek 的并发槽。
        if (!entry->clients.isEmpty()) {
            entry->prefetch = false;
            m_prefetchStartedMs.remove(key);
            continue;
        }
        for (const QPointer<QNetworkReply> &reply : std::as_const(entry->rangeReplies)) {
            if (!reply.isNull())
                abortReplySilently(reply.data());
        }
        entry->rangeReplies.clear();
        entry->rangePending.clear();
        entry->pending = false;
        entry->failed = true;
        entry->prefetch = false;
        pumpCache(key, entry);
        m_cache.remove(key);
    }
    m_rangePrefetchKeys.clear();

    // 还有一批请求停在“已占位、响应头未到”，它们不在 m_prefetchReplies 里。
    // 取消时要一并把占位条目判失败并唤醒挂在上面的播放器请求。
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        const QSharedPointer<CacheEntry> &entry = it.value();
        if (entry->multiRange)
            continue;
        if (entry->pending && entry->reply == nullptr) {
            entry->pending = false;
            entry->failed = true;
            pumpCache(it.key(), entry);
        }
    }

    // 已取消请求的迟到回调会因 generation 不匹配直接返回，不会再减计数；
    // 这里必须归零，否则新窗口会以为并发槽位还满着。
    m_prefetchActive = 0;
    m_prefetchStartedMs.clear();
}

void StreamProxy::attachPlaylist(QTcpSocket *client, QNetworkReply *reply,
                                 const QHash<QString, QString> &headers)
{
    QPointer<QTcpSocket> guard(client);
    auto finished = std::make_shared<bool>(false);

    auto finish = [this, guard, reply, headers, finished] {
        if (*finished)
            return;
        *finished = true;
        qCDebug(lcProxy) << "播放列表收尾：客户端在线" << !guard.isNull()
                         << "成功" << HttpClient::isHttpSuccess(reply)
                         << "已结束" << reply->isFinished();
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

        bool tooLarge = false;
        const QByteArray body =
            HttpClient::readAllLimited(reply, NetLimits::kPlaylistBytes, &tooLarge);
        const QUrl finalUrl = reply->url();
        if (tooLarge) {
            qCWarning(lcProxy) << "播放列表超过" << NetLimits::kPlaylistBytes
                               << "字节上限：" << HttpClient::redactUrl(finalUrl);
            replyError(socket, 502, QStringLiteral("playlist too large"));
            reply->deleteLater();
            return;
        }
        if (!PlaylistSniffer::bodyIsPlaylist(body)) {
            qCWarning(lcProxy) << "播放列表正文不是 m3u8：" << HttpClient::redactUrl(finalUrl);
            replyError(socket, 404, QStringLiteral("playlist unavailable"));
            reply->deleteLater();
            return;
        }
        QString rewriteError;
        QByteArray rewritten = rewritePlaylist(body, finalUrl, headers, &rewriteError);
        if (!rewriteError.isEmpty()) {
            qCWarning(lcProxy) << "播放列表 URI 被拒绝：" << rewriteError;
            replyError(socket, 502, QStringLiteral("unsupported playlist uri"));
            reply->deleteLater();
            return;
        }
        // 相对分片必须按 302 之后的最终地址解析，不能拿原始请求地址当基准。
        rememberPlaylist(body, finalUrl, headers);

        // 本地代理 URL 没有 .m3u8 后缀，改写后的正文已经是播放列表；
        // 透传上游 text/plain / octet-stream 会让 FFmpeg 猜不到 HLS demuxer。
        const QByteArray contentType = QByteArrayLiteral("application/vnd.apple.mpegurl");

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
    // m3u8 是文本，不是音视频；识别出来后立刻限制接收量，
    // 避免恶意/异常站点用超大播放列表把代理内存撑满。
    HttpClient::limitReplySize(reply, NetLimits::kPlaylistBytes);
    if (reply->isFinished())
        finish();
}

QByteArray StreamProxy::rewritePlaylist(const QByteArray &body, const QUrl &playlistUrl,
                                        const QHash<QString, QString> &headers, QString *error)
{
    return HlsPlaylist::rewrite(
        body, playlistUrl,
        [this, &headers](const QUrl &absolute) { return proxyUrlFor(absolute, headers); },
        error);
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

bool StreamProxy::serveFromCache(QTcpSocket *client, const QSharedPointer<CacheEntry> &entry,
                                 const QString &cacheKey, const QString &rangeHeader,
                                 bool hasRange)
{
    if (entry.isNull() || (entry->total < 0 && !entry->pending))
        return false;
    if (client->state() != QAbstractSocket::ConnectedState)
        return true;

    qint64 start = 0;
    qint64 end = entry->total >= 0 ? entry->total - 1 : -1;
    if (entry->total < 0) {
        // 预取请求还在等响应头，长度未知时先按 200 全量挂住播放器；
        // 播放器发来的 Range 等拿到长度后再由 pumpCache 补成完整响应。
        hasRange = false;
    } else if (hasRange && !StreamRange::parseRange(rangeHeader, entry->total, &start, &end)) {
        // 多段 Range 等无法用单段缓存表达的请求，退回“忽略 Range 的 200 全量”，
        // 总比把客户端挂住好；播放器实际只会发单段。
        hasRange = false;
        start = 0;
        end = entry->total - 1;
    }

    if (entry->total >= 0 && (start >= entry->total || start > end)) {
        const QByteArray head = buildCacheHead(*entry, 416, start, end);
        client->write(head);
        client->disconnectFromHost();
        return true;
    }

    if (entry->failed) {
        replyError(client, 502, QStringLiteral("Cached stream unavailable"));
        return true;
    }

    qCInfo(lcProxy) << "流转缓存服务播放器，范围" << start << end
                    << "已缓存" << cacheDataSize(*entry) << "/" << entry->total
                    << (hasRange ? "206" : "200");
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
                qCInfo(lcProxy) << "流转缓存播放器连接断开，已写到"
                                << client->property(kCacheNext).toLongLong() << "字节";
                for (int i = entry->clients.size() - 1; i >= 0; --i) {
                    if (entry->clients.at(i).isNull() || entry->clients.at(i).data() == client)
                        entry->clients.removeAt(i);
                }
            });

    // 缓存已经覆盖起始位置才发响应头；否则让播放器等数据到齐。
    // 不在“无数据”时先发 206 头，是为了超时还能合法地回 504。
    if (cacheDataSize(*entry) > start || entry->complete) {
        const int status = hasRange ? 206 : 200;
        client->write(buildCacheHead(*entry, status, start, end));
        client->setProperty(kHeaderSent, true);
    }

    QPointer<QTcpSocket> guard(client);
    const int waitMs = entry->pending ? kPrefetchWaitMs : kRangeWaitMs;
    QTimer::singleShot(waitMs, client, [this, guard, cacheKey, start] {
        if (guard.isNull())
            return;
        QTcpSocket *socket = guard.data();
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
        if (socket->property(kHeaderSent).toBool())
            return;

        const QSharedPointer<CacheEntry> current = m_cache.value(cacheKey);
        if (!current.isNull() && cacheDataSize(*current) > start) {
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
        qint64 end = client->property(kCacheEnd).toLongLong();
        const bool requested = client->property(kCacheRequested).toBool();
        // 播放器命中“预取已占位、长度未到”的条目时 end 先记为 -1；
        // 响应头到达后必须补成真实末字节，否则会被误判为已读完。
        if (end < 0 && entry->total >= 0) {
            end = entry->total - 1;
            client->setProperty(kCacheEnd, end);
        }

        if (!client->property(kHeaderSent).toBool()) {
            if (cacheDataSize(*entry) <= next && !entry->complete)
                continue;
            const int status = requested ? 206 : 200;
            client->write(buildCacheHead(*entry, status, next, end));
            client->setProperty(kHeaderSent, true);
        }

        if (client->bytesToWrite() > kMaxSocketBacklog)
            continue; // 播放器读得慢，等 bytesWritten 再续写

        const qint64 remaining = end + 1 - next;
        if (remaining <= 0) {
            // 数据已全部交给套接字，但播放器可能只读走了其中一部分。
            // 此时主动 disconnect 会在 Windows 上让尚未读走的缓存被丢弃，
            // 表现为播放约两分钟后收到 WSAECONNRESET（-10054）。
            // Content-Length 已标明正文长度，保持连接由播放器自己关闭。
            qCInfo(lcProxy) << "流转缓存发送到末尾，累计" << next << "字节";
            entry->clients.removeAt(i);
            continue;
        }

        const qint64 available = cacheDataSize(*entry);
        if (available <= next) {
            if (entry->complete) {
                qCInfo(lcProxy) << "流转缓存发送完成，累计" << next << "字节";
                entry->clients.removeAt(i);
            }
            continue;
        }

        const qint64 chunk = qMin(qMin(remaining, available - next), kCacheWriteChunk);
        QByteArray slice;
        const qint64 got = cacheSlice(*entry, next, chunk, &slice);
        if (got <= 0) {
            qCWarning(lcProxy) << "流转缓存读取失败，跳过该客户端" << entry->filePath
                               << next;
            entry->clients.removeAt(i);
            continue;
        }
        client->write(slice);
        client->setProperty(kCacheNext, next + got);
        if (next + got > end || (entry->complete && next + got >= available)) {
            qCInfo(lcProxy) << "流转缓存发送到末尾，累计" << next + got << "字节";
            entry->clients.removeAt(i);
        }
    }
}

void StreamProxy::readIntoCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                                QNetworkReply *reply)
{
    if (entry.isNull() || reply == nullptr || entry->reply != reply)
        return;
    if (entry->total < 0 || cacheDataSize(*entry) >= entry->total)
        return;

    QByteArray chunk = reply->readAll();
    if (chunk.isEmpty())
        return;

    const qint64 remain = entry->total - cacheDataSize(*entry);
    if (chunk.size() > remain)
        chunk.truncate(static_cast<int>(remain));
    if (!appendCacheData(*entry, chunk)) {
        qCWarning(lcProxy) << "流转缓存写入失败，停止该条目" << entry->filePath;
        entry->failed = true;
        entry->reply = nullptr;
        abortReplySilently(reply);
        pumpCache(key, entry);
        return;
    }
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    if (cacheDataSize(*entry) >= entry->total && !entry->complete) {
        entry->complete = true;
        persistCacheState(*entry);
        qCInfo(lcProxy) << "流转缓存完成" << cacheDataSize(*entry) << "字节"
                        << (entry->diskBacked ? "磁盘" : "内存");
    }
    pumpCache(key, entry);
}

void StreamProxy::finishCache(const QString &key, const QSharedPointer<CacheEntry> &entry,
                              QNetworkReply *reply, const QUrl &upstream,
                              const QHash<QString, QString> &headers)
{
    if (entry.isNull() || reply == nullptr || entry->reply != reply)
        return;

    readIntoCache(key, entry, reply);
    if (cacheDataSize(*entry) >= entry->total)
        entry->complete = true;
    if (!entry->complete) {
        // 连接提前结束（断流、被 CDN 掐断）时不要把残缺数据当完整文件。
        // 已经建立过整段缓存的，只要能定位到同一上游，就从缓存末尾
        // 续发 Range；否则播放器会在两分钟左右直接收到 Demuxing failed。
        if (!entry->failed && upstream.isValid()
            && entry->resumeAttempts < kCacheResumeMaxAttempts) {
            qCWarning(lcProxy) << "流转缓存断流，准备续传：" << reply->errorString()
                               << "已缓存" << cacheDataSize(*entry) << "/" << entry->total;
            entry->reply = nullptr;
            entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            persistCacheState(*entry);
            reply->deleteLater();
            resumeCacheUpstream(key, entry, upstream, headers);
            return;
        }
        qCWarning(lcProxy) << "流转缓存中断：" << reply->errorString()
                           << "已缓存" << cacheDataSize(*entry) << "/" << entry->total;
        entry->failed = true;
        persistCacheState(*entry);
        reply->deleteLater();
        pumpCache(key, entry);
        pruneCache(key);
        return;
    }

    entry->reply = nullptr;
    entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
    persistCacheState(*entry);
    reply->deleteLater();
    pumpCache(key, entry);
    pruneCache(key);
}

void StreamProxy::resumeCacheUpstream(const QString &key,
                                      const QSharedPointer<CacheEntry> &entry,
                                      const QUrl &upstream,
                                      const QHash<QString, QString> &headers)
{
    if (entry.isNull() || !upstream.isValid())
        return;
    if (cacheDataSize(*entry) >= entry->total) {
        entry->complete = true;
        persistCacheState(*entry);
        pumpCache(key, entry);
        return;
    }
    if (cacheDataSize(*entry) <= 0 || entry->resumeAttempts >= kCacheResumeMaxAttempts) {
        entry->failed = true;
        persistCacheState(*entry);
        pumpCache(key, entry);
        return;
    }

    ++entry->resumeAttempts;
    // 续传期间保持 pending，让新到的播放器 Range 请求继续等缓存补齐，
    // 不要因为还没完成的条目另开一条重复上游连接。
    entry->pending = true;
    entry->failed = false;

    QTimer::singleShot(kCacheResumeDelayMs, this, [this, key, entry, upstream, headers] {
        const QSharedPointer<CacheEntry> current = m_cache.value(key);
        if (current.isNull() || current != entry || entry->complete || entry->failed)
            return;

        const qint64 offset = cacheDataSize(*entry);
        const QByteArray range = "bytes=" + QByteArray::number(offset) + "-";
        qCInfo(lcProxy) << "流转缓存续传第" << entry->resumeAttempts << "次，从"
                        << offset << "字节开始";

        m_http->stream(upstream, range, headers,
                       [this, key, entry, upstream, headers, offset](QNetworkReply *reply) {
            const QSharedPointer<CacheEntry> now = m_cache.value(key);
            if (now.isNull() || now != entry || entry->complete || entry->failed) {
                if (reply != nullptr) {
                    reply->abort();
                    reply->deleteLater();
                }
                return;
            }
            if (reply == nullptr) {
                entry->pending = false;
                entry->failed = true;
                pumpCache(key, entry);
                return;
            }

            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qint64 start = -1;
            qint64 end = -1;
            qint64 total = -1;
            const bool rangeOk =
                status == 206
                && StreamRange::parseContentRange(reply->rawHeader("Content-Range"),
                                                  &start, &end, &total)
                && start == offset && end >= start;
            if (!rangeOk) {
                qCWarning(lcProxy) << "流转缓存续传失败，状态" << status
                                   << "Content-Range" << reply->rawHeader("Content-Range");
                reply->abort();
                reply->deleteLater();
                entry->pending = false;
                entry->failed = true;
                pumpCache(key, entry);
                return;
            }

            // 续传响应必须与原文件总长一致，避免源站换文件后把两种内容拼在一起。
            if (entry->total >= 0 && total >= 0 && total != entry->total) {
                qCWarning(lcProxy) << "流转缓存续传总长变化" << entry->total << "->" << total;
                reply->abort();
                reply->deleteLater();
                entry->pending = false;
                entry->failed = true;
                pumpCache(key, entry);
                return;
            }

            entry->reply = reply;
            entry->pending = false;
            entry->failed = false;
            connect(reply, &QNetworkReply::readyRead, this,
                    [this, key, entry, reply] { readIntoCache(key, entry, reply); });
            connect(reply, &QNetworkReply::finished, this,
                    [this, key, entry, reply, upstream, headers] {
                        finishCache(key, entry, reply, upstream, headers);
                    });
            readIntoCache(key, entry, reply);
            if (reply->isFinished())
                finishCache(key, entry, reply, upstream, headers);
            pruneCache(key);
        });
    });
}

bool StreamProxy::cacheHasRoom(qint64 bytes, const QString &keepKey, bool diskBacked)
{
    // 先回收空闲/失败条目，再判断配额。整段落盘缓存写的是用户可见的磁盘文件，
    // 不做条数/容量拒绝；内存缓存仍保留预算，避免 HLS 分片无限增长。
    pruneCache(keepKey);
    if (diskBacked)
        return true;
    if (m_cache.size() >= kMaxCacheEntries)
        return false;

    qint64 memoryBytes = 0;
    for (const auto &entry : std::as_const(m_cache)) {
        if (!entry->diskBacked)
            memoryBytes += StreamCacheStore::bytes(*entry);
    }
    const qint64 need = qMax<qint64>(bytes, 0);
    return memoryBytes + need <= kMaxCacheTotalBytes;
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
        if (!entry->diskBacked)
            totalBytes += StreamCacheStore::bytes(*entry);
        if (entry->failed && !active) {
            drop.append(it.key());
            continue;
        }
        if (!active && entry->lastUsedMs > 0 && entry->lastUsedMs < now - kCacheIdleMs)
            drop.append(it.key());
    }
    for (const QString &key : std::as_const(drop)) {
        const QSharedPointer<CacheEntry> entry = m_cache.value(key);
        if (!entry.isNull() && !entry->diskBacked)
            totalBytes -= StreamCacheStore::bytes(*entry);
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
        if (!m_cache.value(oldestKey)->diskBacked)
            totalBytes -= StreamCacheStore::bytes(*m_cache.value(oldestKey));
        dropCacheEntry(oldestKey);
    }
}

StreamProxy::CacheStats StreamProxy::cacheStats() const
{
    CacheStats stats;
    stats.entries = int(m_cache.size());
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        stats.bytes += StreamCacheStore::bytes(*it.value());
    stats.activePrefetches = m_prefetchActive;
    stats.rangePrefetches = int(m_rangePrefetchKeys.size());
    return stats;
}

void StreamProxy::dropCacheEntry(const QString &key)
{
    const QSharedPointer<CacheEntry> entry = m_cache.take(key);
    if (entry.isNull())
        return;

    const bool wasPrefetch = entry->prefetch;
    bool wasTrackedPrefetch = false;
    if (entry->reply != nullptr) {
        const auto prefetchIt = m_prefetchReplies.find(key);
        if (prefetchIt != m_prefetchReplies.end() && prefetchIt.value() == entry->reply) {
            m_prefetchReplies.erase(prefetchIt);
            m_prefetchStartedMs.remove(key);
            wasTrackedPrefetch = true;
        }
        abortReplySilently(entry->reply);
        entry->reply = nullptr;
    }
    for (const QPointer<QNetworkReply> &reply : std::as_const(entry->rangeReplies)) {
        if (!reply.isNull())
            abortReplySilently(reply.data());
    }
    entry->rangeReplies.clear();
    entry->rangePending.clear();
    entry->rangeParts.clear();
    entry->rangePartEnds.clear();
    for (const QPointer<QTcpSocket> &client : std::as_const(entry->clients)) {
        if (!client.isNull() && client->state() == QAbstractSocket::ConnectedState)
            client->disconnectFromHost();
    }
    entry->clients.clear();
    // 磁盘条目只关句柄并落元数据，保留文件供下次播放命中；内存条目才清正文。
    closeCacheFile(*entry);
    entry->data.clear();
    qCDebug(lcProxy) << "流转缓存淘汰" << key.left(32);
    // 预取条目被缓存淘汰时也必须归还并发槽位，否则后续 seek 预取会一直
    // 以为并发还满着，m_rangePrefetchKeys 残留还会阻止同 key 重新预取。
    if (wasPrefetch)
        finishRangePrefetch(key, entry);
    else if (wasTrackedPrefetch) {
        m_prefetchActive = qMax(0, m_prefetchActive - 1);
        startNextPrefetch();
    }
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
