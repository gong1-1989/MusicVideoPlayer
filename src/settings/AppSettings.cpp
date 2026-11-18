#include "settings/AppSettings.h"

#include <QSettings>
#include <QStandardPaths>

namespace {

const char kTheme[] = "ui/theme";
const char kWindowPreset[] = "ui/windowPreset";
const char kGeometry[] = "ui/geometry";
const char kVolume[] = "playback/volume";
const char kMode[] = "playback/mode";
const char kSearchLimit[] = "search/limit";
const char kLastDir[] = "paths/lastDir";
const char kPlaylistMusic[] = "playlists/music";
const char kPlaylistVideo[] = "playlists/video";

const char kDefaultTheme[] = "dark";
const char kDefaultWindowPreset[] = "standard";
const int kDefaultVolume = 60;
const int kDefaultSearchLimit = 20;
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
    return QSize(1360, 840);
}

QByteArray savedGeometry()
{
    return settings().value(QLatin1String(kGeometry)).toByteArray();
}

void setSavedGeometry(const QByteArray &geometry)
{
    settings().setValue(QLatin1String(kGeometry), geometry);
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
    return value;
}

void setPlaybackMode(int mode)
{
    settings().setValue(QLatin1String(kMode), mode);
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

} // namespace AppSettings
