#ifndef MEDIAITEM_H
#define MEDIAITEM_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "util/HeaderPolicy.h"

enum class MediaType {
    Music,
    Video
};

/// 把影视站/音乐源五花八门的清晰度写法归一成短标签。
/// 只认明确的规格词，不把“更新至第 12 集”这类备注当成品质。
inline QString normalizedQuality(const QString &raw, MediaType type)
{
    const QString text = raw.toUpper();
    if (text.isEmpty())
        return QString();
    if (text.contains(QLatin1String("4K")) || text.contains(QLatin1String("2160"))
        || text.contains(QLatin1String("UHD"))) {
        return text.contains(QLatin1String("HDR")) ? QStringLiteral("4K HDR")
                                                   : QStringLiteral("4K");
    }
    if (text.contains(QLatin1String("HDR")))
        return QStringLiteral("HDR");
    if (text.contains(QLatin1String("1080")) || text.contains(QLatin1String("FHD"))
        || text.contains(QStringLiteral("蓝光")) || text.contains(QStringLiteral("BLURAY")))
        return QStringLiteral("1080P");
    if (text.contains(QLatin1String("720")))
        return QStringLiteral("720P");
    if (text.contains(QStringLiteral("杜比")) || text.contains(QLatin1String("DOLBY")))
        return QStringLiteral("杜比");
    if (type == MediaType::Music) {
        if (text.contains(QStringLiteral("无损")) || text.contains(QLatin1String("FLAC"))
            || text.contains(QLatin1String("APE")) || text.contains(QLatin1String("WAV"))
            || text.contains(QLatin1String("HI-RES")) || text.contains(QLatin1String("HIRES"))) {
            return QStringLiteral("无损");
        }
        if (text.contains(QLatin1String("320")) || text.contains(QLatin1String("HQ"))
            || text.contains(QStringLiteral("高品质"))) {
            return QStringLiteral("HQ");
        }
    }
    if (text.contains(QStringLiteral("超清")) || text.contains(QStringLiteral("蓝光")))
        return QStringLiteral("超清");
    if (text.contains(QStringLiteral("高清")) || text.contains(QLatin1String("HD")))
        return QStringLiteral("高清");
    if (text.contains(QStringLiteral("标清")))
        return QStringLiteral("标清");
    return QString();
}

/// 没有独立品质字段的来源从标题/简介/直链里推断，避免搜索结果品质栏长期空着。
inline QString inferQuality(const QString &text, MediaType type)
{
    QString inferred = normalizedQuality(text, type);
    if (!inferred.isEmpty())
        return inferred;
    const QString lower = text.toLower();
    if (type == MediaType::Music
        && (lower.contains(QLatin1String(".flac")) || lower.contains(QLatin1String(".wav"))
            || lower.contains(QLatin1String("lossless")))) {
        return QStringLiteral("无损");
    }
    return QString();
}

/// 从接口的 streamUrl 字段映射推断音频内容范围。
/// 只做保守识别：字段名明确叫 previewUrl 才是试听，
/// Radio Browser 的 url_resolved 才是电台；其他都按完整曲目处理。
/// 详情接口的字段一并参与判断，避免主接口无直链、详情接口才给试听地址时漏标。
inline QString inferStreamScope(const QString &streamField,
                                const QString &detailStreamField = QString())
{
    const QString fields = (streamField + QLatin1Char(' ') + detailStreamField).toLower();
    if (fields.contains(QLatin1String("previewurl")))
        return QStringLiteral("preview");
    if (fields.contains(QLatin1String("url_resolved")))
        return QStringLiteral("radio");
    return QString();
}

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
    QUrl sourceOrigin;              // 源站地址，用于裁剪 Cookie/Authorization 作用域
    QString sourceId;               // 来自哪个搜索源（本地文件为空）
    QHash<QString, QString> headers; // 播放/下载要补的请求头（Referer 等；敏感头按目标 host 过滤）
    bool online = false;
    /// 搜索结果临时条目。只用于当前会话播放，不参与持久化；
    /// 用户明确“加入列表”或下载完成后会清除该标记。
    bool previewOnly = false;
    /// 音频内容语义，与 previewOnly 不是一回事：
    /// - preview：试听片段（如 iTunes previewUrl），不是完整曲目；
    /// - radio：电台/直播流，不是单曲；
    /// - 空：默认完整曲目或普通直链。
    /// 该字段只描述内容范围，不决定条目是否持久化。
    QString streamScope;
    qint64 durationMs = 0;
    /// 视频已播放位置，随播放列表条目一起持久化；0 表示从头播放。
    /// 删除列表条目时进度随条目一起消失，音乐不使用该字段。
    qint64 positionMs = 0;

    // 影片信息（音乐源可能只填一部分，全部可空）
    QUrl posterUrl;                 // 海报 / 封面
    QString year;                   // 年份，如 "2019"
    QString area;                   // 地区，如 "中国大陆"
    QString genre;                  // 类型，如 "剧情 / 悬疑"
    QString description;            // 简介
    /// 清晰度/音质标签：4K、1080P、HDR、无损、HQ 等。
    /// 影视站vod_remarks里有就原样归一化；音乐源没有独立字段时从标题推断。
    QString quality;

    bool isValid() const { return !url.isEmpty() && url.isValid(); }

    QString displayName() const
    {
        if (artist.isEmpty())
            return title;
        return QStringLiteral("%1 - %2").arg(title, artist);
    }

    /// 内容范围标签：试听/电台；完整曲目返回空，不强行加“完整”字样。
    QString streamScopeLabel() const
    {
        if (streamScope == QLatin1String("preview"))
            return QStringLiteral("试听");
        if (streamScope == QLatin1String("radio"))
            return QStringLiteral("电台");
        return QString();
    }

    /// 列表行第二段信息：试听/电台 · 年份 · 地区 · 类型，没有的字段自动跳过
    QString metaLine() const
    {
        QStringList parts;
        const QString scope = streamScopeLabel();
        if (!scope.isEmpty())
            parts.append(scope);
        if (!quality.isEmpty())
            parts.append(quality);
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
    // 只认 path 后缀：query 里出现 .m3u8 的 MP4 不应被当成 HLS，
    // 否则会走分片/播放列表分支，直接 502 或误判缓存类型。
    return url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
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

/// UI 是否应该按 HLS 处理定位等待：必须看实际播放地址，而不是原始 URL。
/// 原始地址是 m3u8、但备选有 mp4 时，播放器走 mp4，不应该进入分片定位。
inline bool playbackIsHls(const MediaItem &item)
{
    return isHlsUrl(preferredPlaybackUrl(item));
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

/// 按 mediaIdentity 保序去重。搜索结果分页和播放列表恢复必须用同一规则，
/// 否则模型里隐藏了跨源重复项，标题/页数仍按原始条数计算。
inline QVector<MediaItem> dedupeMediaItems(const QVector<MediaItem> &items)
{
    QVector<MediaItem> unique;
    unique.reserve(items.size());
    QSet<QString> seen;
    for (const MediaItem &item : items) {
        const QString identity = mediaIdentity(item);
        if (seen.contains(identity))
            continue;
        seen.insert(identity);
        unique.append(item);
    }
    return unique;
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
    if (!item.sourceOrigin.isEmpty())
        object.insert(QStringLiteral("sourceOrigin"), item.sourceOrigin.toString());
    if (!item.sourceId.isEmpty())
        object.insert(QStringLiteral("sourceId"), item.sourceId);
    const QHash<QString, QString> safeHeaders = HeaderPolicy::persistedHeaders(item.headers);
    if (!safeHeaders.isEmpty()) {
        QJsonObject headers;
        for (auto it = safeHeaders.cbegin(); it != safeHeaders.cend(); ++it)
            headers.insert(it.key(), it.value());
        object.insert(QStringLiteral("headers"), headers);
    }
    object.insert(QStringLiteral("online"), item.online);
    if (!item.streamScope.isEmpty())
        object.insert(QStringLiteral("streamScope"), item.streamScope);
    if (item.durationMs > 0)
        object.insert(QStringLiteral("duration"), static_cast<double>(item.durationMs));
    // 视频进度直接写进播放列表条目：条目删除后进度自然一起删除。
    if (item.type == MediaType::Video && item.positionMs > 0)
        object.insert(QStringLiteral("position"), static_cast<double>(item.positionMs));
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
    if (!item.quality.isEmpty())
        object.insert(QStringLiteral("quality"), item.quality);
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
    item.sourceOrigin = QUrl(object.value(QStringLiteral("sourceOrigin")).toString());
    item.sourceId = object.value(QStringLiteral("sourceId")).toString();
    const QJsonObject headers = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        // 旧版列表可能把 Cookie/Authorization 明文写进来了，读取时直接丢弃，
        // 避免历史数据继续把凭据带进内存和下一次保存。
        if (!HeaderPolicy::isSensitiveHeader(it.key()))
            item.headers.insert(it.key(), it.value().toString());
    }
    item.online = object.value(QStringLiteral("online")).toBool();
    const QString scope = object.value(QStringLiteral("streamScope")).toString();
    if (scope == QLatin1String("preview") || scope == QLatin1String("radio"))
        item.streamScope = scope;
    item.durationMs = static_cast<qint64>(object.value(QStringLiteral("duration")).toDouble());
    if (item.type == MediaType::Video)
        item.positionMs = static_cast<qint64>(object.value(QStringLiteral("position")).toDouble());
    item.posterUrl = QUrl(object.value(QStringLiteral("posterUrl")).toString());
    item.year = object.value(QStringLiteral("year")).toString();
    item.area = object.value(QStringLiteral("area")).toString();
    item.genre = object.value(QStringLiteral("genre")).toString();
    item.description = object.value(QStringLiteral("description")).toString();
    item.quality = object.value(QStringLiteral("quality")).toString();
    return item;
}

#endif // MEDIAITEM_H
