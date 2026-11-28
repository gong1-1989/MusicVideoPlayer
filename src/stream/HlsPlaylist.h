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

    struct Key
    {
        bool enabled = false;
        QString method;              ///< 已转大写的 METHOD；NONE 表示恢复明文
        QUrl url;                    ///< 已按 BASE 解析的密钥地址
        QByteArray iv;
        QString unsupported;         ///< 非空表示 SAMPLE-AES 等不支持的方式
        QString error;               ///< 结构错误；仅供严格解析模式使用
    };

    /// 媒体列表中的一个请求单元：普通分片，或 #EXT-X-MAP 初始化段。
    struct MediaSegment
    {
        QUrl url;
        QByteArray range;
        bool initSegment = false;
        qint64 startMs = -1;         ///< 普通分片的时间轴起点；初始化段为 -1
        double durationSeconds = 0.0;
        Key key;                     ///< 该分片生效的加密信息；初始化段始终为空
    };

    struct MediaInfo
    {
        QUrl source;
        QUrl base;                   ///< 最终 #EXT-X-BASE 解析出的基准地址
        QList<MediaSegment> segments;
        qint64 totalMs = 0;
        qint64 mediaSequence = 0;
        bool hasEndList = false;
        QString playlistType;        ///< VOD / EVENT / 空
        bool fragmentedMp4 = false;
        bool hasByteRange = false;
        bool hasNonHttp = false;
    };

    struct MediaParseOptions
    {
        /// 播放器侧解析要求每个分片前必须有 EXTINF；下载侧为兼容旧行为可关闭。
        bool requireExtinf = false;
        /// 严格模式：BASE/KEY/BYTERANGE/MAP/非 HTTP 分片直接报错；
        /// 非严格模式只跳过问题项，供播放代理尽力解析。
        bool strict = false;
        /// 主列表继承下来的 SESSION-KEY。
        Key initialKey;
        /// KEY/SESSION-KEY 解析回调；为空时忽略加密信息。
        std::function<Key(const QString &line, const QUrl &base)> keyParser;
    };

    /// 解析一条 #EXT-X-KEY 或 #EXT-X-SESSION-KEY。
    /// 只接受 HTTP(S) 的 identity AES-128；非 NONE 但不受支持的加密方式写入
    /// unsupported，结构/地址错误写入 error。下载与缓存两条链路共用此规则，
    /// 避免 KEY 字段的兼容性判断各自演化。
    static Key parseKeyLine(const QString &line, const QUrl &baseUrl);

    /// 解析原始 m3u8，记录每个媒体分片及其起始时间；只处理完整媒体列表。
    /// 返回空指针表示这不是可预取的媒体列表（master 列表或分片为空）。
    static QSharedPointer<Info> parse(const QByteArray &body, const QUrl &playlistUrl);

    /// 共享的媒体列表结构解析：BASE、EXTINF、BYTERANGE、MAP、KEY、ENDLIST。
    /// 下载任务与播放代理都从这里取分片/时间轴，避免两套解析规则漂移。
    static QSharedPointer<MediaInfo> parseMedia(const QByteArray &body,
                                                const QUrl &playlistUrl,
                                                const MediaParseOptions &options,
                                                QString *error = nullptr);

    /// 计算分片结构指纹：容器类型、分片顺序、分片地址（忽略会轮换的签名
    /// 查询参数）、BYTERANGE 与每个分片生效的 KEY 都会参与。
    /// 下载任务的续传校验和缓存完成轨道的复用校验必须使用同一份算法。
    static QString fingerprint(const QList<MediaSegment> &segments, bool fragmentedMp4);

    /// 按起始时间二分定位目标分片；空列表返回 -1。
    static int segmentIndexAt(const Info &info, qint64 positionMs);

    /// 同一个视频的 master 列表和子列表通常共享目录，最多再差一层码率目录。
    static bool sameScope(const QUrl &a, const QUrl &b);

    /// 解析主列表中的视频码率变体。正文不是 master 或没有有效 HTTP(S)
    /// 子列表时返回空列表；变体序号按原始文件顺序保留。
    /// 下载链路用它按 BANDWIDTH 选择最高码率，因此变体解析必须保留。
    static QList<Variant> variants(const QByteArray &body, const QUrl &playlistUrl);

    using ProxyResolver = std::function<QUrl(const QUrl &)>;

    /// 把相对/绝对分片、子播放列表、key、init 段改写成代理地址。
    /// proxyFor 负责把一个上游 URL 登记成 token 并返回本地目录 URL。
    /// 任何非 HTTP(S) URI（file/UNC/smb/data 等）都会拒绝整份播放列表，
    /// 不能让播放器绕过代理直接访问本机或局域网资源。
    static QByteArray rewrite(const QByteArray &body, const QUrl &playlistUrl,
                              const ProxyResolver &proxyFor, QString *error = nullptr);
};

#endif // HLSPLAYLIST_H
