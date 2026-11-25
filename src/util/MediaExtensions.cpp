#include "util/MediaExtensions.h"

#include <QFileInfo>
#include <QSet>
#include <QUrl>

namespace {

bool contains(const QSet<QString> &suffixes, const QString &suffix)
{
    return !suffix.isEmpty() && suffixes.contains(suffix);
}

} // namespace

namespace MediaExtensions {

QString suffixFromPath(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

QString suffixFromUrl(const QUrl &url)
{
    return suffixFromPath(url.path());
}

bool isCacheSafeSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("mp3"),  QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("oga"),  QStringLiteral("opus"), QStringLiteral("mp4"),
        QStringLiteral("m4v"),  QStringLiteral("flv"), QStringLiteral("webm"),
        QStringLiteral("mov"),  QStringLiteral("mkv"), QStringLiteral("ts"),
    };
    return contains(suffixes, suffix);
}

bool isAudioSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("wma"),
    };
    return contains(suffixes, suffix);
}

bool isDownloadVideoSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("mov"), QStringLiteral("flv"),
        QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("ts"),
    };
    return contains(suffixes, suffix);
}

bool isDownloadAudioSuffix(const QString &suffix)
{
    return isAudioSuffix(suffix);
}

bool isMediaContentSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("mp3"),  QStringLiteral("mp4"),  QStringLiteral("m4a"),
        QStringLiteral("m4v"),  QStringLiteral("mov"),  QStringLiteral("flv"),
        QStringLiteral("ts"),   QStringLiteral("m4s"),  QStringLiteral("aac"),
        QStringLiteral("ogg"),  QStringLiteral("oga"),  QStringLiteral("opus"),
        QStringLiteral("wav"),  QStringLiteral("flac"), QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("avi"),  QStringLiteral("wmv"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("m3u8"),
    };
    return contains(suffixes, suffix);
}

bool isDirectLinkSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("mp3"),  QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("mov"),  QStringLiteral("webm"), QStringLiteral("mkv"),
        QStringLiteral("flv"),  QStringLiteral("ts"),  QStringLiteral("m3u8"),
        QStringLiteral("mpd"),
    };
    return contains(suffixes, suffix);
}

bool isLosslessAudioSuffix(const QString &suffix)
{
    static const QSet<QString> suffixes = {
        QStringLiteral("flac"), QStringLiteral("ape"), QStringLiteral("wav"),
        QStringLiteral("aiff"), QStringLiteral("alac"), QStringLiteral("dsf"),
    };
    return contains(suffixes, suffix);
}

} // namespace MediaExtensions
