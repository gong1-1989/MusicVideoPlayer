/// @file PlaylistStore.cpp
/// @brief 播放列表主文件/快照的读写、损坏恢复与保存阻断实现。
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

/// @brief 按媒体类型返回 JSON 分区键（music/video）。
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

/// @brief 写文件：QSaveFile 原子替换，Windows 占用时最多重试 3 次。
/// @return 写入并 commit 成功返回 true；失败返回 false 并记录日志。
/// @note 重试不 sleep：这个函数由 GUI 线程的自动保存/退出保存调用，
///       原先两次 80ms 的等待会把界面卡住最多 160ms；QSaveFile 失败通常
///       是瞬时占用，立即重试仍有意义，但不能把等待成本转嫁给 UI。
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
    }

    qCWarning(lcStore) << "播放列表写入失败：" << path << "：" << lastError;
    return false;
}

/// @brief 把损坏文件改名备份，并只保留最近 5 份备份。
/// @return 备份路径；rename 失败返回空。
/// @note 用 rename 而非 copy：第二次 load 就看不到损坏文件，不会重复备份；
///       时间戳带毫秒并查重名，避免连续两次生成同名备份。
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
/// 只有 Ok 可以正常加载；ReadError/TooLarge/Corrupt 都必须阻断保存，
/// 否则退出时可能用空模型覆盖仍然可恢复的原文件。
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

/// @brief 解析指定媒体分区的条目数组。
/// @note 类型以所在分区为准，避免手工改文件后音乐/视频串区。
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

/// @brief 读取并严格校验一个播放列表文件。
/// @return 包含状态、两个分区条目和错误原因；只有 Ok 才带有效数据。
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
    // 合法保存必定同时写音乐和视频数组。若两个键都不是数组，说明文件被
    // 手工改坏或只写了一半；不能再当成“空列表”正常加载，否则退出保存会
    // 用空列表覆盖仍然可恢复的备份。
    if (!root.value(keyFor(MediaType::Music)).isArray()
        || !root.value(keyFor(MediaType::Video)).isArray()) {
        result.status = ReadStatus::Corrupt;
        result.error = QStringLiteral("缺少音乐/视频数组");
        return result;
    }
    result.music = parsePartition(root, MediaType::Music);
    result.video = parsePartition(root, MediaType::Video);
    result.status = ReadStatus::Ok;
    return result;
}

} // namespace

namespace PlaylistStore {

/// @brief 返回主播放列表文件路径（Data/playlists.json）。
QString filePath()
{
    return primaryPath();
}

/// @brief 返回最近一次成功保存的完整快照路径（主文件 + .bak）。
QString backupFilePath()
{
    return snapshotPath();
}

/// @brief 加载音乐与视频两个分区，并在主文件异常时尝试快照恢复。
/// @return 汇总结果：成功数据、是否缺失、是否损坏、是否阻断保存及提示文本。
/// @note ReadError（占用/权限）与 TooLarge 直接阻断保存，不备份、不回退，
///       避免把仍可能有效的用户列表移走或用旧快照覆盖。
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

        // 只有“内容确实损坏”才应该把原文件改名备份。ReadError 常见于
        // 文件被占用/权限不足，文件内容可能仍然有效；此时改名会移走用户
        // 的最新列表，再回退旧快照并允许保存，等于用旧数据覆盖新数据。
        if (read.status == ReadStatus::ReadError || read.status == ReadStatus::TooLarge) {
            result.storageBlocked = true;
            result.message = read.status == ReadStatus::ReadError
                ? QStringLiteral(
                      "播放列表读取失败，原文件可能被占用或没有读取权限；为避免覆盖，"
                      "当前禁止保存。文件：%1（%2）")
                      .arg(primary, read.error)
                : QStringLiteral(
                      "播放列表文件超过 32MB 上限，未加载；为避免覆盖，当前禁止保存。文件：%1")
                      .arg(primary);
            qCCritical(lcStore) << "播放列表未能读取，禁止保存：" << primary << "：" << read.error;
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

    // 主文件缺失或确实损坏时，回退到最近一次成功保存的完整快照。
    // ReadError/TooLarge 已在上方直接 blocked 返回，不能拿快照覆盖可能仍有效的原文件。
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
        // 原文件损坏且没有可用快照时，列表只能为空；如果允许保存，
        // 退出时会把空列表写成新的主文件。备份仍在，但当前会话的修改
        // 会以“成功保存”的方式掩盖问题，所以同样禁止保存。
        result.storageBlocked = true;
        result.message = QStringLiteral(
            "播放列表原文件已备份为 %1，但最近一次成功快照不可用；"
            "当前显示空列表，为避免误写空文件，已禁止保存。")
            .arg(result.corruptBackupPath);
    }
    return result;
}

/// @brief 只取指定媒体分区的条目。
QVector<MediaItem> load(MediaType type)
{
    const LoadResult result = loadAll();
    return type == MediaType::Video ? result.video : result.music;
}

/// @brief 原子写入主文件，成功后再刷新快照。
/// @return 主文件写入成功返回 true；快照失败只记日志，不影响主文件结果。
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
