#include "core/PlaylistStore.h"
#include "storage/StoragePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <limits>
#include <utility>

Q_LOGGING_CATEGORY(lcStore, "mvp.store")

namespace {

const char kMusicKey[] = "music";
const char kVideoKey[] = "video";

QString keyFor(MediaType type)
{
    return QString::fromLatin1(type == MediaType::Video ? kVideoKey : kMusicKey);
}

/// 程序目录下的 Data/，和 log/ 同级；安装目录不可写时退回用户数据目录
QString primaryPath()
{
    return StoragePaths::dataFile(QStringLiteral("playlists.json"));
}

QString fallbackPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/playlists.json");
}

bool writeFile(const QString &path, const QByteArray &payload)
{
    if (path.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const qint64 written = file.write(payload);
    if (written != payload.size()) {
        qCWarning(lcStore) << "播放列表写入不完整：" << path
                           << "，已写入" << written << "字节，应为" << payload.size();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        qCWarning(lcStore) << "播放列表写入失败：" << path << "：" << file.errorString();
        return false;
    }
    return true;
}

/// 损坏文件先备份；时间戳带毫秒并检查重名，避免连续两次 load 生成同名备份。
QString backupCorrupt(const QString &path)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss-zzz"));
    QString backup = path + QStringLiteral(".corrupt-") + stamp;
    int suffix = 1;
    while (QFileInfo::exists(backup)) {
        backup = path + QStringLiteral(".corrupt-") + stamp
            + QStringLiteral("-") + QString::number(suffix++);
    }
    return QFile::copy(path, backup) ? backup : QString();
}

/// 读取一个播放列表文件；文件不存在、打不开或损坏都返回 false。
/// 解析成功时允许空列表（有效状态），以便主备两份按 mtime 比较。
bool readPlaylistFile(const QString &path, MediaType type, QVector<MediaItem> *items)
{
    if (items == nullptr)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const QByteArray payload = file.readAll();
    file.close();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString backup = backupCorrupt(path);
        if (!backup.isEmpty())
            qCWarning(lcStore) << "playlists.json 已损坏，原文件备份为" << backup;
        else
            qCWarning(lcStore) << "playlists.json 已损坏，且无法备份原文件：" << path;
        return false;
    }

    const QJsonArray array = doc.object().value(keyFor(type)).toArray();
    items->clear();
    items->reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        MediaItem item = mediaItemFromJson(value.toObject());
        if (!item.isValid())
            continue;
        // 类型以所在分区为准，避免手工改文件后串区
        item.type = type;
        items->append(item);
    }
    return true;
}

} // namespace

namespace PlaylistStore {

QString filePath()
{
    return primaryPath();
}

QVector<MediaItem> load(MediaType type)
{
    const QString primary = primaryPath();
    const QString fallback = fallbackPath();

    // 主路径存在但不可写时，新数据会落到 fallback，而主路径还留着旧文件。
    // 只“主路径不存在才读 fallback”会把旧列表读回来，所以两份都读，
    // 在都有效的情况下选 mtime 较新的那一份。
    QVector<MediaItem> bestItems;
    qint64 bestMtime = std::numeric_limits<qint64>::min();
    QString bestPath;
    bool foundValid = false;

    QStringList candidates;
    candidates.append(primary);
    if (!fallback.isEmpty() && fallback != primary)
        candidates.append(fallback);

    for (const QString &path : candidates) {
        if (path.isEmpty() || !QFile::exists(path))
            continue;

        QVector<MediaItem> items;
        if (!readPlaylistFile(path, type, &items))
            continue;

        const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        if (!foundValid || mtime > bestMtime) {
            bestItems = items;
            bestMtime = mtime;
            bestPath = path;
            foundValid = true;
        }
    }

    if (!foundValid)
        return {};

    if (bestPath != primary) {
        qCInfo(lcStore) << (QFile::exists(primary)
                                ? "主播放列表不可用或较旧，改读备用路径："
                                : "从旧位置读取播放列表：")
                        << bestPath;
    }
    return bestItems;
}

bool saveAll(const QVector<MediaItem> &music, const QVector<MediaItem> &video)
{
    QJsonObject root;
    for (const auto &entry : {std::make_pair(MediaType::Music, &music),
                              std::make_pair(MediaType::Video, &video)}) {
        QJsonArray array;
        for (const MediaItem &item : *entry.second)
            array.append(mediaItemToJson(item));
        root.insert(keyFor(entry.first), array);
    }

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);

    const QString primary = primaryPath();
    const QString fallback = fallbackPath();
    // 先写备用、最后写主路径：load 按 mtime 选最新有效文件，主路径最后
    // 落盘才能在正常保存时稳定胜出。旧顺序会让备用副本几乎总是更新，
    // 启动时读到备用路径，并误报“主播放列表不可用或较旧”。
    const bool fallbackOk = (fallback.isEmpty() || fallback == primary)
        ? true : writeFile(fallback, payload);
    const bool primaryOk = writeFile(primary, payload);

    if (primaryOk) {
        qCInfo(lcStore) << "播放列表已保存（音乐" << music.size() << "条、视频" << video.size()
                        << "条）到" << primary;
        if (!fallbackOk)
            qCWarning(lcStore) << "主播放列表已保存，但备用路径写入失败：" << fallback;
        return true;
    }

    // 程序目录只读（例如装进 Program Files）时退回用户数据目录，不让保存静默失败
    if (fallbackOk && !fallback.isEmpty() && fallback != primary) {
        qCWarning(lcStore) << "无法写入" << primary << "，改用备用路径" << fallback;
        return true;
    }

    qCWarning(lcStore) << "播放列表保存失败，主路径和备用路径均不可写";
    return false;
}

} // namespace PlaylistStore
