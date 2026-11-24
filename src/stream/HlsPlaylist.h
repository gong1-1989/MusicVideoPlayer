#ifndef HLSPLAYLIST_H
#define HLSPLAYLIST_H

#include <QByteArray>
#include <QList>
#include <QSharedPointer>
#include <QString>
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

    /// 主播放列表里的一个视频码率变体。
    /// index 是它在原始 master 中的顺序号，不是按带宽排序后的位置；
    /// 用户选择、代理裁剪和“当前播放”匹配都必须使用这个稳定序号。
    struct Variant
    {
        int index = -1;
        int bandwidth = 0;
        QString resolution;
        QString codecs;
        QString audioGroup;
        QUrl uri;
    };

    /// 解析原始 m3u8，记录每个媒体分片及其起始时间；只处理完整媒体列表。
    /// 返回空指针表示这不是可预取的媒体列表（master 列表或分片为空）。
    static QSharedPointer<Info> parse(const QByteArray &body, const QUrl &playlistUrl);

    /// 按起始时间二分定位目标分片；空列表返回 -1。
    static int segmentIndexAt(const Info &info, qint64 positionMs);

    /// 同一个视频的 master 列表和子列表通常共享目录，最多再差一层码率目录。
    static bool sameScope(const QUrl &a, const QUrl &b);

    /// 解析主列表中的视频码率变体。正文不是 master 或没有有效 HTTP(S)
    /// 子列表时返回空列表；变体序号按原始文件顺序保留。
    static QList<Variant> variants(const QByteArray &body, const QUrl &playlistUrl);

    /// 只保留指定序号的视频变体，其余 #EXT-X-STREAM-INF 及其 URI 行删除。
    /// 全局标签（#EXT-X-MEDIA、SESSION-KEY、VERSION 等）原样保留，
    /// 保证裁剪后独立音轨和加密信息仍可用。序号无效时返回原文。
    static QByteArray restrictToVariant(const QByteArray &body, const QUrl &playlistUrl,
                                        int variantIndex);

    using ProxyResolver = std::function<QUrl(const QUrl &)>;

    /// 把相对/绝对分片、子播放列表、key、init 段改写成代理地址。
    /// proxyFor 负责把一个上游 URL 登记成 token 并返回本地目录 URL。
    /// 任何非 HTTP(S) URI（file/UNC/smb/data 等）都会拒绝整份播放列表，
    /// 不能让播放器绕过代理直接访问本机或局域网资源。
    static QByteArray rewrite(const QByteArray &body, const QUrl &playlistUrl,
                              const ProxyResolver &proxyFor, QString *error = nullptr);
};

#endif // HLSPLAYLIST_H
