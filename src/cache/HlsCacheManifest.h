#ifndef HLSCACHEMANIFEST_H
#define HLSCACHEMANIFEST_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

/// HLS 缓存用的最小清单解析。
///
/// 与播放代理的 HlsPlaylist 不同，这里需要保留主列表的码率/音轨分组，
/// 并识别“整段视频就是一个 MP4、分片只是 EXT-X-BYTERANGE”的列表，
/// 让缓存可以按普通 MP4 Range 下载，而不是发 101 个分片请求。
namespace HlsCacheManifest {

struct Variant
{
    QUrl url;
    int bandwidth = 0;
    QString audioGroup;
    QString resolution;
    QString codecs;
};

struct AudioRendition
{
    QUrl url;
    QString groupId;
    QString name;
    bool isDefault = false;
};

struct Master
{
    QList<Variant> variants;
    QList<AudioRendition> audioRenditions;
    /// 主列表的 SESSION-KEY 原始行；子列表不重复写 KEY 时要靠它解密。
    QStringList sessionKeyLines;
    bool encrypted = false;
};

/// 解析主播放列表。正文没有 #EXT-X-STREAM-INF 时返回 false（不是主列表）。
bool parseMaster(const QByteArray &body, const QUrl &baseUrl, Master *out,
                 QString *error = nullptr);

struct MediaTrack
{
    bool valid = false;
    bool hasEndList = false;
    bool isVod = false;
    int segmentCount = 0;
    /// 媒体列表里出现非 NONE 的 EXT-X-KEY/SESSION-KEY。
    bool encrypted = false;
    /// 所有分片都指向同一个 URL，且每个分片都有 EXT-X-BYTERANGE。
    /// 此时整个媒体文件本身就是可直接 Range 下载的 MP4。
    bool singleFile = false;
    QUrl singleFileUrl;
};

/// 解析媒体播放列表。不是带 #EXTINF 的媒体列表时 valid=false。
bool parseMediaTrack(const QByteArray &body, const QUrl &baseUrl, MediaTrack *out,
                     QString *error = nullptr);

/// 两个地址只比较协议/host/端口/path，忽略会过期的查询串。
bool sameUrlIgnoringQuery(const QUrl &a, const QUrl &b);

} // namespace HlsCacheManifest

#endif // HLSCACHEMANIFEST_H
