#include "settings/AppSettings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace {

const char kTheme[] = "ui/theme";
const char kFontSizePx[] = "ui/fontSizePx";
const char kLegacyFontPercent[] = "ui/fontPercent";
const char kWindowPreset[] = "ui/windowPreset";
const char kGeometry[] = "ui/geometry";
const char kVolume[] = "playback/volume";
const char kMode[] = "playback/mode";
const char kSearchLimit[] = "search/limit";
const char kSearchHistory[] = "search/history";
const char kDownloadLimitMb[] = "download/limitMb";
const char kDownloadDir[] = "download/dir";
const char kProbePathsVideo[] = "search/probePaths/video";
const char kProbePathsMusic[] = "search/probePaths/music";
const char kLastDir[] = "paths/lastDir";
const char kPlaylistMusic[] = "playlists/music";
const char kPlaylistVideo[] = "playlists/video";
const char kSidebarAutoHide[] = "ui/sidebarAutoHide";
const char kControlsAutoHide[] = "ui/controlsAutoHide";
const char kShortcutPrefix[] = "shortcuts/";

const char kDefaultTheme[] = "dark";
const char kDefaultWindowPreset[] = "auto";
const int kDefaultFontSizePx = 13;
const int kMinFontSizePx = 9;
const int kMaxFontSizePx = 20;
const int kDefaultVolume = 60;
const int kDefaultSearchLimit = 20;
const int kDefaultDownloadLimitMb = 8192;
const int kDefaultMode = 0;

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

bool controlsAutoHide()
{
    return settings().value(QLatin1String(kControlsAutoHide), true).toBool();
}

void setControlsAutoHide(bool enabled)
{
    settings().setValue(QLatin1String(kControlsAutoHide), enabled);
}

int defaultVolume()
{
    // 兼容旧版本键名 "volume"
    QSettings store;
    const int value = store.contains(QLatin1String(kVolume))
                          ? store.value(QLatin1String(kVolume)).toInt()
                          : store.value(QStringLiteral("volume"), kDefaultVolume).toInt();
    return qBound(0, value, 100);
}

void setDefaultVolume(int percent)
{
    settings().setValue(QLatin1String(kVolume), qBound(0, percent, 100));
}

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

int searchLimit()
{
    const int value = settings().value(QLatin1String(kSearchLimit), kDefaultSearchLimit).toInt();
    return qBound(1, value, 100);
}

void setSearchLimit(int limit)
{
    settings().setValue(QLatin1String(kSearchLimit), qBound(1, limit, 100));
}

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

QStringList probePaths(MediaType type)
{
    const char *key = type == MediaType::Video ? kProbePathsVideo : kProbePathsMusic;
    QStringList paths = settings().value(QLatin1String(key)).toStringList();
    QStringList cleaned;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);
    }
    return cleaned;
}

void setProbePaths(MediaType type, const QStringList &paths)
{
    const char *key = type == MediaType::Video ? kProbePathsVideo : kProbePathsMusic;
    QStringList cleaned;
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed))
            cleaned.append(trimmed);
    }
    settings().setValue(QLatin1String(key), cleaned);
}

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

void removeShortcut(const QString &actionId)
{
    if (actionId.isEmpty())
        return;
    settings().remove(QLatin1String(kShortcutPrefix) + actionId);
}

QString lastDir()
{
    return settings()
        .value(QLatin1String(kLastDir),
               QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
        .toString();
}

void setLastDir(const QString &dir)
{
    settings().setValue(QLatin1String(kLastDir), dir);
}

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

bool playlistsMigrated()
{
    return settings().value(QStringLiteral("playlists/migrated"), false).toBool();
}

void setPlaylistsMigrated(bool migrated)
{
    settings().setValue(QStringLiteral("playlists/migrated"), migrated);
    settings().sync();
}

void clearLegacyPlaylists()
{
    QSettings store = settings();
    store.remove(QLatin1String(kPlaylistMusic));
    store.remove(QLatin1String(kPlaylistVideo));
    store.sync();
}

} // namespace AppSettings
