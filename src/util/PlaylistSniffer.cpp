#include "util/PlaylistSniffer.h"

namespace PlaylistSniffer {

bool metadataIndicatesPlaylist(const QUrl &url, const QString &contentType)
{
    if (contentType.toLower().contains(QLatin1String("mpegurl")))
        return true;
    if (url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return true;
    return url.toString().contains(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

bool bodyIsPlaylist(const QByteArray &body)
{
    QByteArray head = body;
    if (head.startsWith("\xEF\xBB\xBF"))
        head.remove(0, 3);

    int offset = 0;
    while (offset < head.size()
           && (head.at(offset) == ' ' || head.at(offset) == '\t'
               || head.at(offset) == '\r' || head.at(offset) == '\n')) {
        ++offset;
    }
    if (offset >= head.size())
        return false;

    static const QByteArray magic = QByteArrayLiteral("#EXTM3U");
    return head.mid(offset, magic.size()) == magic;
}

} // namespace PlaylistSniffer
