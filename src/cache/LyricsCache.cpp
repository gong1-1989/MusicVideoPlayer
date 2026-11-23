/// @file LyricsCache.cpp
/// @brief 在线歌词正文与旁车元数据的磁盘缓存实现。
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

/// @brief 歌词缓存的唯一身份：媒体缓存身份 + 歌词地址。
QString lyricIdentity(const MediaItem &item)
{
    const QUrl lyricUrl = item.lyricUrl;
    return MediaCache::cacheIdentity(item) + QStringLiteral("#lyric:")
           + lyricUrl.toString();
}

/// @brief 生成第 index 个候选正文路径；index 0 用基础名，之后追加 “ (n)”。
QString candidatePath(const MediaItem &item, int index)
{
    const QString base = MediaCache::entryBaseName(item);
    const QString name = index <= 0
                             ? base
                             : base + QStringLiteral(" (%1)").arg(index + 1);
    return LyricsCache::rootDir() + QLatin1Char('/') + name;
}

/// @brief 读取旁车 JSON 的版本与 identity。
/// @param path 正文路径（函数自动加 .json）。
/// @param identity 输出 identity，可为空。
/// @return 文件可读、版本匹配且 identity 解析成功返回 true。
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

/// @brief 为当前条目挑选可写候选路径。
/// @return 空槽位或同身份已有槽位；全部候选被占时用短哈希兜底。
/// @note 有正文但缺 JSON 的残片不复用、不覆盖，换下一个名字。
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

/// @brief 写入旁车 JSON，记录版本、identity、来源、格式与大小。
/// @return 写入并 commit 成功返回 true；短写时 cancel，避免截断 JSON。
bool writeMeta(const QString &path, const MediaItem &item, const QByteArray &payload)
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), kLyricsMetaVersion);
    object.insert(QStringLiteral("identity"), lyricIdentity(item));
    object.insert(QStringLiteral("url"), item.lyricUrl.toString());
    object.insert(QStringLiteral("extension"),
                  payload.trimmed().startsWith('{') ? QStringLiteral(".json")
                                                    : QStringLiteral(".lrc"));
    object.insert(QStringLiteral("total"), payload.size());
    object.insert(QStringLiteral("updatedAt"),
                  static_cast<double>(QDateTime::currentMSecsSinceEpoch()));

    QSaveFile file(path + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray payloadBytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(payloadBytes) != payloadBytes.size()) {
        // 和播放缓存旁车一样：短写时不能 commit 截断的 JSON，否则下次
        // 读不出 identity，已写入的歌词正文会变成无法回收的孤儿。
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace

namespace LyricsCache {

/// @brief 返回歌词缓存根目录（与播放缓存同级）。
QString rootDir()
{
    const QString dataDir = QFileInfo(MediaCache::rootDir()).absolutePath();
    return QDir::cleanPath(dataDir + QStringLiteral("/lyrics-cache"));
}

/// @brief 判断条目是否适合写歌词缓存。
/// @return 仅在线音乐且歌词地址为绝对 http(s) 时返回 true。
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

/// @brief 读取当前条目的歌词正文。
/// @param payload 输出正文；可为空。
/// @return 命中且正文非空返回 true。
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

/// @brief 原子写入歌词正文及旁车元数据。
/// @return 正文和旁车都成功返回 true；旁车失败时回收正文与旧 JSON。
/// @note 正文已落盘但旁车失败会留下孤儿，必须一并删除，避免后续候选位被
///       误判为可用而 load() 打不开正文。
bool store(const MediaItem &item, const QByteArray &payload)
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
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
        return false;

    if (!writeMeta(path, item, payload)) {
        // 正文已写、旁车失败会留下“有正文无 JSON”的孤儿；直接回收，下次重试。
        // 该路径原来的正文已被 QSaveFile 原子替换，旧 JSON 无论属于谁都已经
        // 失效，必须一并删掉；否则下一次 writablePath() 会把它当可用槽位，
        // load() 却打不开正文。
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".json"));
        return false;
    }
    return true;
}

/// @brief 删除条目在全部候选位上的正文与旁车。
/// @return 全部删除成功返回 true；正文删不掉时保留 JSON 供重试。
/// @note 同一条目可能散落在基础名、“(2)”等多个候选位；缺 JSON 的孤儿
///       也要清理，否则会永久占位并把后续写入推到更后面的序号。
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
