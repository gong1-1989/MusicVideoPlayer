#include "net/MediaPageResolver.h"

#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "util/HeaderPolicy.h"
#include "util/MediaExtensions.h"
#include "util/NetworkAddressPolicy.h"
#include "util/PageMediaExtractor.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>
#include <QStringList>

#include <limits>

Q_LOGGING_CATEGORY(lcPageResolver, "mvp.net.page")

namespace {

/// 媒体地址必须仍是允许回源的 http(s) 地址；播放页本身可能位于第三方 CDN，
/// 但解析结果不能把用户带回本机/内网。
bool allowedResolvedMedia(const QUrl &media, const MediaItem &item)
{
    if (!NetworkAddressPolicy::isHttpMediaUrl(media))
        return false;
    const QUrl origin = item.sourceOrigin.isValid() && !item.sourceOrigin.isEmpty()
                            ? item.sourceOrigin
                            : item.url;
    return NetworkAddressPolicy::allowsMediaTarget(media, origin);
}

} // namespace

MediaPageResolver::MediaPageResolver(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

bool MediaPageResolver::isPlaybackPage(const QUrl &url)
{
    if (!NetworkAddressPolicy::isHttpMediaUrl(url))
        return false;

    // 已经有媒体后缀的地址就是直链，不能再按网页处理。
    if (MediaExtensions::isMediaContentSuffix(MediaExtensions::suffixFromUrl(url)))
        return false;

    const QString path = url.path().toLower();
    static const QStringList markers = {
        QStringLiteral("/play/"),  QStringLiteral("/vodplay/"), QStringLiteral("/embed/"),
        QStringLiteral("/player/"), QStringLiteral("/e/"),
    };
    for (const QString &marker : markers) {
        if (path.contains(marker))
            return true;
    }

    const QString suffix = MediaExtensions::suffixFromUrl(url);
    return suffix == QLatin1String("html") || suffix == QLatin1String("htm")
           || suffix == QLatin1String("php") || suffix == QLatin1String("asp")
           || suffix == QLatin1String("aspx") || path.endsWith(QLatin1String("/play"));
}

bool MediaPageResolver::needsResolution(const MediaItem &item)
{
    return item.isValid() && item.type == MediaType::Video && isPlaybackPage(item.url);
}

bool MediaPageResolver::cachedMediaUrl(const QUrl &pageUrl, QUrl *mediaUrl) const
{
    const auto it = m_cache.constFind(pageUrl.toString());
    if (it == m_cache.constEnd())
        return false;
    // 页面签名直链会过期；过期条目不能再参与播放，否则重试也只能拿到
    // 同一个 403/404，直到整个进程重启。
    if (it.value().expiresAtMs <= QDateTime::currentMSecsSinceEpoch())
        return false;
    if (mediaUrl != nullptr)
        *mediaUrl = it.value().mediaUrl;
    return true;
}

void MediaPageResolver::invalidate(const QUrl &pageUrl)
{
    m_cache.remove(pageUrl.toString());
}

void MediaPageResolver::rememberResolution(const QUrl &pageUrl, const QUrl &mediaUrl)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 顺手清掉已过期条目，并限制单次运行内解析页面数量，避免搜索结果
    // 反复刷新时把每个播放页地址都永久留在内存里。
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it.value().expiresAtMs <= now)
            it = m_cache.erase(it);
        else
            ++it;
    }
    if (m_cache.size() >= kMaxCacheEntries) {
        QString oldestKey;
        qint64 oldestExpiry = std::numeric_limits<qint64>::max();
        for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it) {
            if (it.value().expiresAtMs < oldestExpiry) {
                oldestExpiry = it.value().expiresAtMs;
                oldestKey = it.key();
            }
        }
        if (!oldestKey.isEmpty())
            m_cache.remove(oldestKey);
    }
    m_cache.insert(pageUrl.toString(),
                   CachedResolution{mediaUrl, now + kCacheTtlMs});
}

void MediaPageResolver::resolve(const MediaItem &item, Callback callback)
{
    const QUrl pageUrl = item.url;
    if (!needsResolution(item)) {
        if (callback)
            callback(item, QString());
        return;
    }

    QUrl cached;
    if (cachedMediaUrl(pageUrl, &cached)) {
        MediaItem resolved = item;
        resolved.url = cached;
        if (callback)
            callback(resolved, QString());
        return;
    }

    if (m_http == nullptr) {
        if (callback)
            callback(item, tr("网络组件不可用，无法解析播放页"));
        return;
    }

    // 播放页可能与搜索接口不同源，Cookie/Authorization 必须按目标地址裁剪，
    // Referer 等非敏感头保留，很多站点靠 Referer 才返回播放器配置。
    QHash<QString, QString> headers =
        HeaderPolicy::sanitizedForTarget(pageUrl, item.headers, item.sourceOrigin);

    qCInfo(lcPageResolver) << "解析播放页" << HttpClient::redactUrl(pageUrl)
                           << item.displayName();
    QPointer<MediaPageResolver> self(this);
    m_http->fetch(
        pageUrl, QByteArray(), headers,
        [self, item, pageUrl, callback](QNetworkReply *reply) {
            if (self.isNull())
                return;
            self->finishResolve(reply, item, pageUrl, callback);
        },
        NetLimits::kHtmlBytes, item.sourceOrigin);
}

void MediaPageResolver::finishResolve(QNetworkReply *reply, const MediaItem &original,
                                      const QUrl &pageUrl, const Callback &callback)
{
    auto fail = [&](const QString &reason) {
        qCWarning(lcPageResolver) << "播放页解析失败" << HttpClient::redactUrl(pageUrl)
                                  << reason;
        if (callback)
            callback(original, reason);
    };

    if (reply == nullptr) {
        fail(tr("播放页请求失败"));
        return;
    }
    // HttpClient 只负责交付 reply，不负责释放；失败分支如果不 deleteLater，
    // 每次播放页 404/403/超限都会把响应体和 QNetworkReply 一直留到 HttpClient
    // 销毁。这里统一在进入处理时登记延迟释放，后续读状态/正文仍然安全。
    reply->deleteLater();
    if (!HttpClient::isHttpSuccess(reply)) {
        fail(tr("播放页打不开：%1").arg(HttpClient::describeError(reply)));
        return;
    }

    bool exceeded = false;
    const QByteArray body = HttpClient::readAllLimited(reply, NetLimits::kHtmlBytes, &exceeded);
    const QUrl finalUrl = reply->url().isValid() ? reply->url() : pageUrl;
    if (exceeded) {
        fail(tr("播放页内容过大，未继续解析"));
        return;
    }

    const QUrl media = PageMediaExtractor::extractMediaUrl(QString::fromUtf8(body), finalUrl);
    if (media.isEmpty()) {
        // 部分站点直接返回媒体流而没有后缀；浏览器播放页本身是 HTML，
        // 真正的媒体地址解析不到时明确失败，避免再把 HTML 交给播放器。
        fail(tr("播放页里没有找到 m3u8/MP4 地址"));
        return;
    }
    if (!allowedResolvedMedia(media, original)) {
        fail(tr("播放页返回的媒体地址指向本机或内网，已拒绝"));
        return;
    }

    rememberResolution(pageUrl, media);
    MediaItem resolved = original;
    resolved.url = media;
    // 原播放页不能留作备用线路：它永远是 HTML，重试只会再失败一次。
    resolved.alternateUrls.removeAll(pageUrl.toString());
    resolved.alternateUrls.removeAll(media.toString());
    qCInfo(lcPageResolver) << "播放页解析完成" << HttpClient::redactUrl(pageUrl)
                           << "->" << HttpClient::redactUrl(media);
    if (callback)
        callback(resolved, QString());
}
