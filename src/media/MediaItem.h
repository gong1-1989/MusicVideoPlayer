#ifndef MEDIAITEM_H
#define MEDIAITEM_H

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

#endif // MEDIAITEM_H
