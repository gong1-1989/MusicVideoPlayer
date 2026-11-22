#include "cache/MediaCache.h"

#include "storage/StoragePaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr int kMetaVersion = 1;

QString safeExtension(const MediaItem &item)
{
    const QUrl url = preferredPlaybackUrl(item);
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
        // `.../outer/url?id=123.mp3`。只按 path 取后缀会得到 "url"，
        // 最终落盘成 media.bin，既难辨认，也不利于播放器按扩展名建解码器。
        const QUrlQuery query(url);
        for (const auto &pair : query.queryItems(QUrl::FullyDecoded)) {
            const QString candidate = QFileInfo(pair.second).suffix().toLower();
            if (known.contains(candidate)) {
                suffix = candidate;
                break;
            }
        }
    }
    if (!known.contains(suffix))
        suffix = QStringLiteral("bin");
    return QLatin1Char('.') + suffix;
}

} // namespace

namespace MediaCache {

QString rootDir()
{
    return StoragePaths::dataFile(QStringLiteral("media-cache"));
}

QString directoryForItem(const MediaItem &item)
{
    const QByteArray digest =
        QCryptographicHash::hash(mediaIdentity(item).toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32);
    return rootDir() + QLatin1Char('/') + QString::fromLatin1(digest);
}

QString filePathForItem(const MediaItem &item)
{
    return directoryForItem(item) + QStringLiteral("/media") + safeExtension(item);
}

QString metaPathForFile(const QString &cachePath)
{
    return QFileInfo(cachePath).absolutePath() + QStringLiteral("/meta.json");
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
    loaded.url = QUrl(object.value(QStringLiteral("url")).toString());
    loaded.contentType = object.value(QStringLiteral("contentType")).toString();
    loaded.totalBytes =
        static_cast<qint64>(object.value(QStringLiteral("total")).toDouble(-1));
    loaded.complete = object.value(QStringLiteral("complete")).toBool();
    loaded.updatedAtMs =
        static_cast<qint64>(object.value(QStringLiteral("updatedAt")).toDouble());
    *state = loaded;
    return true;
}

bool writeState(const QString &cachePath, const QUrl &url, const QByteArray &contentType,
                qint64 totalBytes, bool complete)
{
    if (cachePath.isEmpty())
        return false;
    if (!QDir().mkpath(QFileInfo(cachePath).absolutePath()))
        return false;

    QJsonObject object;
    object.insert(QStringLiteral("version"), kMetaVersion);
    // 地址只留 scheme/host/path：CDN 直链的查询串常带临时签名，
    // 缓存旁车没有理由把另一份短期凭据写到磁盘。
    QUrl safeUrl = url;
    safeUrl.setQuery(QString());
    safeUrl.setFragment(QString());
    object.insert(QStringLiteral("url"), safeUrl.toString());
    object.insert(QStringLiteral("contentType"), QString::fromLatin1(contentType));
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
    if (!isCacheable(item))
        return QString();
    const QString path = filePathForItem(item);
    State state;
    if (!readState(path, &state) || state.totalBytes <= 0)
        return QString();
    const qint64 size = fileSize(path);
    if (size <= 0 || size != state.totalBytes)
        return QString();
    return path;
}

QString partialFileForItem(const MediaItem &item)
{
    if (!isCacheable(item))
        return QString();
    const QString path = filePathForItem(item);
    const qint64 size = fileSize(path);
    if (size <= 0)
        return QString();

    State state;
    if (readState(path, &state) && state.totalBytes > 0 && size >= state.totalBytes)
        return QString();
    return path;
}

void prune(qint64 maxBytes)
{
    QDir root(rootDir());
    if (!root.exists() || maxBytes <= 0)
        return;

    struct Entry
    {
        QString path;
        qint64 bytes = 0;
        qint64 lastUsedMs = 0;
    };
    QVector<Entry> entries;
    qint64 total = 0;
    const QFileInfoList dirs =
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &dirInfo : dirs) {
        Entry entry;
        entry.path = dirInfo.absoluteFilePath();
        entry.lastUsedMs = dirInfo.lastModified().toMSecsSinceEpoch();
        QDirIterator it(entry.path, QDir::Files | QDir::Hidden,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            entry.bytes += info.size();
            entry.lastUsedMs = qMax(entry.lastUsedMs, info.lastModified().toMSecsSinceEpoch());
        }
        total += entry.bytes;
        entries.append(entry);
    }
    if (total <= maxBytes)
        return;

    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.lastUsedMs < b.lastUsedMs;
    });
    for (const Entry &entry : std::as_const(entries)) {
        if (total <= maxBytes)
            break;
        if (QDir(entry.path).removeRecursively())
            total -= entry.bytes;
    }
}

} // namespace MediaCache
