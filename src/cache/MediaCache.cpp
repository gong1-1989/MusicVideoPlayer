#include "cache/MediaCache.h"
#include "core/FileNameUtils.h"

#include "storage/StoragePaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrlQuery>

namespace {
constexpr int kMetaVersion = 2;

QString safeExtensionFromUrl(const QUrl &url, const QByteArray &contentType = QByteArray())
{
    static const QStringList known = {
        QStringLiteral("mp3"),  QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("oga"),  QStringLiteral("opus"), QStringLiteral("mp4"),
        QStringLiteral("m4v"),  QStringLiteral("flv"), QStringLiteral("webm"),
        QStringLiteral("mov"),  QStringLiteral("mkv"), QStringLiteral("ts"),
    };
    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (!known.contains(suffix)) {
        // 网易云这类外链把真实文件名放在查询串里：
        // `.../outer/url?id=123.mp3`。只按 path 取后缀会得到 "url"。
        const QUrlQuery query(url);
        for (const auto &pair : query.queryItems(QUrl::FullyDecoded)) {
            const QString candidate = QFileInfo(pair.second).suffix().toLower();
            if (known.contains(candidate)) {
                suffix = candidate;
                break;
            }
        }
    }
    if (!known.contains(suffix) && !contentType.isEmpty()) {
        // CDN 经常给无后缀签名地址；Content-Type 是最后一道可靠线索。
        const QString type = QString::fromLatin1(contentType).toLower();
        if (type.contains(QStringLiteral("mpeg")) && type.contains(QStringLiteral("audio")))
            suffix = QStringLiteral("mp3");
        else if (type.contains(QStringLiteral("audio/mp4")))
            suffix = QStringLiteral("m4a");
        else if (type.contains(QStringLiteral("flac")))
            suffix = QStringLiteral("flac");
        else if (type.contains(QStringLiteral("wav")))
            suffix = QStringLiteral("wav");
        else if (type.contains(QStringLiteral("ogg")))
            suffix = QStringLiteral("ogg");
        else if (type.contains(QStringLiteral("webm")))
            suffix = QStringLiteral("webm");
        else if (type.contains(QStringLiteral("flv")))
            suffix = QStringLiteral("flv");
        else if (type.contains(QStringLiteral("mp2t")))
            suffix = QStringLiteral("ts");
        else if (type.contains(QStringLiteral("video/mp4")) || type.contains(QStringLiteral("m4v")))
            suffix = QStringLiteral("mp4");
        else if (type.contains(QStringLiteral("quicktime")))
            suffix = QStringLiteral("mov");
    }
    if (!known.contains(suffix))
        suffix = QStringLiteral("bin");
    return QLatin1Char('.') + suffix;
}

QString safeEntryName(const QString &raw, MediaType type)
{
    return FileNameUtils::sanitizeComponent(
        raw, type == MediaType::Video ? QStringLiteral("影视") : QStringLiteral("音乐"), 120);
}

QString identityDigest(const MediaItem &item, int length)
{
    return QCryptographicHash::hash(mediaIdentity(item).toUtf8(), QCryptographicHash::Sha256)
        .toHex()
        .left(length);
}

QUrl comparableUrl(QUrl url)
{
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

/// 第一轮缓存是 `<SHA-256 前 32 位>/media.<ext> + meta.json`。
/// 新布局第一次播放同一 URL 时把数据搬到用户可读的新位置，避免升级后
/// 已经下好的歌曲/影视全部变成看不见的孤儿。
bool adoptLegacyCache(const MediaItem &item)
{
    if (!MediaCache::isCacheable(item))
        return false;
    const QString newPath = MediaCache::filePathForItem(item);
    if (QFileInfo::exists(newPath) || QFileInfo::exists(MediaCache::metaPathForFile(newPath)))
        return false;

    const QString legacyDir =
        MediaCache::rootDir() + QLatin1Char('/') + identityDigest(item, 32);
    QDir dir(legacyDir);
    if (!dir.exists())
        return false;

    QFile meta(dir.filePath(QStringLiteral("meta.json")));
    if (!meta.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(meta.readAll()).object();
    meta.close();
    if (object.value(QStringLiteral("version")).toInt() != 1)
        return false;

    const QUrl oldUrl = comparableUrl(QUrl(object.value(QStringLiteral("url")).toString()));
    if (!oldUrl.isValid() || oldUrl != comparableUrl(preferredPlaybackUrl(item)))
        return false;
    const qint64 total =
        static_cast<qint64>(object.value(QStringLiteral("total")).toDouble(-1));
    const bool complete = object.value(QStringLiteral("complete")).toBool();
    const QByteArray contentType =
        object.value(QStringLiteral("contentType")).toString().toLatin1();

    QString legacyData;
    const QFileInfoList files =
        dir.entryInfoList(QStringList{QStringLiteral("media.*")}, QDir::Files);
    for (const QFileInfo &info : files) {
        legacyData = info.absoluteFilePath();
        break;
    }
    if (legacyData.isEmpty())
        return false;

    if (!QDir().mkpath(QFileInfo(newPath).absolutePath()))
        return false;
    if (!QFile::rename(legacyData, newPath)) {
        if (!QFile::copy(legacyData, newPath))
            return false;
        QFile::remove(legacyData);
    }
    if (!MediaCache::writeState(newPath, MediaCache::cacheIdentity(item),
                                preferredPlaybackUrl(item), contentType, total, complete)) {
        // 不允许出现“正文搬走了、旁车没写成”的半套状态；搬回去让下次再试。
        QFile::rename(newPath, legacyData);
        return false;
    }
    dir.removeRecursively();
    return true;
}

QString candidateName(const QString &base, int index)
{
    return index <= 0 ? base : base + QStringLiteral(" (%1)").arg(index + 1);
}

QString resolvedCachePath(const MediaItem &item)
{
    const QString root = QDir::cleanPath(MediaCache::rootDir());
    const QString base = safeEntryName(item.displayName(), item.type);
    const QString identity = MediaCache::cacheIdentity(item);

    for (int index = 0; index < 1000; ++index) {
        const QString name = candidateName(base, index);
        QString path;
        bool reusable = false;
        if (item.type == MediaType::Video) {
            const QString folder = root + QLatin1Char('/') + name;
            // 影视条目是一个目录，正文固定叫 media，不带后缀。
            path = folder + QStringLiteral("/media");
            const QFileInfo folderInfo(folder);
            if (folderInfo.exists() && !folderInfo.isDir())
                continue;
            if (!folderInfo.exists())
                reusable = true;
        } else {
            // 音乐条目直接放缓存根目录，一个条目一个文件，不带后缀。
            path = root + QLatin1Char('/') + name;
            const QFileInfo dataInfo(path);
            if (dataInfo.exists() && dataInfo.isDir())
                continue;
            if (!dataInfo.exists())
                reusable = true;
        }

        MediaCache::State state;
        const QFileInfo metaInfo(MediaCache::metaPathForFile(path));
        if (metaInfo.exists()) {
            if (MediaCache::readState(path, &state) && state.identity == identity)
                return path;
            continue;
        }

        // 没有旁车 JSON 的空目录可以复用；已有正文却缺 JSON 说明是异常残留，
        // 换下一个名字，避免把别的条目数据覆盖掉。
        if (item.type == MediaType::Video && !reusable
            && QDir(QFileInfo(path).absolutePath()).isEmpty()) {
            reusable = true;
        }
        if (reusable)
            return path;
    }
    // 极端同名情况下仍保证能落盘：短哈希是稳定且唯一的最后手段。
    return root + QLatin1Char('/') + base + QStringLiteral("__") + identityDigest(item, 8)
           + (item.type == MediaType::Video ? QStringLiteral("/media") : QString());
}

/// 删除时必须找到“当前实际占用”的条目路径，而不能复用 `resolvedCachePath`
/// 的“可写候选”逻辑。`removeForItem` 会先删正文再删 JSON；如果正文因文件
/// 句柄未释放而删除失败，再次进入时 JSON 仍在，身份还能定位到同一文件。
/// 反过来，一旦先删 JSON，正文就变成没有身份的孤儿，候选会跳到 `(2)`，
/// 重试永远删不到真正被占用的文件。这里把无 JSON 的残留正文也视为待删目标。
QString removalPathForItem(const MediaItem &item)
{
    const QString root = QDir::cleanPath(MediaCache::rootDir());
    const QString base = safeEntryName(item.displayName(), item.type);
    const QString identity = MediaCache::cacheIdentity(item);

    for (int index = 0; index < 1000; ++index) {
        const QString name = candidateName(base, index);
        QString path;
        if (item.type == MediaType::Video) {
            path = root + QLatin1Char('/') + name + QStringLiteral("/media");
        } else {
            path = root + QLatin1Char('/') + name;
        }
        if (!QFileInfo::exists(path))
            continue;

        const QFileInfo metaInfo(MediaCache::metaPathForFile(path));
        if (!metaInfo.exists())
            return path;

        MediaCache::State state;
        if (MediaCache::readState(path, &state) && state.identity == identity)
            return path;
    }

    // 没有任何残留可删时返回候选 0，让调用方走“无需删除”的成功路径。
    return item.type == MediaType::Video
               ? root + QLatin1Char('/') + base + QStringLiteral("/media")
               : root + QLatin1Char('/') + base;
}

} // namespace

namespace MediaCache {

QString rootDir()
{
    return StoragePaths::dataFile(QStringLiteral("media-cache"));
}

QString cacheIdentity(const MediaItem &item)
{
    if (!item.online)
        return mediaIdentity(item);
    if (!item.webUrl.isEmpty() && item.webUrl.isValid()) {
        return QStringLiteral("web:") + item.webUrl.toString() + QLatin1Char('#')
               + item.title;
    }

    QUrl bare = preferredPlaybackUrl(item);
    bare.setQuery(QString());
    bare.setFragment(QString());
    QString stableId;
    // 只捡名字里带 id 的参数（id/songId/videoId/aid…）；签名、过期时间这类
    // 会变的参数不能参与缓存归属，否则同一首歌每次重播都换一份缓存。
    const QUrlQuery query(item.url);
    for (const auto &pair : query.queryItems(QUrl::FullyDecoded)) {
        const QString key = pair.first.toLower();
        if (key == QStringLiteral("id") || key.endsWith(QStringLiteral("id"))) {
            stableId = pair.second;
            break;
        }
    }
    return QStringLiteral("url:") + bare.toString() + QLatin1Char('#')
           + QString::number(static_cast<int>(item.type)) + QLatin1Char('#') + item.title
           + QLatin1Char('#') + item.artist + QLatin1Char('#') + item.sourceId
           + QLatin1Char('#') + stableId;
}

QString entryBaseName(const MediaItem &item)
{
    return safeEntryName(item.displayName(), item.type);
}

QString directoryForItem(const MediaItem &item) {
    if (item.type == MediaType::Music)
        return rootDir();
    return QFileInfo(filePathForItem(item)).absolutePath();
}

QString filePathForItem(const MediaItem &item)
{
    return resolvedCachePath(item);
}

QString metaPathForFile(const QString &cachePath)
{
    // 正文故意不带后缀，真实格式写在紧邻的 `<正文>.json` 里。
    return cachePath + QStringLiteral(".json");
}

bool removeForItem(const MediaItem &item)
{
    const QString cacheRoot = QDir::cleanPath(rootDir());
    const QString path = QDir::cleanPath(removalPathForItem(item));
    const QString meta = QDir::cleanPath(metaPathForFile(path));
    bool ok = true;

    if (item.type == MediaType::Video) {
        const QString dir = QFileInfo(path).absolutePath();
        // 只允许删 media-cache 下这一层条目目录，防止路径计算异常时误删别处。
        if (dir.isEmpty() || dir == cacheRoot
            || !dir.startsWith(cacheRoot + QLatin1Char('/'))) {
            return false;
        }
        // 先删正文、再删 JSON、最后删空目录。正文被播放器短暂占用时保留
        // JSON，下一次重试还能靠身份找到同一个文件。
        if (QFileInfo::exists(path))
            ok = QFile::remove(path) && ok;
        if (QFileInfo::exists(path))
            return false;
        if (QFileInfo::exists(meta))
            ok = QFile::remove(meta) && ok;
        QDir().rmdir(dir);
    } else {
        if (path.isEmpty() || path == cacheRoot
            || !path.startsWith(cacheRoot + QLatin1Char('/'))) {
            return false;
        }
        if (QFileInfo::exists(path))
            ok = QFile::remove(path) && ok;
        // 正文没删掉就保留旁车 JSON；重试时才能继续定位这个正文。
        if (QFileInfo::exists(path))
            return false;
        if (QFileInfo::exists(meta))
            ok = QFile::remove(meta) && ok;
    }

    // 第一轮缓存是 `<32 位哈希>/media.<ext>`；删除条目时连旧布局一起清掉，
    // 避免用户升级后删了列表却留下看不见的孤儿目录。
    const QString legacy = cacheRoot + QLatin1Char('/') + identityDigest(item, 32);
    if (QDir(legacy).exists())
        ok = QDir(legacy).removeRecursively() && ok;
    return ok;
}

bool isCacheable(const MediaItem &item)
{
    if (!item.online)
        return false;
    const QUrl url = preferredPlaybackUrl(item);
    if (url.isEmpty() || !url.isValid())
        return false;
    // HLS 的主体是播放列表和分片，不存在单一可顺序读取的媒体文件；
    // 强行拼成一个文件会破坏分片索引与相对 URI，仍交给流代理按片缓存。
    return !isHlsUrl(url);
}

qint64 fileSize(const QString &cachePath)
{
    QFileInfo info(cachePath);
    return info.exists() && info.isFile() ? info.size() : 0;
}

bool readState(const QString &cachePath, State *state)
{
    if (state == nullptr || cachePath.isEmpty())
        return false;
    QFile file(metaPathForFile(cachePath));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != kMetaVersion)
        return false;

    State loaded;
    loaded.identity = object.value(QStringLiteral("identity")).toString();
    loaded.url = QUrl(object.value(QStringLiteral("url")).toString());
    loaded.contentType = object.value(QStringLiteral("contentType")).toString();
    loaded.extension = object.value(QStringLiteral("extension")).toString();
    loaded.totalBytes =
        static_cast<qint64>(object.value(QStringLiteral("total")).toDouble(-1));
    loaded.complete = object.value(QStringLiteral("complete")).toBool();
    loaded.updatedAtMs =
        static_cast<qint64>(object.value(QStringLiteral("updatedAt")).toDouble());
    *state = loaded;
    return true;
}

bool writeState(const QString &cachePath, const QString &identity, const QUrl &url,
                const QByteArray &contentType, qint64 totalBytes, bool complete,
                const QString &extension)
{
    if (cachePath.isEmpty() || identity.isEmpty())
        return false;
    if (!QDir().mkpath(QFileInfo(cachePath).absolutePath()))
        return false;

    QJsonObject object;
    object.insert(QStringLiteral("version"), kMetaVersion);
    object.insert(QStringLiteral("identity"), identity);
    // 地址只留 scheme/host/path：CDN 直链的查询串常带临时签名，
    // 缓存旁车没有理由把另一份短期凭据写到磁盘。
    QUrl safeUrl = url;
    safeUrl.setQuery(QString());
    safeUrl.setFragment(QString());
    object.insert(QStringLiteral("url"), safeUrl.toString());
    object.insert(QStringLiteral("contentType"), QString::fromLatin1(contentType));
    QString normalizedExtension = extension.trimmed().toLower();
    if (!normalizedExtension.isEmpty() && !normalizedExtension.startsWith(QLatin1Char('.')))
        normalizedExtension.prepend(QLatin1Char('.'));
    object.insert(QStringLiteral("extension"),
                  normalizedExtension.isEmpty()
                      ? safeExtensionFromUrl(url, contentType)
                      : normalizedExtension);
    object.insert(QStringLiteral("total"), static_cast<double>(totalBytes));
    object.insert(QStringLiteral("complete"), complete);
    object.insert(QStringLiteral("updatedAt"),
                  static_cast<double>(QDateTime::currentMSecsSinceEpoch()));

    QSaveFile file(metaPathForFile(cachePath));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}

QString completeFileForItem(const MediaItem &item)
{
    const bool hls = item.online && isHlsUrl(preferredPlaybackUrl(item));
    // HLS 播放列表本身不是文件，但 HlsCacheBuilder 会把它合并转封装成
    // 无后缀的 MP4 正文，所以“完整缓存”查询必须覆盖 HLS。
    if (!isCacheable(item) && !hls)
        return QString();
    if (!hls)
        adoptLegacyCache(item);
    const QString path = filePathForItem(item);
    State state;
    if (!readState(path, &state) || state.totalBytes <= 0
        || state.identity != cacheIdentity(item)) {
        return QString();
    }
    const qint64 size = fileSize(path);
    if (size <= 0 || size != state.totalBytes)
        return QString();
    return path;
}

QString partialFileForItem(const MediaItem &item)
{
    if (!isCacheable(item))
        return QString();
    adoptLegacyCache(item);
    const QString path = filePathForItem(item);
    const qint64 size = fileSize(path);
    if (size <= 0)
        return QString();

    State state;
    if (readState(path, &state)) {
        if (state.identity != cacheIdentity(item))
            return QString();
        if (state.totalBytes > 0 && size >= state.totalBytes)
            return QString();
    }
    return path;
}

} // namespace MediaCache
