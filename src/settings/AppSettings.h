#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include "media/MediaItem.h"
#include "settings/WindowPreset.h"

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QStringList>

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

// ---------------------------------------------------------------- 界面布局
/// 启动时是否自动收起左侧列表（收起后鼠标贴左边缘弹出）
bool sidebarAutoHide();
void setSidebarAutoHide(bool enabled);
/// 启动时是否自动收起底部控制区（收起后鼠标贴底边弹出）
bool controlsAutoHide();
void setControlsAutoHide(bool enabled);

// ---------------------------------------------------------------- 播放
int defaultVolume();               ///< 0..100
void setDefaultVolume(int percent);
int playbackMode();                ///< 对应 PlaybackController::Mode
void setPlaybackMode(int mode);

// ---------------------------------------------------------------- 搜索
int searchLimit();                 ///< 单次搜索结果条数
void setSearchLimit(int limit);
/// 搜索列表每页显示条数（用户可在列表底部分页条里改）
int searchPageSize();
void setSearchPageSize(int size);

/// 探源时先试的用户自定义接口路径（相对站点，或完整 URL；支持 {keyword}/{limit}）
QStringList probePaths(MediaType type);
void setProbePaths(MediaType type, const QStringList &paths);

// ---------------------------------------------------------------- 路径
QString lastDir();
void setLastDir(const QString &dir);

// ---------------------------------------------------------------- 播放列表
/// 播放列表以 JSON 文本持久化，空表示没有历史记录
QByteArray playlist(MediaType type);
void setPlaylist(MediaType type, const QByteArray &json);

/// 旧版把播放列表存在 QSettings；迁移到 Data/playlists.json 后只读一次并清掉旧键
bool playlistsMigrated();
void setPlaylistsMigrated(bool migrated);
void clearLegacyPlaylists();

} // namespace AppSettings

#endif // APPSETTINGS_H
