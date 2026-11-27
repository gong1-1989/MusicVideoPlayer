/// @file SourceRegistry.cpp
/// @brief 搜索源配置的加载、校验、内置保护与持久化实现。
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

/// 预置源：只保留长期可用的三个源；其余示例/试验源不再随版本补入。
/// 网易云完全由配置描述，fee 过滤规则也在配置里，用户可自行修改。
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
    "id": "web_dmxq",
    "name": "大米星球",
    "enabled": true,
    "builtin": true,
    "kind": "builtin",
    "provider": "maccms_web",
    "mediaType": "video",
    "url": "https://dmxq.info/",
    "headers": { "Referer": "https://dmxq.info/" }
  },
  {
    "id": "audius",
    "name": "Audius 音乐",
    "enabled": true,
    "builtin": true,
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
  }
]
)JSON";

/// 已下线的历史预置源：旧 sources.json 里可能仍留有这些条目。
/// 只按 id 清理，避免用户升级后继续看到失效的示例/试验源。
bool isRetiredSeedId(const QString &id)
{
    return id == QLatin1String("itunes")
        || id == QLatin1String("radio_browser")
        || id == QLatin1String("ccmixter")
        || id == QLatin1String("web_studioslob")
        || id == QLatin1String("movie_example");
}

/// @brief 返回解析后的预置源列表（只解析一次）。
/// @note load() 在逐条校验和补入缺失内置源时会多次访问同一份 JSON，
///       静态缓存避免重复构造 QJsonDocument。
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

/// @brief 返回源配置文件所在目录（程序目录下的 Data/）。
QString primaryDir()
{
    return StoragePaths::dataDir();
}

/// 最近一次成功写盘的主文件快照。主文件损坏时优先恢复它，避免只回到预置源。
QString backupPath()
{
    return QDir(primaryDir()).filePath(QStringLiteral("sources.json.bak"));
}

/// @brief 读取一个源配置数组文件。
/// @return 文件存在、可读、未超限且根节点是数组时返回 true。
bool readSourceArray(const QString &path, QJsonArray *array, QString *reason)
{
    if (array == nullptr)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (reason != nullptr)
            *reason = file.errorString();
        return false;
    }
    if (file.size() > kMaxSourceFileBytes) {
        if (reason != nullptr)
            *reason = QStringLiteral("文件超过大小上限");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (reason != nullptr) {
            *reason = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("根节点不是数组");
        }
        return false;
    }
    *array = document.array();
    return true;
}

/// @brief 恢复内置源被外部配置改写过的字段。
/// @param input 待校验配置。
/// @param changed 输出是否发生了修正，可为空。
/// @return 内置 id 只保留用户选择的 enabled，其余字段以预置配置为准；
///         非预置源强制清除 builtin 标记。
/// @note upsert/replaceAll/load 共用这套逻辑，避免只有 load 做保护、别的入口能绕过。
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

/// @brief 把损坏的 sources.json 改名备份。
/// @param path 原文件路径。
/// @return 备份文件路径；原文件不存在或连续复制失败时返回空。
/// @note 绝不直接覆盖用户原文件；同一秒内多次损坏时追加序号防撞名。
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

/// @brief 构造注册表；此时尚未加载磁盘配置。
SourceRegistry::SourceRegistry(QObject *parent)
    : QObject(parent)
{
}

/// @brief 返回当前实际使用的配置文件路径。
/// @return 上次成功读写的路径；从未加载/保存时回退程序目录下的 sources.json。
QString SourceRegistry::storagePath() const
{
    if (!m_storagePath.isEmpty())
        return m_storagePath;
    return QDir(primaryDir()).filePath(QStringLiteral("sources.json"));
}

/// @brief 从磁盘加载源配置。
/// @return 读取并校验成功返回 true；文件不可读/损坏且无法备份时返回 false。
/// @note 文件不存在或内容损坏时会补入预置源并尝试写盘；不可读（占用/权限）
///       只报错不备份、不覆盖，避免一次临时占用丢配置。
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

    bool restoredFromBackup = false;
    if (selectedPath.isEmpty()) {
        QJsonArray backupArray;
        QString backupReason;
        if (readSourceArray(backupPath(), &backupArray, &backupReason)) {
            if (!parseErrorReason.isEmpty() && info.exists()) {
                const QString corruptBackup = backupCorruptFile(primary);
                if (corruptBackup.isEmpty()) {
                    m_lastError = tr("源配置已损坏（%1），且无法备份原文件；未覆盖原文件。")
                                      .arg(parseErrorReason);
                    qCWarning(lcRegistry) << m_lastError;
                    return false;
                }
                m_lastError = tr("源配置已损坏（%1），原文件已备份为 %2。")
                                  .arg(parseErrorReason, QFileInfo(corruptBackup).fileName());
                qCWarning(lcRegistry) << m_lastError;
            }
            selectedPath = primary;
            selectedArray = backupArray;
            restoredFromBackup = true;
            m_lastError = tr("已从最近一次成功保存的备份恢复源配置。");
            qCInfo(lcRegistry) << m_lastError;
        } else {
            // 备份文件存在但读不出来时，它可能是用户自定义源的最后一份数据。
            // 不能直接写预置源把 sources.json.bak 覆盖掉；先改名保留损坏内容，
            // 无法备份时宁可本次加载失败，也不破坏唯一的恢复源。
            if (QFileInfo::exists(backupPath()) && !backupReason.isEmpty()) {
                const QString corruptBackup = backupCorruptFile(backupPath());
                if (corruptBackup.isEmpty()) {
                    m_lastError = tr("源配置备份无法读取（%1），且无法备份；未覆盖原文件。")
                                      .arg(backupReason);
                    qCWarning(lcRegistry) << m_lastError;
                    return false;
                }
                m_lastError = tr("源配置备份无法读取（%1），已备份为 %2。")
                                  .arg(backupReason, QFileInfo(corruptBackup).fileName());
                qCWarning(lcRegistry) << m_lastError;
            }
            if (!parseErrorReason.isEmpty()) {
                const QString corruptBackup = backupCorruptFile(primary);
                if (corruptBackup.isEmpty()) {
                    m_lastError = tr("源配置已损坏（%1），且无法备份原文件；未覆盖原文件。")
                                      .arg(parseErrorReason);
                    qCWarning(lcRegistry) << m_lastError;
                    return false;
                }
                m_lastError = tr("源配置已损坏（%1），原文件已备份为 %2。")
                                  .arg(parseErrorReason, QFileInfo(corruptBackup).fileName());
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
    }

    m_storagePath = selectedPath;
    const QJsonArray array = selectedArray;
    bool dirty = restoredFromBackup;
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
        // 已下线的 bilibili 内置源：旧配置里残留的条目直接剔除并回写文件
        if (config.id.startsWith(QLatin1String("bilibili"))
            || config.provider.startsWith(QLatin1String("bilibili"))) {
            qCInfo(lcRegistry) << "移除已下线源：" << config.id << config.name;
            dirty = true;
            continue;
        }
        // 版本升级后只保留长期可用的三个内置源；旧 itunes/电台/示例源一并清理。
        if (isRetiredSeedId(config.id)) {
            qCInfo(lcRegistry) << "移除已下线示例源：" << config.id << config.name;
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
    if (dirty) {
        // save() 会先清空 m_lastError；恢复快照/剔除坏源后要回写给用户看的
        // 提示不能被写盘成功顺手抹掉，否则界面会以为什么都没发生。
        const QString loadMessage = m_lastError;
        if (!save()) {
            if (!loadMessage.isEmpty())
                m_lastError = loadMessage + tr(" 恢复回写失败：%1").arg(m_lastError);
            return false;
        }
        m_lastError = loadMessage;
    }

    qCInfo(lcRegistry) << "已加载" << m_sources.size() << "个搜索源，来自" << storagePath();
    return true;
}

/// @brief 把当前源列表原子写入 sources.json。
/// @return 写入并 commit 成功返回 true；失败时保留旧文件并记录 m_lastError。
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
        // 主文件提交成功后再刷新快照；备份失败不影响本次保存结果，
        // 但下次主文件损坏时就少了一层恢复手段，因此必须留日志。
        if (!writeTarget(backupPath()))
            qCWarning(lcRegistry) << "源配置快照写入失败：" << backupPath();
        return true;
    }

    m_lastError = tr("无法写入源配置：%1").arg(errors.join(QStringLiteral("；")));
    qCWarning(lcRegistry) << m_lastError;
    return false;
}

/// @brief 返回启用且配置有效的源，供搜索协调器使用。
QVector<SourceConfig> SourceRegistry::enabledSources() const
{
    QVector<SourceConfig> result;
    for (const SourceConfig &config : m_sources) {
        if (config.enabled && config.isValid())
            result.append(config);
    }
    return result;
}

/// @brief 按 id 查找源配置。
/// @return 命中副本；不存在时返回空配置（id 为空）。
SourceConfig SourceRegistry::sourceById(const QString &id) const
{
    for (const SourceConfig &config : m_sources) {
        if (config.id == id)
            return config;
    }
    return SourceConfig();
}

/// @brief 为播放条目补回该源的内存凭据与受信源站。
/// @param item 待播放条目，通常来自播放列表或搜索结果。
/// @return 补全后的副本；源 id 不存在/为空时原样返回。
/// @note 敏感头只在内存中恢复，不回写磁盘；没有可信源站时 fail-closed，
///       剥离 Referer/Origin 等敏感头。
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

/// @brief 新增或替换一条源配置。
/// @param config 待保存配置；id 为空会拒绝。
/// @return 保存成功返回 true；失败时回滚内存列表并保留 m_lastError。
/// @note 与 load 共用内置保护：内置源只接受 enabled 变化，非预置源不能自称 builtin。
bool SourceRegistry::upsert(const SourceConfig &config)
{
    if (config.id.trimmed().isEmpty()) {
        m_lastError = tr("源配置缺少 id，未保存");
        qCWarning(lcRegistry) << m_lastError;
        return false;
    }
    if (isRetiredSeedId(config.id)) {
        m_lastError = tr("示例源 %1 已下线，不能重新添加").arg(config.id);
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

/// @brief 删除指定源。
/// @return 删除并保存成功返回 true；内置源、找不到或写盘失败返回 false。
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

/// @brief 设置指定源的启用状态。
/// @return 状态未变或保存成功返回 true；源不存在/写盘失败返回 false。
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

/// @brief 整表替换源配置。
/// @param sources 新列表；空 id/重复 id 会整表拒绝。
/// @return 全部校验并保存成功返回 true；失败时回滚旧列表。
/// @note 处理与单条保存一致，并自动补回被漏掉的内置源，防止绕过“内置源不可删除”。
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
        if (isRetiredSeedId(id))
            continue;
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
