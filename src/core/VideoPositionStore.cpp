#include "core/VideoPositionStore.h"
#include "storage/StoragePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QPair>
#include <QSaveFile>
#include <QStringList>
#include <QVector>

#include <algorithm>

Q_LOGGING_CATEGORY(lcPosition, "mvp.position")

namespace {

constexpr qint64 kMinResumeMs = 3000;
constexpr qint64 kTailGuardMs = 3000;
constexpr int kMaxEntries = 300;

QString primaryPath()
{
    return StoragePaths::dataFile(QStringLiteral("video_positions.json"));
}

QJsonObject readRoot()
{
    QFile file(primaryPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    const QByteArray payload = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcPosition) << "video_positions.json 损坏，按空记录处理";
        return {};
    }
    return doc.object();
}

bool writeRoot(const QJsonObject &root)
{
    const QString path = primaryPath();
    if (path.isEmpty())
        return false;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const qint64 written = file.write(payload);
    if (written != payload.size()) {
        qCWarning(lcPosition) << "播放进度写入不完整：" << path;
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        qCWarning(lcPosition) << "播放进度写入失败：" << path << "：" << file.errorString();
        return false;
    }
    return true;
}

QJsonObject entryFor(const MediaItem &item, qint64 positionMs, qint64 durationMs)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("title"), item.displayName());
    entry.insert(QStringLiteral("positionMs"), QJsonValue::fromVariant(positionMs));
    entry.insert(QStringLiteral("durationMs"), QJsonValue::fromVariant(durationMs));
    entry.insert(QStringLiteral("updatedAt"),
                 QJsonValue::fromVariant(QDateTime::currentMSecsSinceEpoch()));
    return entry;
}

} // namespace

namespace VideoPositionStore {

QString filePath()
{
    return primaryPath();
}

qint64 positionFor(const MediaItem &item)
{
    if (item.type != MediaType::Video)
        return 0;

    const QString key = mediaIdentity(item);
    if (key.isEmpty())
        return 0;

    const QJsonObject root = readRoot();
    const QJsonValue value = root.value(key);
    if (!value.isObject())
        return 0;

    const QJsonObject entry = value.toObject();
    const qint64 position = entry.value(QStringLiteral("positionMs"))
                                .toVariant()
                                .toLongLong();
    const qint64 duration = entry.value(QStringLiteral("durationMs"))
                                .toVariant()
                                .toLongLong();

    if (position < kMinResumeMs)
        return 0;
    if (duration > 0 && position + kTailGuardMs >= duration)
        return 0;
    return position;
}

bool save(const MediaItem &item, qint64 positionMs, qint64 durationMs)
{
    if (item.type != MediaType::Video)
        return false;

    const QString key = mediaIdentity(item);
    if (key.isEmpty())
        return false;

    // 片头几秒和片尾都不值得恢复：下次从头播放更符合预期。
    if (positionMs < kMinResumeMs
        || (durationMs > 0 && positionMs + kTailGuardMs >= durationMs)) {
        return remove(item);
    }

    QJsonObject root = readRoot();
    root.insert(key, entryFor(item, positionMs, durationMs));

    if (root.size() > kMaxEntries) {
        QVector<QPair<qint64, QString>> oldest;
        oldest.reserve(root.size());
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            const QJsonObject entry = it.value().toObject();
            const qint64 updated = entry.value(QStringLiteral("updatedAt"))
                                       .toVariant()
                                       .toLongLong();
            oldest.append(qMakePair(updated, it.key()));
        }
        std::sort(oldest.begin(), oldest.end(),
                  [](const QPair<qint64, QString> &left,
                     const QPair<qint64, QString> &right) {
                      return left.first < right.first;
                  });
        const int removeCount = root.size() - kMaxEntries;
        for (int i = 0; i < removeCount && i < oldest.size(); ++i)
            root.remove(oldest.at(i).second);
    }

    if (!writeRoot(root)) {
        qCWarning(lcPosition) << "视频进度保存失败：" << item.displayName();
        return false;
    }
    qCInfo(lcPosition) << "已保存视频进度" << item.displayName() << positionMs << "/"
                       << durationMs;
    return true;
}

bool remove(const MediaItem &item)
{
    if (item.type != MediaType::Video)
        return false;

    const QString key = mediaIdentity(item);
    if (key.isEmpty())
        return false;

    QJsonObject root = readRoot();
    if (!root.contains(key))
        return true;

    root.remove(key);
    if (!writeRoot(root)) {
        qCWarning(lcPosition) << "视频进度删除失败：" << item.displayName();
        return false;
    }
    qCInfo(lcPosition) << "已清除视频进度" << item.displayName();
    return true;
}

} // namespace VideoPositionStore
