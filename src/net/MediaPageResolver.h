#ifndef MEDIAPAGERESOLVER_H
#define MEDIAPAGERESOLVER_H

#include "media/MediaItem.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class HttpClient;
class QNetworkReply;

/// 把影视源返回的“网页播放页”解析成真实媒体地址。
///
/// 配置源的 `vod_play_url` 经常是 `https://站点/play/ID` 这种 HTML 页面，
/// 页面里的播放器配置才写着 m3u8/MP4。播放前必须先取出直链，否则
/// 播放器会把 HTML 当视频流，表现为 `Could not open file`。
class MediaPageResolver : public QObject
{
    Q_OBJECT

public:
    using Callback = std::function<void(const MediaItem &item, const QString &error)>;

    explicit MediaPageResolver(HttpClient *http, QObject *parent = nullptr);

    /// 解析一条条目的播放页地址。
    /// 不是网页播放页、已有缓存或参数量为空时会立即回调；网络请求异步完成。
    /// 失败时回调保留原条目，并给出可展示的原因。
    void resolve(const MediaItem &item, Callback callback);

    /// 查询页面地址已解析出的媒体地址；缓存超过有效期时视为未命中。
    bool cachedMediaUrl(const QUrl &pageUrl, QUrl *mediaUrl = nullptr) const;

    /// 让某个播放页的解析结果失效。播放失败时调用，避免签名直链过期后
    /// 本次运行内一直复用旧地址，用户重试也没有机会重新解析。
    void invalidate(const QUrl &pageUrl);

    /// 是否是“需要二次解析”的视频网页播放页。
    static bool needsResolution(const MediaItem &item);

    /// 判断地址是否像影视站播放页而不是媒体直链。
    static bool isPlaybackPage(const QUrl &url);

private:
    struct CachedResolution
    {
        QUrl mediaUrl;
        qint64 expiresAtMs = 0;
    };

    void finishResolve(QNetworkReply *reply, const MediaItem &original, const QUrl &pageUrl,
                       const Callback &callback);
    void rememberResolution(const QUrl &pageUrl, const QUrl &mediaUrl);

    HttpClient *m_http;
    /// 同一次播放会话里反复点同一集时不再重复请求播放页。
    /// 播放页直链常带签名/时效，必须有 TTL 和数量上限，不能永久缓存。
    QHash<QString, CachedResolution> m_cache;
    static constexpr qint64 kCacheTtlMs = 30 * 60 * 1000;
    static constexpr int kMaxCacheEntries = 128;
};

#endif // MEDIAPAGERESOLVER_H
