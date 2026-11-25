/// @file DownloadPaths.cpp
/// @brief 下载目录、去重文件名与扩展名选择实现。
#include "download/DownloadPaths.h"

#include "util/MediaExtensions.h"
#include "util/PlaylistSniffer.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace {

/// @brief 转为绝对、干净的路径，便于跨写法比较。
QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

/// @brief 最终文件、续传半成品或 HLS 中间文件任一存在，都视为路径已占用。
bool occupiedOnDisk(const QString &path)
{
    return QFileInfo::exists(path)
        || QFileInfo::exists(path + QStringLiteral(".mvpdownload"))
        || QFileInfo::exists(path + QStringLiteral(".ts"))
        || QFileInfo::exists(path + QStringLiteral(".ts.mvpdownload"));
}

/// @brief 根据 URL 路径推断扩展名；识别为播放列表时统一返回 ts。
QString normalizedSuffix(const QUrl &url)
{
    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (PlaylistSniffer::metadataIndicatesPlaylist(url, QString()) || suffix == QLatin1String("m3u8"))
        return QStringLiteral("ts");
    return suffix;
}

} // namespace

namespace DownloadPaths {

/// @brief 返回下载根目录下按媒体类型划分的子目录。
/// @param root 下载根目录；空字符串原样返回，避免生成绝对子路径。
/// @param video true 返回“影视”，false 返回“音乐”。
QString mediaDirectory(const QString &root, bool video)
{
    if (root.trimmed().isEmpty())
        return root;
    return QDir(root).filePath(video ? QStringLiteral("影视") : QStringLiteral("音乐"));
}

/// @brief 生成不与磁盘/队列冲突的最终文件路径。
/// @param directory 目标目录。
/// @param fileName 首选文件名。
/// @param inUse 队列占用查询函数，可为空。
/// @return 可用绝对路径；连续 2 万次常规候选 + 8 次 UUID 兜底仍冲突时返回空。
/// @note 同名优先追加 “ (1)”“ (2)”，避免覆盖用户文件或另一个任务。
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
    // 最后一级 UUID 兜底同样检查磁盘/队列占用，不能直接返回固定
    // “(copy)” 路径，否则极端情况下仍会覆盖已有文件或另一个任务的半成品。
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

/// @brief 判断两个路径是否指向同一文件。
/// @note 先转绝对干净路径；Windows 文件系统大小写不敏感，按不敏感比较。
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

/// @brief 根据 URL 和媒体类型选择下载扩展名。
/// @param url 源地址。
/// @param hls true 时统一先落 .ts，完成后由 MediaRemux 转 MP4。
/// @param video true 为影视，false 为音乐。
/// @return 已知后缀原样返回；未知时影视回退 mp4、音乐回退 mp3。
QString suffixForDownload(const QUrl &url, bool hls, bool video)
{
    if (hls)
        return QStringLiteral("ts");

    const QString suffix = normalizedSuffix(url);
    if (MediaExtensions::isDownloadVideoSuffix(suffix)
        || MediaExtensions::isDownloadAudioSuffix(suffix)) {
        return suffix;
    }
    return video ? QStringLiteral("mp4") : QStringLiteral("mp3");
}

} // namespace DownloadPaths
