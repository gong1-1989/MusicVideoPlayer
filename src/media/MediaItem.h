#ifndef MEDIAITEM_H
#define MEDIAITEM_H

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
    item.online = object.value(QStringLiteral("online")).toBool();
    item.durationMs = static_cast<qint64>(object.value(QStringLiteral("duration")).toDouble());
    return item;
}

#endif // MEDIAITEM_H
