#include "core/PlaylistStore.h"

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

} // namespace

namespace PlaylistStore {

QString filePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return QString();
    return dir + QStringLiteral("/playlists.json");
}

QVector<MediaItem> load(MediaType type)
{
    QVector<MediaItem> items;

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return items;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        qCWarning(lcStore) << "playlists.json is not an object";
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
    const QString path = filePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject root;
    for (const auto &entry : {std::make_pair(MediaType::Music, &music),
                              std::make_pair(MediaType::Video, &video)}) {
        QJsonArray array;
        for (const MediaItem &item : *entry.second)
            array.append(mediaItemToJson(item));
        root.insert(keyFor(entry.first), array);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(lcStore) << "cannot open" << path << "for writing:" << file.errorString();
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qCWarning(lcStore) << "cannot commit" << path << ":" << file.errorString();
}

void save(MediaType type, const QVector<MediaItem> &items)
{
    if (type == MediaType::Music)
        saveAll(items, load(MediaType::Video));
    else
        saveAll(load(MediaType::Music), items);
}

} // namespace PlaylistStore
