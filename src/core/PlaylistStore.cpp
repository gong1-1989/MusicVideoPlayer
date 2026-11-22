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
#include <utility>

Q_LOGGING_CATEGORY(lcStore, "mvp.store")

namespace {

const char kMusicKey[] = "music";
const char kVideoKey[] = "video";
constexpr qint64 kMaxPlaylistFileBytes = 32 * 1024 * 1024;
constexpr int kMaxCorruptBackups = 5;

QString keyFor(MediaType type)
{
    return QString::fromLatin1(type == MediaType::Video ? kVideoKey : kMusicKey);
}

/// 播放列表只存程序目录下的 Data/，不再写用户 AppData
QString primaryPath()
{
    return StoragePaths::dataFile(QStringLiteral("playlists.json"));
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
/// 用 rename 而不是 copy：第二次 load 就看不到损坏文件，不会重复备份。
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
    if (!QFile::rename(path, backup))
        return QString();

    // 只保留最近几次损坏备份，避免反复启动把备份目录撑大。
    const QFileInfo info(path);
    const QString pattern = info.fileName() + QStringLiteral(".corrupt-*");
    const QFileInfoList backups =
        QDir(info.absolutePath()).entryInfoList({pattern}, QDir::Files, QDir::Name);
    for (int i = 0; i < backups.size() - kMaxCorruptBackups; ++i)
        QFile::remove(backups.at(i).absoluteFilePath());
    return backup;
}

/// 读取播放列表文件；文件不存在、打不开或损坏都返回 false。
/// 解析成功时允许空列表（有效状态）。
bool readPlaylistFile(const QString &path, MediaType type, QVector<MediaItem> *items)
{
    if (items == nullptr)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    if (file.size() > kMaxPlaylistFileBytes) {
        qCWarning(lcStore) << "播放列表文件超过大小上限，按损坏处理：" << path;
        file.close();
        const QString backup = backupCorrupt(path);
        if (!backup.isEmpty())
            qCWarning(lcStore) << "playlists.json 已超过上限，原文件备份为" << backup;
        return false;
    }

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
    QVector<MediaItem> items;
    if (readPlaylistFile(primaryPath(), type, &items))
        return items;
    return {};
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
    if (writeFile(primary, payload)) {
        qCInfo(lcStore) << "播放列表已保存（音乐" << music.size() << "条、视频" << video.size()
                        << "条）到" << primary;
        return true;
    }

    qCWarning(lcStore) << "播放列表保存失败：" << primary;
    return false;
}

} // namespace PlaylistStore
