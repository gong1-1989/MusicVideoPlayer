#include "net/SourceRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcRegistry, "mvp.sources")

namespace {

/// 预置源：网易云完全由配置描述，fee 过滤规则也在配置里，用户可自行修改
const char *kSeedSources = R"JSON(
[
  {
    "id": "netease",
    "name": "网易云音乐",
    "enabled": true,
    "builtin": true,
    "mediaType": "music",
    "method": "POST",
    "url": "https://music.163.com/api/cloudsearch/pc",
    "body": "s={keyword}&type=1&offset=0&limit={limit}",
    "bodyIsJson": false,
    "headers": {
      "Referer": "https://music.163.com/",
      "Origin": "https://music.163.com"
    },
    "itemsPath": "result.songs",
    "fields": {
      "id": "id",
      "title": "name",
      "artist": "ar[0].name",
      "durationMs": "dt",
      "streamUrl": "@https://music.163.com/song/media/outer/url?id={id}.mp3",
      "lyricUrl": "@https://music.163.com/api/song/lyric?lv=-1&kv=-1&tv=-1&id={id}",
      "webUrl": "@https://music.163.com/song?id={id}"
    },
    "skipIf": {
      "any": [
        { "path": "fee", "op": "eq", "value": 1 },
        { "all": [
            { "path": "fee", "op": "eq", "value": 4 },
            { "path": "privilege.pl", "op": "lte", "value": 0 }
        ] }
      ]
    }
  },
  {
    "id": "itunes",
    "name": "iTunes 试听（示例）",
    "enabled": false,
    "builtin": false,
    "mediaType": "music",
    "method": "GET",
    "url": "https://itunes.apple.com/search?term={keyword}&media=music&limit={limit}",
    "headers": { "User-Agent": "Mozilla/5.0" },
    "itemsPath": "results",
    "fields": {
      "id": "trackId",
      "title": "trackName",
      "artist": "artistName",
      "durationMs": "trackTimeMillis",
      "streamUrl": "previewUrl",
      "webUrl": "trackViewUrl"
    },
    "skipIf": { "any": [ { "path": "previewUrl", "op": "empty" } ] }
  }
]
)JSON";

QString primaryDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/Data");
}

} // namespace

SourceRegistry::SourceRegistry(QObject *parent)
    : QObject(parent)
{
}

QString SourceRegistry::storagePath() const
{
    return primaryDir() + QStringLiteral("/sources.json");
}

bool SourceRegistry::load()
{
    m_sources.clear();

    QDir dir;
    if (!dir.exists(primaryDir()))
        dir.mkpath(primaryDir());

    QFile file(storagePath());
    if (!file.exists()) {
        // 首次运行：写入预置源，之后它就是普通可编辑文件
        QJsonParseError parseError{};
        const QJsonDocument seed = QJsonDocument::fromJson(QByteArray(kSeedSources), &parseError);
        if (parseError.error == QJsonParseError::NoError) {
            for (const QJsonValue &entry : seed.array()) {
                bool ok = false;
                const SourceConfig config = sourceConfigFromJson(entry.toObject(), &ok);
                if (ok)
                    m_sources.append(config);
            }
        }
        qCInfo(lcRegistry) << "seed sources written:" << m_sources.size();
        save();
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcRegistry) << "cannot open" << storagePath() << file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(lcRegistry) << "sources.json broken:" << parseError.errorString();
        return false;
    }

    const QJsonArray array = document.isArray() ? document.array()
                                                : QJsonArray();
    for (const QJsonValue &entry : array) {
        if (!entry.isObject())
            continue;
        const QJsonObject object = entry.toObject();
        if (!object.contains(QStringLiteral("id")))
            continue;
        // 单个源写坏不影响其它源：坏源标记停用并保留，交给界面修复
        bool ok = false;
        SourceConfig config = sourceConfigFromJson(object, &ok);
        if (!ok) {
            config.enabled = false;
            qCWarning(lcRegistry) << "source invalid, disabled:" << config.id << config.name;
        }
        m_sources.append(config);
    }
    qCInfo(lcRegistry) << "loaded" << m_sources.size() << "sources from" << storagePath();
    return true;
}

bool SourceRegistry::save()
{
    QDir dir;
    if (!dir.exists(primaryDir()))
        dir.mkpath(primaryDir());

    QJsonArray array;
    for (const SourceConfig &config : m_sources)
        array.append(sourceConfigToJson(config));

    QSaveFile file(storagePath());
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(lcRegistry) << "cannot write" << storagePath() << file.errorString();
        return false;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        qCWarning(lcRegistry) << "commit failed" << storagePath();
        return false;
    }
    return true;
}

QVector<SourceConfig> SourceRegistry::enabledSources() const
{
    QVector<SourceConfig> result;
    for (const SourceConfig &config : m_sources) {
        if (config.enabled && config.isValid())
            result.append(config);
    }
    return result;
}

SourceConfig SourceRegistry::sourceById(const QString &id) const
{
    for (const SourceConfig &config : m_sources) {
        if (config.id == id)
            return config;
    }
    return SourceConfig();
}

bool SourceRegistry::upsert(const SourceConfig &config)
{
    for (SourceConfig &existing : m_sources) {
        if (existing.id == config.id) {
            existing = config;
            save();
            emit changed();
            return true;
        }
    }
    m_sources.append(config);
    save();
    emit changed();
    return true;
}

bool SourceRegistry::remove(const QString &id)
{
    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources.at(i).id != id)
            continue;
        if (m_sources.at(i).builtin) {
            qCWarning(lcRegistry) << "builtin source is not deletable:" << id;
            return false;
        }
        m_sources.removeAt(i);
        save();
        emit changed();
        return true;
    }
    return false;
}

bool SourceRegistry::setEnabled(const QString &id, bool enabled)
{
    for (SourceConfig &config : m_sources) {
        if (config.id != id)
            continue;
        config.enabled = enabled;
        save();
        emit changed();
        return true;
    }
    return false;
}

int SourceRegistry::enabledCount() const
{
    int count = 0;
    for (const SourceConfig &config : m_sources) {
        if (config.enabled && config.isValid())
            ++count;
    }
    return count;
}
