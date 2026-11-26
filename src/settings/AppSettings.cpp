/// @file AppSettings.cpp
/// @brief 应用设置的键名、默认值、范围收口与兼容读取实现。
#include "settings/AppSettings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace {

// QSettings 键名集中在这里，避免界面层散落字符串。历史键名只在迁移/兼容
// 函数中读取，不再作为新写入目标。
const char kTheme[] = "ui/theme";
const char kFontSizePx[] = "ui/fontSizePx";
const char kLegacyFontPercent[] = "ui/fontPercent";
const char kWindowPreset[] = "ui/windowPreset";
const char kGeometry[] = "ui/geometry";
const char kVolume[] = "playback/volume";
const char kMode[] = "playback/mode";
const char kLastItemType[] = "playback/lastItemType";
const char kLastItemIdentity[] = "playback/lastItemIdentity";
const char kSearchLimit[] = "search/limit";
const char kSearchHistory[] = "search/history";
const char kDownloadLimitMb[] = "download/limitMb";
const char kDownloadDir[] = "download/dir";
const char kRemovePlaybackCacheAfterDownload[] = "download/removePlaybackCache";
const char kSourceParserCommand[] = "search/sourceParser/command";
const char kSourceParserArguments[] = "search/sourceParser/arguments";
const char kSourceParserTimeoutMs[] = "search/sourceParser/timeoutMs";
const char kLastDir[] = "paths/lastDir";
const char kPlaylistMusic[] = "playlists/music";
const char kPlaylistVideo[] = "playlists/video";
const char kSidebarAutoHide[] = "ui/sidebarAutoHide";
const char kSidebarWidth[] = "ui/sidebarWidth";
const char kControlsAutoHide[] = "ui/controlsAutoHide";
const char kShortcutPrefix[] = "shortcuts/";

const char kDefaultTheme[] = "dark";
const char kDefaultWindowPreset[] = "auto";
const int kDefaultFontSizePx = 13;
const int kMinFontSizePx = 9;
const int kMaxFontSizePx = 20;
const int kFallbackVolume = 60;
const int kDefaultSearchLimit = 20;
const int kDefaultDownloadLimitMb = 8192;
const int kDefaultMode = 0;
/// 内置解析器与主程序一起发布；只写文件名，运行时按程序目录解析。
const char kDefaultSourceParserCommand[] = "source-probe-cli.exe";
const char kDefaultSourceParserArguments[] =
    "--site {site} --type {type} --mode {mode}";
const int kDefaultSourceParserTimeoutMs = 120000;

/// @brief 每次读写都创建新的 QSettings，保证跨线程/多窗口看到最新值。
QSettings settings()
{
    return QSettings();
}

} // namespace

namespace AppSettings {

QString themeId()
{
    return settings().value(QLatin1String(kTheme), QLatin1String(kDefaultTheme)).toString();
}

void setThemeId(const QString &id)
{
    settings().setValue(QLatin1String(kTheme), id);
}

/// @brief 读取界面基础字号（px）。
/// @return 9..20 之间的像素字号；新键缺失时把旧版百分比按 13px=100% 换算。
/// @note 旧键 ui/fontPercent 只在首次升级时读取，之后不再写回。
int uiFontSizePx()
{
    QSettings store;
    if (store.contains(QLatin1String(kFontSizePx))) {
        return qBound(kMinFontSizePx,
                      store.value(QLatin1String(kFontSizePx), kDefaultFontSizePx).toInt(),
                      kMaxFontSizePx);
    }

    // 旧版本保存的是 80..150 的百分比，首次升级时换算成真实像素字号；
    // 以 13px 为 100%，避免老用户一启动字号突然变化。
    const int legacyPercent =
        store.value(QLatin1String(kLegacyFontPercent), 100).toInt();
    return qBound(kMinFontSizePx,
                  qRound(kDefaultFontSizePx * qBound(80, legacyPercent, 150) / 100.0),
                  kMaxFontSizePx);
}

void setUiFontSizePx(int pixels)
{
    settings().setValue(QLatin1String(kFontSizePx),
                        qBound(kMinFontSizePx, pixels, kMaxFontSizePx));
}

QString windowPresetId()
{
    return settings().value(QLatin1String(kWindowPreset),
                            QLatin1String(kDefaultWindowPreset))
        .toString();
}

void setWindowPresetId(const QString &id)
{
    settings().setValue(QLatin1String(kWindowPreset), id);
}

WindowPreset windowPreset()
{
    return windowPresetFromId(windowPresetId());
}

QSize presetWindowSize(const QString &id)
{
    return presetWindowSize(windowPresetFromId(id));
}

QSize presetWindowSize(WindowPreset preset)
{
    int count = 0;
    const WindowPresetEntry *table = windowPresetTable(&count);
    for (int i = 0; i < count; ++i) {
        if (table[i].preset == preset)
            return table[i].size;
    }
    return QSize();
}

QByteArray savedGeometry()
{
    return settings().value(QLatin1String(kGeometry)).toByteArray();
}

void setSavedGeometry(const QByteArray &geometry)
{
    settings().setValue(QLatin1String(kGeometry), geometry);
}

bool sidebarAutoHide()
{
    return settings().value(QLatin1String(kSidebarAutoHide), true).toBool();
}

void setSidebarAutoHide(bool enabled)
{
    settings().setValue(QLatin1String(kSidebarAutoHide), enabled);
}

/// @brief 左侧播放列表用户拖出的宽度。
/// @return 常显模式的宽度，0 表示还没记录过；范围夹在 160..1200，
///         防止手工改配置把侧栏撑满整个窗口。
int sidebarWidth()
{
    const int value = settings().value(QLatin1String(kSidebarWidth), 0).toInt();
    return value <= 0 ? 0 : qBound(160, value, 1200);
}

void setSidebarWidth(int width)
{
    if (width <= 0)
        return;
    settings().setValue(QLatin1String(kSidebarWidth), qBound(160, width, 1200));
}

bool controlsAutoHide()
{
    return settings().value(QLatin1String(kControlsAutoHide), true).toBool();
}

void setControlsAutoHide(bool enabled)
{
    settings().setValue(QLatin1String(kControlsAutoHide), enabled);
}

/// @brief 读取上次退出时保存的播放音量。
/// @return 0..100；优先 playback/volume，旧版顶层 volume 作为兼容回退。
int playbackVolume()
{
    // 兼容旧版本键名 "volume"
    QSettings store;
    const int value = store.contains(QLatin1String(kVolume))
                          ? store.value(QLatin1String(kVolume)).toInt()
                          : store.value(QStringLiteral("volume"), kFallbackVolume).toInt();
    return qBound(0, value, 100);
}

void setPlaybackVolume(int percent)
{
    settings().setValue(QLatin1String(kVolume), qBound(0, percent, 100));
}

/// @brief 读取播放模式。
/// @return 0=列表循环、1=单曲循环、2=随机；越界值统一夹到合法范围。
/// @note 旧版顶层 mode 键仍可读取一次，新值写入 playback/mode。
int playbackMode()
{
    QSettings store;
    const int value = store.contains(QLatin1String(kMode))
                          ? store.value(QLatin1String(kMode)).toInt()
                          : store.value(QStringLiteral("mode"), kDefaultMode).toInt();
    // 配置可能被手工改成越界值；这里统一收口到 ListLoop/SingleLoop/Random，
    // 避免调用方各自的 static_cast 落到未定义枚举。
    return qBound(0, value, 2);
}

void setPlaybackMode(int mode)
{
    settings().setValue(QLatin1String(kMode), qBound(0, mode, 2));
}

/// @brief 上次退出时正在播放的条目类型。
/// @return 1=视频、0=音乐、-1=没有记录；只接受这三个值。
int lastPlayedItemType()
{
    const int value = settings().value(QLatin1String(kLastItemType), -1).toInt();
    return value == 0 || value == 1 ? value : -1;
}

QString lastPlayedItemIdentity()
{
    if (lastPlayedItemType() < 0)
        return QString();
    return settings().value(QLatin1String(kLastItemIdentity)).toString();
}

/// @brief 记录当前正在播放的列表条目。
/// @note 类型不合法或身份为空时直接清空，避免下次启动按残缺记录误选其它行。
void setLastPlayedItem(int type, const QString &identity)
{
    if ((type != 0 && type != 1) || identity.isEmpty()) {
        clearLastPlayedItem();
        return;
    }
    QSettings store = settings();
    store.setValue(QLatin1String(kLastItemType), type);
    store.setValue(QLatin1String(kLastItemIdentity), identity);
}

void clearLastPlayedItem()
{
    QSettings store = settings();
    store.remove(QLatin1String(kLastItemType));
    store.remove(QLatin1String(kLastItemIdentity));
}

int searchLimit()
{
    const int value = settings().value(QLatin1String(kSearchLimit), kDefaultSearchLimit).toInt();
    return qBound(1, value, 100);
}

void setSearchLimit(int limit)
{
    settings().setValue(QLatin1String(kSearchLimit), qBound(1, limit, 100));
}

/// @brief 读取最近搜索词。
/// @return 去掉空白、按原顺序去重后的最多 8 条记录。
QStringList searchHistory()
{
    const QStringList stored =
        settings().value(QLatin1String(kSearchHistory)).toStringList();
    QStringList cleaned;
    for (const QString &word : stored) {
        const QString trimmed = word.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);
    }
    return cleaned.mid(0, 8);
}

void setSearchHistory(const QStringList &history)
{
    QStringList cleaned;
    for (const QString &word : history) {
        const QString trimmed = word.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);
    }
    settings().setValue(QLatin1String(kSearchHistory), cleaned.mid(0, 8));
}

/// @brief 读取单个下载任务的体积上限（MB）。
/// @return 512..65536；下限防止误填 0 让所有任务立即失败。
int downloadLimitMb()
{
    const int value =
        settings().value(QLatin1String(kDownloadLimitMb), kDefaultDownloadLimitMb).toInt();
    // 512 MB 兜底避免用户误填 0 导致所有下载立刻失败；上限放到 64 GB。
    return qBound(512, value, 65536);
}

void setDownloadLimitMb(int limitMb)
{
    settings().setValue(QLatin1String(kDownloadLimitMb), qBound(512, limitMb, 65536));
}

/// @brief 读取默认下载根目录。
/// @return 用户配置的绝对目录；未配置或清理过时返回“下载/MusicVideoPlayer”。
/// @note 返回路径不做存在性检查，由下载管理器在创建任务时确保目录可用。
QString downloadDir()
{
    QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (fallback.isEmpty())
        fallback = QDir::homePath();
    fallback = QDir(fallback).filePath(QStringLiteral("MusicVideoPlayer"));

    const QString configured =
        settings().value(QLatin1String(kDownloadDir)).toString().trimmed();
    return configured.isEmpty() ? fallback : QDir::cleanPath(configured);
}

void setDownloadDir(const QString &dir)
{
    const QString cleaned = QDir::cleanPath(dir.trimmed());
    if (cleaned.isEmpty() || cleaned == QLatin1String("."))
        settings().remove(QLatin1String(kDownloadDir));
    else
        settings().setValue(QLatin1String(kDownloadDir), cleaned);
}

bool removePlaybackCacheAfterDownload()
{
    return settings()
        .value(QLatin1String(kRemovePlaybackCacheAfterDownload), true)
        .toBool();
}

void setRemovePlaybackCacheAfterDownload(bool enabled)
{
    settings().setValue(QLatin1String(kRemovePlaybackCacheAfterDownload), enabled);
}

QString sourceParserCommand()
{
    const QString command =
        settings()
            .value(QLatin1String(kSourceParserCommand),
                   QString::fromLatin1(kDefaultSourceParserCommand))
            .toString()
            .trimmed();
    return command.isEmpty() ? QString::fromLatin1(kDefaultSourceParserCommand) : command;
}

void setSourceParserCommand(const QString &command)
{
    settings().setValue(QLatin1String(kSourceParserCommand), command.trimmed());
}

QString sourceParserArguments()
{
    return settings()
        .value(QLatin1String(kSourceParserArguments),
               QString::fromLatin1(kDefaultSourceParserArguments))
        .toString()
        .trimmed();
}

void setSourceParserArguments(const QString &arguments)
{
    settings().setValue(QLatin1String(kSourceParserArguments), arguments.trimmed());
}

int sourceParserTimeoutMs()
{
    return qBound(5000,
                  settings()
                      .value(QLatin1String(kSourceParserTimeoutMs),
                             kDefaultSourceParserTimeoutMs)
                      .toInt(),
                  600000);
}

void setSourceParserTimeoutMs(int timeoutMs)
{
    settings().setValue(QLatin1String(kSourceParserTimeoutMs),
                        qBound(5000, timeoutMs, 600000));
}

/// @brief 读取指定快捷键动作当前保存的按键文本。
/// @return 未配置或动作 id 为空时返回空字符串；不负责回落到默认值。
QString shortcut(const QString &actionId)
{
    if (actionId.isEmpty())
        return QString();
    return settings().value(QLatin1String(kShortcutPrefix) + actionId).toString();
}

bool hasShortcut(const QString &actionId)
{
    if (actionId.isEmpty())
        return false;
    return settings().contains(QLatin1String(kShortcutPrefix) + actionId);
}

void setShortcut(const QString &actionId, const QString &sequence)
{
    if (actionId.isEmpty())
        return;
    settings().setValue(QLatin1String(kShortcutPrefix) + actionId, sequence);
}

/// @brief 读取最近一次文件选择目录。
/// @return 一个确定存在的目录，按“保存值 → 音乐目录 → 主目录 → 当前目录”
///         逐级回退，避免 QFileDialog 从随机位置打开。
QString lastDir()
{
    QString saved = settings().value(QLatin1String(kLastDir)).toString();
    // 设置里可能是空字符串（旧版本或清理过），也可能指向已删除/拔掉的
    // 目录；QFileDialog 收到这种路径会从“桌面/文档”随机位置打开，这里
    // 逐级回退到音乐目录、主目录、当前目录，保证始终给一个存在的起点。
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;
    const QStringList fallbacks = {
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
        QDir::currentPath(),
    };
    for (const QString &candidate : fallbacks) {
        if (!candidate.isEmpty() && QDir(candidate).exists())
            return candidate;
    }
    return QDir::homePath();
}

void setLastDir(const QString &dir)
{
    settings().setValue(QLatin1String(kLastDir), dir);
}

/// @brief 读取旧版 QSettings 中的播放列表 JSON。
/// @note 正式存储已迁移到 Data/playlists.json；这里只服务一次性迁移。
QByteArray playlist(MediaType type)
{
    const char *key = type == MediaType::Video ? kPlaylistVideo : kPlaylistMusic;
    return settings().value(QLatin1String(key)).toByteArray();
}

void setPlaylist(MediaType type, const QByteArray &json)
{
    const char *key = type == MediaType::Video ? kPlaylistVideo : kPlaylistMusic;
    settings().setValue(QLatin1String(key), json);
}

/// @brief 旧版播放列表是否已完成迁移。
/// @return 已迁移返回 true；迁移成功后不会再重复读取旧 QSettings。
bool playlistsMigrated()
{
    return settings().value(QStringLiteral("playlists/migrated"), false).toBool();
}

/// @brief 标记迁移完成并立即 sync，防止崩溃时状态丢失。
void setPlaylistsMigrated(bool migrated)
{
    settings().setValue(QStringLiteral("playlists/migrated"), migrated);
    settings().sync();
}

/// @brief 删除旧版 QSettings 中的播放列表键，释放重复存储。
void clearLegacyPlaylists()
{
    QSettings store = settings();
    store.remove(QLatin1String(kPlaylistMusic));
    store.remove(QLatin1String(kPlaylistVideo));
    store.sync();
}

} // namespace AppSettings
