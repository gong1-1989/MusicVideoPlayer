#include "download/DownloadPaths.h"

#include "util/PlaylistSniffer.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

namespace {

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

/// 最终文件、半成品、HLS 中间文件任意一个存在，都不能再分给同一路径。
bool occupiedOnDisk(const QString &path)
{
    return QFileInfo::exists(path)
        || QFileInfo::exists(path + QStringLiteral(".mvpdownload"))
        || QFileInfo::exists(path + QStringLiteral(".ts"))
        || QFileInfo::exists(path + QStringLiteral(".ts.mvpdownload"));
}

QString normalizedSuffix(const QUrl &url)
{
    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (PlaylistSniffer::metadataIndicatesPlaylist(url, QString()) || suffix == QLatin1String("m3u8"))
        return QStringLiteral("ts");
    return suffix;
}

} // namespace

namespace DownloadPaths {

QString mediaDirectory(const QString &root, bool video)
{
    if (root.trimmed().isEmpty())
        return root;
    return QDir(root).filePath(video ? QStringLiteral("影视") : QStringLiteral("音乐"));
}

QString uniqueFilePath(const QString &directory, const QString &fileName,
                       const PathInUse &inUse)
{
    const QDir dir(directory);
    const QString base = dir.absoluteFilePath(fileName);
    auto occupied = [&](const QString &candidate) {
        if (occupiedOnDisk(candidate))
            return true;
        return inUse
            && (inUse(candidate)
                || inUse(candidate + QStringLiteral(".mvpdownload"))
                // 队列里已有任务的最终名可能是 `foo.mp4.ts`，或 HLS 任务的
                // 中间名是 `foo.mp4.ts`；只检查最终名和 `.mvpdownload` 会
                // 让另一个任务拿到 `foo.mp4` 后与这些派生文件撞名。
                || inUse(candidate + QStringLiteral(".ts"))
                || inUse(candidate + QStringLiteral(".ts.mvpdownload")));
    };
    if (!occupied(base))
        return base;

    const QFileInfo original(base);
    const QString stem = original.completeBaseName();
    const QString suffix = original.suffix();
    const QString tail = suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;
    for (int i = 1; i < 10000; ++i) {
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(tail));
        if (!occupied(candidate))
            return candidate;
    }
    for (int i = 1; i < 10000; ++i) {
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("%1 (copy %2)%3").arg(stem).arg(i).arg(tail));
        if (!occupied(candidate))
            return candidate;
    }
    // 最后一级兜底也必须检查占用/队列预留。旧实现直接返回固定
    // “(copy)”路径，极端情况下会覆盖已有文件或另一个任务的半成品。
    for (int attempt = 0; attempt < 8; ++attempt) {
        const QString token =
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        const QString candidate =
            dir.absoluteFilePath(QStringLiteral("%1 (copy %2)%3").arg(stem, token, tail));
        if (!occupied(candidate))
            return candidate;
    }
    return QString();
}

bool pathsReferToSameFile(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    const QString left = normalizedPath(a);
    const QString right = normalizedPath(b);
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
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
