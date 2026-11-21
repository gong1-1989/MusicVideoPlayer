#include "download/DownloadPaths.h"

#include "util/PlaylistSniffer.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace {

QString normalizedSuffix(const QUrl &url)
{
    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (PlaylistSniffer::metadataIndicatesPlaylist(url, QString()) || suffix == QLatin1String("m3u8"))
        return QStringLiteral("ts");
    return suffix;
}

} // namespace

namespace DownloadPaths {

QString uniqueFilePath(const QString &directory, const QString &fileName)
{
    const QDir dir(directory);
    const QString base = dir.absoluteFilePath(fileName);
    if (!QFileInfo::exists(base))
        return base;

    const QFileInfo original(base);
    const QString stem = original.completeBaseName();
    const QString suffix = original.suffix();
    const QString tail = suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;
    for (int i = 1; i < 10000; ++i) {
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(tail));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return dir.absoluteFilePath(
        QStringLiteral("%1 (copy)%2").arg(stem, tail));
}

QString suffixForDownload(const QUrl &url, bool hls, bool video)
{
    if (hls)
        return QStringLiteral("ts");

    const QString suffix = normalizedSuffix(url);
    static const QSet<QString> videoSuffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("mov"), QStringLiteral("flv"),
        QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("ts"),
    };
    static const QSet<QString> audioSuffixes = {
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("wma"),
    };
    if (videoSuffixes.contains(suffix))
        return suffix;
    if (audioSuffixes.contains(suffix))
        return suffix;
    return video ? QStringLiteral("mp4") : QStringLiteral("mp3");
}

} // namespace DownloadPaths
