#include "core/PlaylistStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
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
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Data/playlists.json"));
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
    file.write(payload);
    if (!file.commit()) {
        qCWarning(lcStore) << "播放列表写入失败：" << path << "：" << file.errorString();
        return false;
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

    QString path = primaryPath();
    if (!QFile::exists(path)) {
        // 旧版本把列表放在 C 盘 AppData，首次运行时做一次无缝迁移读取
        const QString legacy = fallbackPath();
        if (!legacy.isEmpty() && QFile::exists(legacy)) {
            qCInfo(lcStore) << "从旧位置读取播放列表：" << legacy;
            path = legacy;
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return items;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        qCWarning(lcStore) << "playlists.json 根节点不是对象";
        return items;
    }

    const QJsonArray array = doc.object().value(keyFor(type)).toArray();
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

void saveAll(const QVector<MediaItem> &music, const QVector<MediaItem> &video)
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

    if (writeFile(primaryPath(), payload)) {
        qCInfo(lcStore) << "播放列表已保存（音乐" << music.size() << "条、视频" << video.size()
                        << "条）到" << primaryPath();
        return;
    }

    // 程序目录只读（例如装进 Program Files）时退回用户数据目录，不让保存静默失败
    const QString fallback = fallbackPath();
    qCWarning(lcStore) << "无法写入" << primaryPath() << "，改用备用路径" << fallback;
    writeFile(fallback, payload);
}

void save(MediaType type, const QVector<MediaItem> &items)
{
    if (type == MediaType::Music)
        saveAll(items, load(MediaType::Video));
    else
        saveAll(load(MediaType::Music), items);
}

} // namespace PlaylistStore
