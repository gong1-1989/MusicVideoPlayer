/// @file MediaCache.cpp
/// @brief 播放缓存的条目布局、身份计算、旁车元数据与删除/迁移逻辑。
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

/// @brief 从 URL 路径、查询串文件名或 Content-Type 推断安全扩展名。
/// @param url 媒体地址。
/// @param contentType 响应 Content-Type，可为空。
/// @return 带点的扩展名；无法识别时返回 ".bin"。
/// @note 只接受已知媒体后缀，避免把任意路径后缀直接拼到磁盘文件名上。
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

/// @brief 生成可安全用作单级文件/目录名的条目名。
QString safeEntryName(const QString &raw, MediaType type)
{
    return FileNameUtils::sanitizeComponent(
        raw, type == MediaType::Video ? QStringLiteral("影视") : QStringLiteral("音乐"), 120);
}

/// @brief 对 mediaIdentity() 做 SHA-256 并截取前 length 位十六进制。
QString identityDigest(const MediaItem &item, int length)
{
    return QCryptographicHash::hash(mediaIdentity(item).toUtf8(), QCryptographicHash::Sha256)
        .toHex()
        .left(length);
}

/// 第一轮缓存按“去掉整个查询串”的 mediaIdentity 算哈希。mediaIdentity 后来
/// 改为保留 `id` 等稳定参数后，老版本留下的 `<hash>/media.*` 目录会换个哈希，
/// 升级用户就会看不到已经下好的缓存。这里保留旧口径用于兼容查找与清理。
/// @brief 计算旧版缓存目录口径的身份摘要（去掉整个查询串与片段）。
QString legacyIdentityDigest(const MediaItem &item, int length)
{
    MediaItem legacy = item;
    legacy.url.setQuery(QString());
    legacy.url.setFragment(QString());
    return QCryptographicHash::hash(mediaIdentity(legacy).toUtf8(),
                                    QCryptographicHash::Sha256)
        .toHex()
        .left(length);
}

/// 旧目录按“去掉整个查询串”的身份做哈希，`outer/url?id=1` 和
/// `outer/url?id=2` 会共用同一个旧目录。旧 `meta.json` 可能只存了不带
/// 查询串的地址，无法区分曲目；此时按稳定查询参数比较并拒绝收养，
/// 宁可少迁移一首，也不能把 A 歌的缓存搬给 B 歌。
/// @brief 尝试把旧版 `<hash>/media.*` 缓存迁移到当前条目布局。
/// @param item 目标媒体条目。
/// @return 迁移成功（或无需迁移）返回 true；身份无法确认、搬迁或写旁车失败返回 false。
/// @note 旧版 meta 可能只存去掉查询串的地址，无法区分同名曲目时拒绝收养，
///       宁可少迁移一首，也不能把 A 歌的缓存搬给 B 歌。
bool adoptLegacyCache(const MediaItem &item)
{
    if (!MediaCache::isCacheable(item))
        return false;
    const QString newPath = MediaCache::filePathForItem(item);
    if (QFileInfo::exists(newPath) || QFileInfo::exists(MediaCache::metaPathForFile(newPath)))
        return false;

    QString legacyDir;
    for (const QString &digest : {identityDigest(item, 32), legacyIdentityDigest(item, 32)}) {
        const QString candidate = MediaCache::rootDir() + QLatin1Char('/') + digest;
        if (QDir(candidate).exists()) {
            legacyDir = candidate;
            break;
        }
    }
    if (legacyDir.isEmpty())
        return false;
    QDir dir(legacyDir);

    QFile meta(dir.filePath(QStringLiteral("meta.json")));
    if (!meta.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(meta.readAll()).object();
    meta.close();
    if (object.value(QStringLiteral("version")).toInt() != 1)
        return false;

    const QUrl oldUrl(QUrl(object.value(QStringLiteral("url")).toString()));
    const QUrl preferred = preferredPlaybackUrl(item);
    if (!oldUrl.isValid()
        || identityUrlWithoutVolatileQuery(oldUrl) != identityUrlWithoutVolatileQuery(preferred))
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

/// @brief 生成同名冲突时的候选名：第二个起追加 " (2)"、" (3)"…
QString candidateName(const QString &base, int index)
{
    return index <= 0 ? base : base + QStringLiteral(" (%1)").arg(index + 1);
}

/// @brief 为条目解析一个可写的缓存正文路径。
/// @param item 媒体条目。
/// @return 影视条目为 `<root>/<名称>/media`，音乐条目为 `<root>/<名称>`。
/// @note 同名冲突时依次尝试 " (2)"…；只有无旁车且无正文的空目录才复用；
///       1000 个候选仍冲突时退回“名称__8位身份摘要”保证可落盘。
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
/// @brief 为删除操作定位“当前实际占用”的缓存正文路径。
/// @param item 媒体条目。
/// @return 命中的正文路径；没有残留时返回候选 0（调用方按“无需删除”处理）。
/// @note 与 resolvedCachePath() 的“可写候选”语义不同：这里把无 JSON 的残留正文
///       也视为待删目标，并且先删正文后删 JSON，保证删除失败重试时仍能定位。
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

/// @brief 缓存根目录（Data/media-cache）。
QString rootDir()
{
    return StoragePaths::dataFile(QStringLiteral("media-cache"));
}

/// @brief 计算缓存归属身份。
/// @param item 媒体条目。
/// @return 本地条目直接用 mediaIdentity；网页条目用 webUrl+标题；在线条目用
///         去查询串的播放地址、类型、标题、作者、源 id 与稳定 id 参数拼成。
/// @note 签名、过期时间等易变参数不参与身份，否则同一首歌每次重播都会新建缓存。
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

/// @brief 条目的缓存文件名基名（已清洗成单级安全名）。
QString entryBaseName(const MediaItem &item)
{
    return safeEntryName(item.displayName(), item.type);
}

/// @brief 条目缓存所在目录：音乐为缓存根目录，影视为条目目录。
QString directoryForItem(const MediaItem &item) {
    if (item.type == MediaType::Music)
        return rootDir();
    return QFileInfo(filePathForItem(item)).absolutePath();
}

/// @brief 条目缓存正文的目标路径。
QString filePathForItem(const MediaItem &item)
{
    return resolvedCachePath(item);
}

/// @brief 正文对应的旁车元数据路径（正文路径加 ".json"）。
QString metaPathForFile(const QString &cachePath)
{
    // 正文故意不带后缀，真实格式写在紧邻的 `<正文>.json` 里。
    return cachePath + QStringLiteral(".json");
}

/// @brief 删除单个条目的缓存，并清理旧版 `<hash>/` 布局。
/// @param item 媒体条目。
/// @return 全部删除成功返回 true；正文被占用或路径越界返回 false。
/// @note 只允许操作 media-cache 下的路径；先删正文、后删 JSON，正文没删掉时保留 JSON
///       以便下次重试仍能定位同一文件。
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
    // 避免用户升级后删了列表却留下看不见的孤儿目录。查询串稳定参数改造
    // 前后哈希口径不同，两种都要清。
    for (const QString &digest : {identityDigest(item, 32), legacyIdentityDigest(item, 32)}) {
        const QString legacy = cacheRoot + QLatin1Char('/') + digest;
        if (QDir(legacy).exists())
            ok = QDir(legacy).removeRecursively() && ok;
    }
    return ok;
}

/// @brief 按已知缓存路径删除正文、旁车 JSON 和空条目目录。
/// @param cachePath 正文路径。
/// @return 删除成功返回 true；路径在缓存根之外或正文仍被占用返回 false。
bool removeCachePath(const QString &cachePath)
{
    if (cachePath.isEmpty())
        return false;

    const QString cacheRoot = QDir::cleanPath(rootDir());
    const QString path = QDir::cleanPath(cachePath);
    if (path == cacheRoot || !path.startsWith(cacheRoot + QLatin1Char('/')))
        return false;

    const QString meta = QDir::cleanPath(metaPathForFile(path));
    bool ok = true;

    if (QFileInfo::exists(path))
        ok = QFile::remove(path) && ok;
    // 正文仍被播放器占用时不能先删元数据；否则下一次重试只剩一个
    // 找不到归属的正文，用户也无法从界面判断它属于哪一条。
    if (QFileInfo::exists(path))
        return false;
    if (QFileInfo::exists(meta))
        ok = QFile::remove(meta) && ok;

    // 影视正文在 media-cache/<条目名>/media，正文和 JSON 都删完后
    // 顺手回收空目录；音乐正文直接在根目录，父目录等于 root，不处理。
    const QString parent = QFileInfo(path).absolutePath();
    if (parent != cacheRoot && parent.startsWith(cacheRoot + QLatin1Char('/')))
        QDir().rmdir(parent);

    return ok;
}

/// @brief 判断条目是否可做单文件播放缓存。
/// @return 只有在线且非 HLS 地址返回 true；HLS 交给流代理按分片缓存。
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

/// @brief 返回缓存正文大小；文件不存在或不是普通文件时返回 0。
qint64 fileSize(const QString &cachePath)
{
    QFileInfo info(cachePath);
    return info.exists() && info.isFile() ? info.size() : 0;
}

/// @brief 读取正文旁车元数据。
/// @param cachePath 正文路径。
/// @param state 输出状态，必须非空。
/// @return 文件可读、JSON 合法且版本匹配时返回 true。
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

/// @brief 原子写入旁车元数据。
/// @param cachePath 正文路径。
/// @param identity 缓存归属身份。
/// @param url 播放地址；写入时去掉查询串与片段。
/// @param contentType 响应类型。
/// @param totalBytes 完整大小，未知为 -1。
/// @param complete 是否已完整下载。
/// @param extension 正文扩展名；为空时按 URL/Content-Type 推断。
/// @return 提交成功返回 true；短写会 cancelWriting，避免提交截断 JSON。
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
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const qint64 written = file.write(payload);
    if (written != payload.size()) {
        // 磁盘满/配额不足时 QSaveFile 允许短写；不 cancel 就直接 commit
        // 会把截断的 JSON 当成新旁车提交，下一次 readState 失败后会丢掉
        // 已下好的缓存归属（正文还在，但程序再也找不到它）。
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

/// @brief 返回可优先复用的完整缓存正文路径。
/// @param item 媒体条目。
/// @return 身份匹配、大小等于记录总字节且文件存在时返回路径，否则返回空。
/// @note 覆盖 HLS：HlsCacheBuilder 会把分片合并成无后缀 MP4 正文。
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

/// @brief 返回可继续续写的部分缓存正文路径。
/// @param item 媒体条目。
/// @return 文件存在、身份匹配且尚未达到记录总字节时返回路径，否则返回空。
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
