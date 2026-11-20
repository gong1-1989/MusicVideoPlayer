#ifndef MEDIAITEM_H
#define MEDIAITEM_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>

enum class MediaType {
    Music,
    Video
};

/// 一条可播放的媒体记录。本地文件与在线流共用同一结构，
/// 区别只在 url 的 scheme（file:// 还是 http/https）。
struct MediaItem
{
    MediaType type = MediaType::Music;
    QString title;
    QString artist;
    QUrl url;
    /// 备用播放地址。采集接口常用 `$$$` 分隔多条线路，同一集可能同时有
    /// m3u8 和 mp4；播放时优先选 mp4/flv 这类可按字节定位的直链。
    QStringList alternateUrls;
    QUrl lyricUrl;
    QUrl webUrl;                    // 原始页面，便于回看来源
    QString sourceId;               // 来自哪个搜索源（本地文件为空）
    QHash<QString, QString> headers; // 播放/下载要补的请求头（Referer、Cookie 等）
    bool online = false;
    qint64 durationMs = 0;

    // 影片信息（音乐源可能只填一部分，全部可空）
    QUrl posterUrl;                 // 海报 / 封面
    QString year;                   // 年份，如 "2019"
    QString area;                   // 地区，如 "中国大陆"
    QString genre;                  // 类型，如 "剧情 / 悬疑"
    QString description;            // 简介

    bool isValid() const { return !url.isEmpty() && url.isValid(); }

    QString displayName() const
    {
        if (artist.isEmpty())
            return title;
        return QStringLiteral("%1 - %2").arg(title, artist);
    }

    /// 列表行第二段信息：年份 · 地区 · 类型，没有的字段自动跳过
    QString metaLine() const
    {
        QStringList parts;
        if (!year.isEmpty())
            parts.append(year);
        if (!area.isEmpty())
            parts.append(area);
        if (!genre.isEmpty())
            parts.append(genre);
        return parts.join(QStringLiteral(" · "));
    }

    /// 悬停提示用的多行摘要
    QString detailText() const
    {
        QStringList lines;
        lines.append(displayName());
        const QString meta = metaLine();
        if (!meta.isEmpty())
            lines.append(meta);
        if (!description.isEmpty()) {
            QString summary = description.simplified();
            if (summary.size() > 260)
                summary = summary.left(260) + QStringLiteral("…");
            lines.append(summary);
        }
        if (!url.isEmpty())
            lines.append(online ? url.toString() : url.toLocalFile());
        return lines.join(QLatin1Char('\n'));
    }
};

Q_DECLARE_METATYPE(MediaItem)

/// 可按字节 Range 定位的直链视频格式。HLS 只能按分片定位，深拖动时
/// 目标分片慢会把整个等待放大；同一条目有 mp4/flv 时应优先用它。
inline bool isProgressiveVideoUrl(const QUrl &url)
{
    const QString path = url.path().toLower();
    return path.endsWith(QLatin1String(".mp4")) || path.endsWith(QLatin1String(".m4v"))
        || path.endsWith(QLatin1String(".mov")) || path.endsWith(QLatin1String(".flv"))
        || path.endsWith(QLatin1String(".webm"));
}

inline bool isHlsUrl(const QUrl &url)
{
    const QString path = url.path().toLower();
    return path.endsWith(QLatin1String(".m3u8"))
        || url.toString().toLower().contains(QLatin1String(".m3u8"));
}

/// 条目实际播放地址：视频优先 mp4/flv，找不到再回退原地址；
/// 备选地址无效时不会把播放地址换成空值。
inline QUrl preferredPlaybackUrl(const MediaItem &item)
{
    if (!item.url.isEmpty() && item.url.isValid()
        && (item.type != MediaType::Video || isProgressiveVideoUrl(item.url))) {
        return item.url;
    }

    for (const QString &raw : item.alternateUrls) {
        const QUrl candidate(raw);
        if (candidate.isEmpty() || !candidate.isValid())
            continue;
        if (isProgressiveVideoUrl(candidate))
            return candidate;
    }
    return item.url;
}

/// 去重用的身份标识：
/// - 本地文件看路径；
/// - 在线条目优先看原始网页地址（稳定），再带上标题——
///   采集接口一季剧共用同一个详情页，只有标题能区分集数；
/// - 没有网页地址时退回去掉查询串的直链（B站等直链带过期签名，每次搜索都不一样）。
inline QString mediaIdentity(const MediaItem &item)
{
    if (!item.online)
        return QStringLiteral("file:") + item.url.toLocalFile();

    if (!item.webUrl.isEmpty() && item.webUrl.isValid()) {
        return QStringLiteral("web:") + item.webUrl.toString() + QLatin1Char('#')
               + item.title;
    }

    QUrl bare = item.url;
    bare.setQuery(QString());
    return QStringLiteral("url:") + bare.toString();
}

// 持久化用：播放列表要跨会话保留，这里只做字段级序列化，不引入额外格式文件
inline QJsonObject mediaItemToJson(const MediaItem &item)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), item.type == MediaType::Video ? 1 : 0);
    object.insert(QStringLiteral("title"), item.title);
    if (!item.artist.isEmpty())
        object.insert(QStringLiteral("artist"), item.artist);
    object.insert(QStringLiteral("url"), item.url.toString());
    if (!item.alternateUrls.isEmpty()) {
        QJsonArray alternatives;
        for (const QString &url : item.alternateUrls)
            alternatives.append(url);
        object.insert(QStringLiteral("altUrls"), alternatives);
    }
    if (!item.lyricUrl.isEmpty())
        object.insert(QStringLiteral("lyricUrl"), item.lyricUrl.toString());
    if (!item.webUrl.isEmpty())
        object.insert(QStringLiteral("webUrl"), item.webUrl.toString());
    if (!item.sourceId.isEmpty())
        object.insert(QStringLiteral("sourceId"), item.sourceId);
    if (!item.headers.isEmpty()) {
        QJsonObject headers;
        for (auto it = item.headers.cbegin(); it != item.headers.cend(); ++it)
            headers.insert(it.key(), it.value());
        object.insert(QStringLiteral("headers"), headers);
    }
    object.insert(QStringLiteral("online"), item.online);
    if (item.durationMs > 0)
        object.insert(QStringLiteral("duration"), static_cast<double>(item.durationMs));
    // 影片信息：只写有值的字段，旧版列表文件不受影响
    if (!item.posterUrl.isEmpty())
        object.insert(QStringLiteral("posterUrl"), item.posterUrl.toString());
    if (!item.year.isEmpty())
        object.insert(QStringLiteral("year"), item.year);
    if (!item.area.isEmpty())
        object.insert(QStringLiteral("area"), item.area);
    if (!item.genre.isEmpty())
        object.insert(QStringLiteral("genre"), item.genre);
    if (!item.description.isEmpty())
        object.insert(QStringLiteral("description"), item.description);
    return object;
}

inline MediaItem mediaItemFromJson(const QJsonObject &object)
{
    MediaItem item;
    item.type = object.value(QStringLiteral("type")).toInt() == 1 ? MediaType::Video
                                                                 : MediaType::Music;
    item.title = object.value(QStringLiteral("title")).toString();
    item.artist = object.value(QStringLiteral("artist")).toString();
    item.url = QUrl(object.value(QStringLiteral("url")).toString());
    const QJsonArray altUrls = object.value(QStringLiteral("altUrls")).toArray();
    for (const QJsonValue &value : altUrls) {
        const QString raw = value.toString();
        if (!raw.isEmpty())
            item.alternateUrls.append(raw);
    }
    item.lyricUrl = QUrl(object.value(QStringLiteral("lyricUrl")).toString());
    item.webUrl = QUrl(object.value(QStringLiteral("webUrl")).toString());
    item.sourceId = object.value(QStringLiteral("sourceId")).toString();
    const QJsonObject headers = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        item.headers.insert(it.key(), it.value().toString());
    item.online = object.value(QStringLiteral("online")).toBool();
    item.durationMs = static_cast<qint64>(object.value(QStringLiteral("duration")).toDouble());
    item.posterUrl = QUrl(object.value(QStringLiteral("posterUrl")).toString());
    item.year = object.value(QStringLiteral("year")).toString();
    item.area = object.value(QStringLiteral("area")).toString();
    item.genre = object.value(QStringLiteral("genre")).toString();
    item.description = object.value(QStringLiteral("description")).toString();
    return item;
}

#endif // MEDIAITEM_H
