#include "net/SourceRegistry.h"
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
  },
  {
    "id": "audius",
    "name": "Audius 音乐（二次解析示例）",
    "enabled": true,
    "builtin": false,
    "mediaType": "music",
    "method": "GET",
    "url": "https://api.audius.co/v1/tracks/search?query={keyword}&app_name=MVP&limit={limit}",
    "headers": { "User-Agent": "Mozilla/5.0" },
    "itemsPath": "data",
    "fields": {
      "id": "id",
      "title": "title",
      "artist": "user.name",
      "durationMs": "duration",
      "webUrl": "@https://audius.co/{permalink}"
    },
    "skipIf": { "any": [ { "path": "is_streamable", "op": "eq", "value": false } ] },
    "detailUrl": "https://api.audius.co/v1/tracks/{id}?app_name=MVP",
    "detailItemsPath": "data",
    "detailFields": { "streamUrl": "stream.url" }
  },
  {
    "id": "web_dmxq",
    "name": "网页源：大米星球（内置）",
    "enabled": true,
    "builtin": true,
    "kind": "builtin",
    "provider": "maccms_web",
    "mediaType": "video",
    "url": "https://dmxq.info/",
    "headers": { "Referer": "https://dmxq.info/" }
  },
  {
    "id": "web_studioslob",
    "name": "网页源：studioslob（内置，默认停用）",
    "enabled": false,
    "builtin": true,
    "kind": "builtin",
    "provider": "maccms_web",
    "mediaType": "video",
    "url": "https://www.studioslob.com/",
    "headers": { "Referer": "https://www.studioslob.com/" }
  },
  {
    "id": "movie_example",
    "name": "影视采集接口（示例，需自行填写）",
    "enabled": false,
    "builtin": false,
    "mediaType": "video",
    "streamKind": "episodes",
    "method": "GET",
    "url": "https://example.com/api.php/provide/vod/?ac=detail&wd={keyword}",
    "headers": { "User-Agent": "Mozilla/5.0" },
    "itemsPath": "list",
    "fields": {
      "id": "vod_id",
      "title": "vod_name",
      "year": "vod_year",
      "area": "vod_area",
      "genre": "vod_class",
      "posterUrl": "vod_pic",
      "description": "vod_content",
      "streamUrl": "vod_play_url",
      "webUrl": "@https://example.com/detail/{id}"
    },
    "skipIf": { "any": [ { "path": "vod_play_url", "op": "empty" } ] }
  }
]
)JSON";

QVector<SourceConfig> seedSources()
{
    QVector<SourceConfig> configs;
    QJsonParseError parseError{};
    const QJsonDocument seed = QJsonDocument::fromJson(QByteArray(kSeedSources), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return configs;
    for (const QJsonValue &entry : seed.array()) {
        bool ok = false;
        const SourceConfig config = sourceConfigFromJson(entry.toObject(), &ok);
        if (ok)
            configs.append(config);
    }
    return configs;
}

QString primaryDir()
{
    return StoragePaths::dataDir();
}

/// 安装目录只读时的备用位置，和播放列表保持同一套策略。
QString fallbackSourcePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/sources.json");
}

/// 损坏的源配置先改名备份，绝不能直接补种子覆盖用户原文件。
QString backupCorruptFile(const QString &path)
{
    if (!QFile::exists(path))
        return QString();
    const QString backup =
        path + QStringLiteral(".corrupt-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    if (QFile::copy(path, backup))
        return backup;
    return QString();
}

} // namespace

SourceRegistry::SourceRegistry(QObject *parent)
    : QObject(parent)
{
}

QString SourceRegistry::storagePath() const
{
    if (!m_storagePath.isEmpty())
        return m_storagePath;
    return QDir(primaryDir()).filePath(QStringLiteral("sources.json"));
}

bool SourceRegistry::load()
{
    m_sources.clear();
    m_lastError.clear();
    m_storagePath.clear();

    const QString primary = QDir(primaryDir()).filePath(QStringLiteral("sources.json"));
    const QString fallback = fallbackSourcePath();
    QStringList candidates;
    candidates.append(primary);
    if (!fallback.isEmpty() && fallback != primary)
        candidates.append(fallback);

    QString selectedPath;
    QJsonArray selectedArray;
    qint64 selectedMtime = -1;
    QString newestExisting;
    qint64 newestExistingMtime = -1;
    QStringList parseErrors;

    // 主备两份都读，选最新且可解析的一份；主目录只读时新数据落在用户数据目录。
    for (const QString &path : candidates) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
        if (mtime > newestExistingMtime) {
            newestExistingMtime = mtime;
            newestExisting = path;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            parseErrors.append(tr("%1：%2").arg(path, file.errorString()));
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
            const QString reason = parseError.error != QJsonParseError::NoError
                                       ? parseError.errorString()
                                       : tr("根节点不是数组");
            parseErrors.append(tr("%1：%2").arg(path, reason));
            continue;
        }
        if (mtime > selectedMtime) {
            selectedMtime = mtime;
            selectedPath = path;
            selectedArray = document.array();
        }
    }

    if (selectedPath.isEmpty()) {
        if (!newestExisting.isEmpty()) {
            const QString reason = parseErrors.join(QStringLiteral("；"));
            const QString backup = backupCorruptFile(newestExisting);
            if (backup.isEmpty()) {
                m_lastError = tr("源配置已损坏（%1），且无法备份原文件；未覆盖原文件。").arg(reason);
                qCWarning(lcRegistry) << m_lastError;
                return false;
            }
            m_lastError = tr("源配置已损坏（%1），原文件已备份为 %2，已恢复预置源。")
                              .arg(reason)
                              .arg(QFileInfo(backup).fileName());
            qCWarning(lcRegistry) << m_lastError;
        }

        m_sources = seedSources();
        qCInfo(lcRegistry) << "已写入预置源：" << m_sources.size() << "个";
        const QString corruptMessage = m_lastError;
        if (!save()) {
            if (!corruptMessage.isEmpty())
                m_lastError = corruptMessage + tr(" 预置源写入失败：%1").arg(m_lastError);
            return false;
        }
        // save() 会清空错误；恢复成功后仍要把“原文件已损坏并备份”带到界面提示
        m_lastError = corruptMessage;
        return true;
    }

    m_storagePath = selectedPath;
    const QJsonArray array = selectedArray;
    bool dirty = false;
    for (const QJsonValue &entry : array) {
        if (!entry.isObject())
            continue;
        const QJsonObject object = entry.toObject();
        if (!object.contains(QStringLiteral("id")))
            continue;
        // 单个源写坏不影响其它源：坏源标记停用并保留，交给界面修复
        bool ok = false;
        SourceConfig config = sourceConfigFromJson(object, &ok);
        // 已下线的内置实现（历史版 B 站源）：旧配置里残留的条目直接剔除并回写文件
        if (config.id.startsWith(QLatin1String("bilibili"))
            || config.provider.startsWith(QLatin1String("bilibili"))) {
            qCInfo(lcRegistry) << "移除已下线源：" << config.id << config.name;
            dirty = true;
            continue;
        }
        if (!ok) {
            config.enabled = false;
            qCWarning(lcRegistry) << "源配置无效，已停用：" << config.id << config.name;
        }
        m_sources.append(config);
    }
    // 版本升级后新增的内置源要补进来；用户自己已有的源一律不动
    for (const SourceConfig &seed : seedSources()) {
        if (!seed.builtin)
            continue;
        bool exists = false;
        for (const SourceConfig &config : m_sources) {
            if (config.id == seed.id) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            m_sources.append(seed);
            dirty = true;
            qCInfo(lcRegistry) << "补入缺失的内置源：" << seed.id;
        }
    }
    if (dirty && !save())
        return false;

    qCInfo(lcRegistry) << "已加载" << m_sources.size() << "个搜索源，来自" << storagePath();
    return true;
}

bool SourceRegistry::save()
{
    m_lastError.clear();

    QJsonArray array;
    for (const SourceConfig &config : m_sources)
        array.append(sourceConfigToJson(config));
    const QByteArray payload = QJsonDocument(array).toJson(QJsonDocument::Indented);

    const QString primary = QDir(primaryDir()).filePath(QStringLiteral("sources.json"));
    const QString fallback = fallbackSourcePath();
    QStringList targets;
    if (!m_storagePath.isEmpty())
        targets.append(m_storagePath);
    if (!targets.contains(primary))
        targets.append(primary);
    if (!fallback.isEmpty() && fallback != primary && !targets.contains(fallback))
        targets.append(fallback);

    QStringList errors;
    for (const QString &path : targets) {
        if (path.isEmpty())
            continue;
        QDir().mkpath(QFileInfo(path).absolutePath());

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            errors.append(tr("%1：%2").arg(path, file.errorString()));
            continue;
        }
        if (file.write(payload) != payload.size()) {
            errors.append(tr("%1：写入不完整").arg(path));
            file.cancelWriting();
            continue;
        }
        if (!file.commit()) {
            errors.append(tr("%1：%2").arg(path, file.errorString()));
            continue;
        }
        if (path != primary)
            qCWarning(lcRegistry) << "主数据目录不可写，源配置已改用备用路径" << path;
        m_storagePath = path;
        return true;
    }

    m_lastError = tr("无法写入源配置：%1").arg(errors.join(QStringLiteral("；")));
    qCWarning(lcRegistry) << m_lastError;
    return false;
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
    const QVector<SourceConfig> old = m_sources;
    bool found = false;
    for (SourceConfig &existing : m_sources) {
        if (existing.id == config.id) {
            existing = config;
            found = true;
            break;
        }
    }
    if (!found)
        m_sources.append(config);

    if (!save()) {
        m_sources = old;
        return false;
    }
    emit changed();
    return true;
}

bool SourceRegistry::remove(const QString &id)
{
    for (int i = 0; i < m_sources.size(); ++i) {
        if (m_sources.at(i).id != id)
            continue;
        if (m_sources.at(i).builtin) {
            qCWarning(lcRegistry) << "内置源不可删除：" << id;
            return false;
        }
        const QVector<SourceConfig> old = m_sources;
        m_sources.removeAt(i);
        if (!save()) {
            m_sources = old;
            return false;
        }
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
        if (config.enabled == enabled)
            return true;
        const QVector<SourceConfig> old = m_sources;
        config.enabled = enabled;
        if (!save()) {
            m_sources = old;
            return false;
        }
        emit changed();
        return true;
    }
    return false;
}

bool SourceRegistry::replaceAll(const QVector<SourceConfig> &sources)
{
    const QVector<SourceConfig> old = m_sources;
    m_sources = sources;
    if (!save()) {
        m_sources = old;
        return false;
    }
    emit changed();
    return true;
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
