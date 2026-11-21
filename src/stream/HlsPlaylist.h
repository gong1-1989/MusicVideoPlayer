#ifndef HLSPLAYLIST_H
#define HLSPLAYLIST_H

#include <QByteArray>
#include <QList>
#include <QSharedPointer>
#include <QUrl>

#include <functional>

/// m3u8 的解析与改写。
///
/// StreamProxy 只关心“把上游播放列表变成可代理的本地播放列表”，
/// 分片索引和 URI 重写放在这里，便于单独测试。
class HlsPlaylist
{
public:
    struct Info
    {
        QUrl source;
        QList<QUrl> segments;
        QList<qint64> starts;
        qint64 totalMs = 0;
        qint64 lastUsedMs = 0;
        bool prefetchable = false;
    };

    /// 解析原始 m3u8，记录每个媒体分片及其起始时间；只处理完整媒体列表。
    /// 返回空指针表示这不是可预取的媒体列表（master 列表或分片为空）。
    static QSharedPointer<Info> parse(const QByteArray &body, const QUrl &playlistUrl);

    /// 按起始时间二分定位目标分片；空列表返回 -1。
    static int segmentIndexAt(const Info &info, qint64 positionMs);

    /// 同一个视频的 master 列表和子列表通常共享目录，最多再差一层码率目录。
    static bool sameScope(const QUrl &a, const QUrl &b);

    using ProxyResolver = std::function<QUrl(const QUrl &)>;

    /// 把相对/绝对分片、子播放列表、key、init 段改写成代理地址。
    /// proxyFor 负责把一个上游 URL 登记成 token 并返回本地目录 URL。
    static QByteArray rewrite(const QByteArray &body, const QUrl &playlistUrl,
                              const ProxyResolver &proxyFor);
};

#endif // HLSPLAYLIST_H
