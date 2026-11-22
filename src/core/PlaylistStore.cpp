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
#include <QThread>
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

/// 最近一次成功保存的完整快照。主文件损坏时用它恢复，而不是把损坏内容当成空列表。
QString snapshotPath()
{
    return primaryPath() + QStringLiteral(".bak");
}

bool writeFile(const QString &path, const QByteArray &payload)
{
    if (path.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(path).absolutePath());

    // Windows 上杀毒/同步软件可能短暂占用目标文件，QSaveFile 的替换会失败。
    // 连续重试几次通常能跨过这个窗口；永久无权限时仍会返回失败，不吞错误。
    QString lastError;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            lastError = file.errorString();
        } else {
            const qint64 written = file.write(payload);
            if (written != payload.size()) {
                lastError = QStringLiteral("写入不完整：%1/%2 字节")
                                .arg(written)
                                .arg(payload.size());
                file.cancelWriting();
            } else if (file.commit()) {
                return true;
            } else {
                lastError = file.errorString();
            }
        }
        if (attempt < 2)
            QThread::msleep(80);
    }

    qCWarning(lcStore) << "播放列表写入失败：" << path << "：" << lastError;
    return false;
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

/// 读取结果必须区分“正常读取”“文件不存在”“读不了”“格式损坏”。
/// 旧实现把后三者都当空列表，导致退出时可能用空模型覆盖原文件。
enum class ReadStatus
{
    Ok,
    Missing,
    ReadError,
    TooLarge,
    Corrupt
};

struct ReadFileResult
{
    ReadStatus status = ReadStatus::Missing;
    QVector<MediaItem> music;
    QVector<MediaItem> video;
    QString error;
};

QVector<MediaItem> parsePartition(const QJsonObject &root, MediaType type)
{
    const QJsonArray array = root.value(keyFor(type)).toArray();
    QVector<MediaItem> items;
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        MediaItem item = mediaItemFromJson(value.toObject());
        if (!item.isValid())
            continue;
        // 类型以所在分区为准，避免手工改文件后串区
        item.type = type;
        items.append(item);
    }
    return items;
}

ReadFileResult readPlaylistFile(const QString &path)
{
    ReadFileResult result;
    const QFileInfo info(path);
    if (!info.exists()) {
        result.status = ReadStatus::Missing;
        return result;
    }
    if (!info.isFile()) {
        result.status = ReadStatus::ReadError;
        result.error = QStringLiteral("路径不是普通文件");
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.status = ReadStatus::ReadError;
        result.error = file.errorString();
        return result;
    }

    if (file.size() > kMaxPlaylistFileBytes) {
        file.close();
        result.status = ReadStatus::TooLarge;
        result.error = QStringLiteral("文件超过 32MB 上限");
        return result;
    }

    const QByteArray payload = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.status = ReadStatus::ReadError;
        result.error = file.errorString();
        file.close();
        return result;
    }
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.status = ReadStatus::Corrupt;
        result.error = parseError.errorString();
        return result;
    }

    const QJsonObject root = doc.object();
    result.music = parsePartition(root, MediaType::Music);
    result.video = parsePartition(root, MediaType::Video);
    result.status = ReadStatus::Ok;
    return result;
}

} // namespace

namespace PlaylistStore {

QString filePath()
{
    return primaryPath();
}

QString backupFilePath()
{
    return snapshotPath();
}

LoadResult loadAll()
{
    LoadResult result;
    const QString primary = primaryPath();
    const QString snapshot = snapshotPath();
    const QFileInfo primaryInfo(primary);

    if (primaryInfo.exists()) {
        const ReadFileResult read = readPlaylistFile(primary);
        if (read.status == ReadStatus::Ok) {
            result.music = read.music;
            result.video = read.video;
            return result;
        }

        const QString corrupt = backupCorrupt(primary);
        if (corrupt.isEmpty()) {
            // 既读不了、又无法把原文件移走：绝不能把空模型写回去覆盖它。
            result.storageBlocked = true;
            result.message = QStringLiteral(
                "播放列表读取失败，且无法备份原文件；为避免覆盖，当前禁止保存。文件：%1")
                .arg(primary);
            qCCritical(lcStore) << "播放列表读取失败且无法备份，禁止保存：" << primary
                                << "：" << read.error;
            return result;
        }

        result.corruptBackupPath = corrupt;
        qCWarning(lcStore) << "播放列表无法读取，原文件已备份为" << corrupt
                           << "，原因：" << read.error;
    } else {
        result.fileMissing = true;
    }

    // 主文件缺失、损坏或读不了时，回退到最近一次成功保存的完整快照。
    const QFileInfo snapshotInfo(snapshot);
    if (snapshotInfo.exists()) {
        const ReadFileResult read = readPlaylistFile(snapshot);
        if (read.status == ReadStatus::Ok) {
            result.music = read.music;
            result.video = read.video;
            result.recoveredFromBackup = true;
            result.message = QStringLiteral(
                "播放列表主文件不可用，已从最近一次成功保存的快照恢复：音乐 %1 首，视频 %2 个")
                .arg(result.music.size())
                .arg(result.video.size());
            qCInfo(lcStore) << "播放列表已从快照恢复：" << snapshot;
            return result;
        }
        qCWarning(lcStore) << "播放列表快照也无法读取：" << snapshot
                           << "，原因：" << read.error;
    }

    if (!result.corruptBackupPath.isEmpty()) {
        result.message = QStringLiteral(
            "播放列表原文件已备份为 %1，但最近一次成功快照不可用；当前显示空列表，保存前请确认。")
            .arg(result.corruptBackupPath);
    }
    return result;
}

QVector<MediaItem> load(MediaType type)
{
    const LoadResult result = loadAll();
    return type == MediaType::Video ? result.video : result.music;
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
    if (!writeFile(primary, payload)) {
        qCWarning(lcStore) << "播放列表保存失败：" << primary;
        return false;
    }

    // 主文件写成功后再刷新快照；快照失败不影响本次主文件已经落盘。
    const QString snapshot = snapshotPath();
    if (!writeFile(snapshot, payload))
        qCWarning(lcStore) << "播放列表快照写入失败：" << snapshot;

    qCInfo(lcStore) << "播放列表已保存（音乐" << music.size() << "条、视频" << video.size()
                    << "条）到" << primary;
    return true;
}

} // namespace PlaylistStore
