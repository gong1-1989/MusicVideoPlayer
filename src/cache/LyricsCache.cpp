#include "cache/LyricsCache.h"

#include "cache/MediaCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

namespace {

constexpr int kLyricsMetaVersion = 1;

QString lyricIdentity(const MediaItem &item)
{
    const QUrl lyricUrl = item.lyricUrl;
    return MediaCache::cacheIdentity(item) + QStringLiteral("#lyric:")
           + lyricUrl.toString();
}

QString candidatePath(const MediaItem &item, int index)
{
    const QString base = MediaCache::entryBaseName(item);
    const QString name = index <= 0
                             ? base
                             : base + QStringLiteral(" (%1)").arg(index + 1);
    return LyricsCache::rootDir() + QLatin1Char('/') + name;
}

bool readMetaIdentity(const QString &path, QString *identity)
{
    QFile file(path + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != kLyricsMetaVersion)
        return false;
    if (identity != nullptr)
        *identity = object.value(QStringLiteral("identity")).toString();
    return true;
}

/// 找当前条目实际占用的歌词正文。创建候选和读取现有必须分开：
/// 创建时遇到“无正文无 JSON”的空位就停，读取时只有身份匹配才算命中。
QString existingPath(const MediaItem &item)
{
    if (!LyricsCache::isEligible(item))
        return QString();
    const QString identity = lyricIdentity(item);
    for (int index = 0; index < 1000; ++index) {
        const QString path = candidatePath(item, index);
        const QFileInfo dataInfo(path);
        if (!dataInfo.exists() || !dataInfo.isFile())
            continue;
        QString storedIdentity;
        if (readMetaIdentity(path, &storedIdentity) && storedIdentity == identity)
            return path;
    }
    return QString();
}

QString writablePath(const MediaItem &item)
{
    const QString root = LyricsCache::rootDir();
    const QString identity = lyricIdentity(item);
    for (int index = 0; index < 1000; ++index) {
        const QString path = candidatePath(item, index);
        const QFileInfo dataInfo(path);
        const QFileInfo metaInfo(path + QStringLiteral(".json"));
        if (dataInfo.exists() && !dataInfo.isFile())
            continue;
        if (metaInfo.exists()) {
            QString storedIdentity;
            if (readMetaIdentity(path, &storedIdentity) && storedIdentity == identity)
                return path;
            continue;
        }
        if (dataInfo.exists()) {
            // 有正文但缺 JSON：可能是别的条目残留，换下一个名字，绝不覆盖。
            continue;
        }
        return path;
    }
    const QString digest =
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(8);
    return root + QLatin1Char('/') + MediaCache::entryBaseName(item)
           + QStringLiteral("__") + digest;
}

bool writeMeta(const QString &path, const MediaItem &item, const QUrl &lyricUrl,
               const QByteArray &payload)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), kLyricsMetaVersion);
    object.insert(QStringLiteral("identity"), lyricIdentity(item));
    object.insert(QStringLiteral("url"), lyricUrl.toString());
    object.insert(QStringLiteral("extension"),
                  payload.trimmed().startsWith('{') ? QStringLiteral(".json")
                                                    : QStringLiteral(".lrc"));
    object.insert(QStringLiteral("total"), payload.size());
    object.insert(QStringLiteral("updatedAt"),
                  static_cast<double>(QDateTime::currentMSecsSinceEpoch()));

    QSaveFile file(path + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

} // namespace

namespace LyricsCache {

QString rootDir()
{
    const QString dataDir = QFileInfo(MediaCache::rootDir()).absolutePath();
    return QDir::cleanPath(dataDir + QStringLiteral("/lyrics-cache"));
}

bool isEligible(const MediaItem &item)
{
    if (item.type != MediaType::Music || !item.online)
        return false;
    const QUrl url = item.lyricUrl;
    if (url.isEmpty() || !url.isValid() || url.isRelative())
        return false;
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

bool load(const MediaItem &item, QByteArray *payload)
{
    if (payload == nullptr)
        return false;
    const QString path = existingPath(item);
    if (path.isEmpty())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *payload = file.readAll();
    return !payload->isEmpty();
}

bool store(const MediaItem &item, const QUrl &lyricUrl, const QByteArray &payload)
{
    if (!isEligible(item) || payload.isEmpty())
        return false;
    if (!QDir().mkpath(rootDir()))
        return false;

    const QString path = writablePath(item);
    if (path.isEmpty())
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(payload);
    if (!file.commit())
        return false;

    if (!writeMeta(path, item, lyricUrl, payload)) {
        // 正文已写、旁车失败会留下无法归属的孤儿文件；直接回收，下次重试。
        QFile::remove(path);
        return false;
    }
    return true;
}

bool removeForItem(const MediaItem &item)
{
    if (!LyricsCache::isEligible(item))
        return true;

    // 同一条目可能散落在基础名、"(2)"、"(3)" 等多个候选位；
    // 其中“有正文但没有 JSON”的孤儿文件无法通过 existingPath() 定位，
    // 删除条目时必须一并清理，否则会永久占位并把后续写入推到更后面的序号。
    const QString identity = lyricIdentity(item);
    bool ok = true;
    for (int index = 0; index < 1000; ++index) {
        const QString path = candidatePath(item, index);
        const bool bodyExists = QFileInfo::exists(path);
        const QString meta = path + QStringLiteral(".json");
        const bool metaExists = QFileInfo::exists(meta);
        if (!bodyExists && !metaExists)
            continue;

        QString storedIdentity;
        const bool metaUsable = metaExists && readMetaIdentity(path, &storedIdentity);
        if (!metaUsable || storedIdentity != identity) {
            // 缺 JSON 或 JSON 已损坏的正文不可复用，属于孤儿，直接回收。
            // 身份不同且元数据可读的正文属于同名其它条目，保留。
            if (metaUsable)
                continue;
        }

        if (bodyExists)
            ok = QFile::remove(path) && ok;
        // 正文删除失败时保留 JSON，和播放缓存一样让调用方可以重试。
        if (QFileInfo::exists(path))
            return false;
        if (QFileInfo::exists(meta))
            ok = QFile::remove(meta) && ok;
    }
    return ok;
}

} // namespace LyricsCache
