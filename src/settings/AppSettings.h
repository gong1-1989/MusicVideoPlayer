#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include "media/MediaItem.h"
#include "settings/WindowPreset.h"

#include <QByteArray>
#include <QSize>
#include <QString>

/// 应用设置的唯一入口：所有可配置项的键名、默认值、取值范围都在这里，
/// 界面层不再直接拼 QSettings 的键名，也不再散落魔法数字。
namespace AppSettings {

// ---------------------------------------------------------------- 外观
QString themeId();                 ///< "dark" / "light"
void setThemeId(const QString &id);

// ---------------------------------------------------------------- 窗口
QString windowPresetId();          ///< WindowPreset 的 id，含 "last"
void setWindowPresetId(const QString &id);
WindowPreset windowPreset();
QSize presetWindowSize(const QString &id);
QSize presetWindowSize(WindowPreset preset);
QByteArray savedGeometry();
void setSavedGeometry(const QByteArray &geometry);

// ---------------------------------------------------------------- 播放
int defaultVolume();               ///< 0..100
void setDefaultVolume(int percent);
int playbackMode();                ///< 对应 PlaybackController::Mode
void setPlaybackMode(int mode);

// ---------------------------------------------------------------- 搜索
int searchLimit();                 ///< 单次搜索结果条数
void setSearchLimit(int limit);

// ---------------------------------------------------------------- 路径
QString lastDir();
void setLastDir(const QString &dir);

// ---------------------------------------------------------------- 播放列表
/// 播放列表以 JSON 文本持久化，空表示没有历史记录
QByteArray playlist(MediaType type);
void setPlaylist(MediaType type, const QByteArray &json);

} // namespace AppSettings

#endif // APPSETTINGS_H
