#include "net/SourceRegistry.h"
#include "storage/StoragePaths.h"
#include "util/HeaderPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSet>

Q_LOGGING_CATEGORY(lcRegistry, "mvp.sources")

namespace {

/// 源配置通常只有几十 KB；限制读取上限，避免误选/损坏文件把内存撑满。
constexpr qint64 kMaxSourceFileBytes = 16 * 1024 * 1024;

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
    "name": "iTunes 音乐（试听片段，可选）",
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
    "name": "Audius 音乐（完整曲目，可选）",
    "enabled": false,
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
    "id": "radio_browser",
    "name": "Radio Browser 电台（可选）",
    "enabled": false,
    "builtin": false,
    "mediaType": "music",
    "method": "GET",
    "url": "https://de1.api.radio-browser.info/json/stations/search?name={keyword}&limit={limit}",
    "headers": { "User-Agent": "Mozilla/5.0" },
    "itemsPath": "",
    "fields": {
      "id": "stationuuid",
      "title": "name",
      "artist": "country",
      "streamUrl": "url_resolved",
      "webUrl": "homepage"
    },
    "skipIf": { "any": [ { "path": "url_resolved", "op": "empty" } ] }
  },
  {
    "id": "ccmixter",
    "name": "ccMixter 音乐（完整曲目，可选）",
    "enabled": false,
    "builtin": false,
    "mediaType": "music",
    "method": "GET",
    "url": "https://ccmixter.org/api/query?f=json&search={keyword}&limit={limit}",
    "headers": { "User-Agent": "Mozilla/5.0" },
    "itemsPath": "",
    "fields": {
      "title": "upload_name",
      "streamUrl": "files[0].download_url"
    },
    "skipIf": { "any": [ { "path": "files[0].download_url", "op": "empty" } ] }
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

/// 预置源解析一次后复用：load() 会在逐条校验和补入缺失内置源时多次
/// 访问同一份 JSON，避免重复构造 QJsonDocument 和解析 8 个配置对象。
const QVector<SourceConfig> &seedSources()
{
    static const QVector<SourceConfig> configs = [] {
        QVector<SourceConfig> parsed;
        QJsonParseError parseError{};
        const QJsonDocument seed = QJsonDocument::fromJson(QByteArray(kSeedSources), &parseError);
        if (parseError.error != QJsonParseError::NoError)
            return parsed;
        for (const QJsonValue &entry : seed.array()) {
            bool ok = false;
            const SourceConfig config = sourceConfigFromJson(entry.toObject(), &ok);
            if (ok)
                parsed.append(config);
        }
        return parsed;
    }();
    return configs;
}

QString primaryDir()
{
    return StoragePaths::dataDir();
}

/// 内置源只允许用户改 enabled；实现、地址、请求头等必须始终以预置配置为准。
/// upsert/replaceAll/load 共用这套逻辑，避免只有 load 做保护、别的入口能绕过。
SourceConfig protectBuiltinConfig(const SourceConfig &input, bool *changed)
{
    SourceConfig config = input;
    if (changed != nullptr)
        *changed = false;

    const SourceConfig *seed = nullptr;
    for (const SourceConfig &candidate : seedSources()) {
        if (candidate.builtin && candidate.id == config.id) {
            seed = &candidate;
            break;
        }
    }

    if (seed == nullptr) {
        // 非预置源不能通过配置文件自称内置，否则会绕过“内置源不可删除”。
        if (config.builtin) {
            config.builtin = false;
            if (changed != nullptr)
                *changed = true;
        }
        return config;
    }

    SourceConfig protectedSeed = *seed;
    protectedSeed.enabled = config.enabled;
    const bool differs =
        sourceConfigToJson(protectedSeed, true) != sourceConfigToJson(config, true);
    if (differs && changed != nullptr)
        *changed = true;
    return protectedSeed;
}

/// 损坏的源配置先改名备份，绝不能直接补种子覆盖用户原文件。
QString backupCorruptFile(const QString &path)
{
    if (!QFile::exists(path))
        return QString();
    const QString base =
        path + QStringLiteral(".corrupt-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    // 同一秒内连续损坏/重载时，固定时间戳会撞名导致 QFile::copy 失败。
    // 追加序号直到找到未占用名字；非“已存在”的复制失败仍按真实 IO 失败处理。
    for (int suffix = 0; suffix < 100; ++suffix) {
        const QString candidate =
            suffix == 0 ? base : base + QStringLiteral("-") + QString::number(suffix);
        if (QFile::exists(candidate))
            continue;
        if (QFile::copy(path, candidate))
            return candidate;
        if (!QFile::exists(candidate))
            return QString();
    }
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
    QString selectedPath;
    QJsonArray selectedArray;
    QString parseErrorReason;
    bool unreadable = false;

    // 源配置只读程序目录下的 Data/sources.json，不再回退用户 AppData。
    const QFileInfo info(primary);
    if (info.exists()) {
        if (!info.isFile()) {
            m_lastError = tr("%1：不是普通文件；未备份、未覆盖。").arg(primary);
            unreadable = true;
        } else {
            QFile file(primary);
            if (!file.open(QIODevice::ReadOnly)) {
                // 打不开不等于内容损坏：文件被占用/权限不足时备份通常也会失败，
                // 不能把它当损坏文件删改，否则一次临时占用会丢掉用户配置。
                m_lastError = tr("%1：无法读取（%2）；未备份、未覆盖。")
                                  .arg(primary, file.errorString());
                unreadable = true;
            } else if (file.size() > kMaxSourceFileBytes) {
                parseErrorReason = tr("%1：文件超过 %2 MB 上限")
                                       .arg(primary)
                                       .arg(kMaxSourceFileBytes / (1024 * 1024));
                file.close();
            } else {
                QJsonParseError parseError{};
                const QJsonDocument document =
                    QJsonDocument::fromJson(file.readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
                    const QString reason = parseError.error != QJsonParseError::NoError
                                               ? parseError.errorString()
                                               : tr("根节点不是数组");
                    parseErrorReason = tr("%1：%2").arg(primary, reason);
                } else {
                    selectedPath = primary;
                    selectedArray = document.array();
                }
            }
        }
    }

    if (unreadable) {
        qCWarning(lcRegistry) << m_lastError;
        return false;
    }

    if (selectedPath.isEmpty()) {
        if (!parseErrorReason.isEmpty()) {
            const QString backup = backupCorruptFile(primary);
            if (backup.isEmpty()) {
                m_lastError = tr("源配置已损坏（%1），且无法备份原文件；未覆盖原文件。")
                                  .arg(parseErrorReason);
                qCWarning(lcRegistry) << m_lastError;
                return false;
            }
            m_lastError = tr("源配置已损坏（%1），原文件已备份为 %2。")
                              .arg(parseErrorReason)
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
        // 只有真的写盘成功后才能说“已恢复预置源”，否则用户会以为损坏文件已被修好。
        if (!corruptMessage.isEmpty())
            m_lastError = corruptMessage + tr(" 已恢复预置源。");
        return true;
    }

    m_storagePath = selectedPath;
    const QJsonArray array = selectedArray;
    bool dirty = false;
    QSet<QString> seenIds;
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
        if (config.id.isEmpty() || seenIds.contains(config.id)) {
            qCWarning(lcRegistry) << "重复/空 sourceId，已剔除：" << config.id << config.name;
            dirty = true;
            continue;
        }
        // 内置源 ID 的实现、地址和头部不允许被 sources.json 改写；
        // 只保留用户对启用状态的选择，避免“自定义源声明 builtin”绕过限制。
        bool builtinChanged = false;
        const SourceConfig protectedConfig = protectBuiltinConfig(config, &builtinChanged);
        if (builtinChanged) {
            dirty = true;
            if (protectedConfig.builtin)
                qCWarning(lcRegistry) << "内置源 ID 被外部配置改写，已恢复预置实现："
                                      << protectedConfig.id;
            else
                qCWarning(lcRegistry) << "非预置源声明 builtin，已清除标记：" << config.id;
        }
        config = protectedConfig;
        seenIds.insert(config.id);
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
    QStringList errors;

    auto writeTarget = [&](const QString &path) {
        if (path.isEmpty())
            return false;
        QDir().mkpath(QFileInfo(path).absolutePath());

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            errors.append(tr("%1：%2").arg(path, file.errorString()));
            return false;
        }
        if (file.write(payload) != payload.size()) {
            errors.append(tr("%1：写入不完整").arg(path));
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            errors.append(tr("%1：%2").arg(path, file.errorString()));
            return false;
        }
        return true;
    };

    // 只写程序目录下的 Data/sources.json；不再双写或回退用户 AppData。
    const bool primaryOk = writeTarget(primary);

    if (primaryOk) {
        m_storagePath = primary;
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

MediaItem SourceRegistry::mediaItemWithCredentials(const MediaItem &item) const
{
    if (item.sourceId.isEmpty())
        return item;

    const SourceConfig config = sourceById(item.sourceId);
    if (config.id.isEmpty())
        return item;

    MediaItem result = item;
    // 凭据作用域只认用户配置的源站。远程 webUrl/旧条目的 sourceOrigin 都可能被
    // 搜索结果或外部解析器改写，不能作为 Cookie 发往哪个 host 的依据。
    const QUrl trustedUrl(config.urlTemplate);
    const QUrl origin = HeaderPolicy::originOnly(trustedUrl);

    if (origin.isValid() && !origin.host().isEmpty()) {
        result.sourceOrigin = origin;
        // 源配置更新后旧条目的 Referer 可能过期，按当前源站重建；
        // 配置里的显式 Referer/Origin 仍会在下面覆盖回来。
        result.headers.remove(QStringLiteral("Referer"));
        result.headers.remove(QStringLiteral("Origin"));
        result.headers = HeaderPolicy::withSourceHeaders(result.headers, trustedUrl);
    }

    // 播放列表落盘时剥离了敏感头，这里只从内存中的 SourceRegistry 恢复，
    // 不把 Cookie 重新写回磁盘。
    for (auto it = config.headers.cbegin(); it != config.headers.cend(); ++it)
        result.headers.insert(it.key(), it.value());
    if (!config.cookie.isEmpty())
        result.headers.insert(QStringLiteral("Cookie"), config.cookie);

    if (!origin.isValid() || origin.host().isEmpty()) {
        // 没有可信源站时 fail closed：配置里残留的 Referer/Origin 也不能成为
        // 请求层推断凭据作用域的依据，敏感头一并剥离。
        result.sourceOrigin = QUrl();
        result.headers.remove(QStringLiteral("Referer"));
        result.headers.remove(QStringLiteral("Origin"));
        result.headers = HeaderPolicy::withoutSensitiveHeaders(result.headers);
    }

    return result;
}

bool SourceRegistry::upsert(const SourceConfig &config)
{
    if (config.id.trimmed().isEmpty()) {
        m_lastError = tr("源配置缺少 id，未保存");
        qCWarning(lcRegistry) << m_lastError;
        return false;
    }

    // 与 load 同一套内置保护：按 id 命中预置源时只接受 enabled 变化，
    // 非预置源不能把自己的 builtin 改成 true。
    bool builtinChanged = false;
    const SourceConfig safeConfig = protectBuiltinConfig(config, &builtinChanged);
    if (builtinChanged && safeConfig.builtin)
        qCWarning(lcRegistry) << "内置源不可改写实现，已恢复预置配置：" << safeConfig.id;
    else if (builtinChanged)
        qCWarning(lcRegistry) << "非预置源不能声明 builtin，已清除标记：" << safeConfig.id;

    const QVector<SourceConfig> old = m_sources;
    QVector<SourceConfig> next = m_sources;
    bool found = false;
    for (SourceConfig &existing : next) {
        if (existing.id == safeConfig.id) {
            existing = safeConfig;
            found = true;
            break;
        }
    }
    if (!found)
        next.append(safeConfig);

    m_sources = next;
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
    // 整表提交也要做与单条保存相同的校验，否则空 id/重复 id 会直接落盘，
    // 下次加载时才被剔除，用户会以为“保存成功但源消失了”。
    QVector<SourceConfig> next;
    next.reserve(sources.size() + seedSources().size());
    QSet<QString> ids;
    for (const SourceConfig &input : sources) {
        const QString id = input.id.trimmed();
        if (id.isEmpty()) {
            m_lastError = tr("有源缺少 id，整表未保存");
            qCWarning(lcRegistry) << m_lastError;
            return false;
        }
        if (ids.contains(id)) {
            m_lastError = tr("源 id 重复：%1，整表未保存").arg(id);
            qCWarning(lcRegistry) << m_lastError;
            return false;
        }
        ids.insert(id);

        bool builtinChanged = false;
        SourceConfig config = protectBuiltinConfig(input, &builtinChanged);
        if (builtinChanged && config.builtin)
            qCWarning(lcRegistry) << "内置源 ID 被改写，已恢复预置实现：" << config.id;
        else if (builtinChanged)
            qCWarning(lcRegistry) << "非预置源声明 builtin，已清除标记：" << config.id;
        next.append(config);
    }
    // 内置源不可删除：整表里漏掉的预置源按默认配置补回，避免通过整表提交绕过限制。
    for (const SourceConfig &seed : seedSources()) {
        if (!seed.builtin || ids.contains(seed.id))
            continue;
        next.append(seed);
        ids.insert(seed.id);
        qCWarning(lcRegistry) << "整表提交缺少内置源，已补回：" << seed.id;
    }

    const QVector<SourceConfig> old = m_sources;
    m_sources = next;
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
