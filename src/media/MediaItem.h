#ifndef MEDIAITEM_H
#define MEDIAITEM_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QString>
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
    QUrl lyricUrl;
    QUrl webUrl;                    // 原始页面，便于回看来源
    QString sourceId;               // 来自哪个搜索源（本地文件为空）
    QHash<QString, QString> headers; // 播放/下载要补的请求头（Referer、Cookie 等）
    bool online = false;
    qint64 durationMs = 0;

    bool isValid() const { return url.isValid(); }

    QString displayName() const
    {
        if (artist.isEmpty())
            return title;
        return QStringLiteral("%1 - %2").arg(title, artist);
    }
};

Q_DECLARE_METATYPE(MediaItem)

// 持久化用：播放列表要跨会话保留，这里只做字段级序列化，不引入额外格式文件
inline QJsonObject mediaItemToJson(const MediaItem &item)
{
    QJsonObject object;
    object.insert(QStringLiteral("type"), item.type == MediaType::Video ? 1 : 0);
    object.insert(QStringLiteral("title"), item.title);
    if (!item.artist.isEmpty())
        object.insert(QStringLiteral("artist"), item.artist);
    object.insert(QStringLiteral("url"), item.url.toString());
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
    item.lyricUrl = QUrl(object.value(QStringLiteral("lyricUrl")).toString());
    item.webUrl = QUrl(object.value(QStringLiteral("webUrl")).toString());
    item.sourceId = object.value(QStringLiteral("sourceId")).toString();
    const QJsonObject headers = object.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        item.headers.insert(it.key(), it.value().toString());
    item.online = object.value(QStringLiteral("online")).toBool();
    item.durationMs = static_cast<qint64>(object.value(QStringLiteral("duration")).toDouble());
    return item;
}

#endif // MEDIAITEM_H
