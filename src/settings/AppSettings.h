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
/// 界面基础字号（px），默认 13，范围 9..20；ThemeManager 会把 QSS 里的
/// 11/12/13px 按相对差值同步调整，音乐动感视图也直接读取它。
int uiFontSizePx();
void setUiFontSizePx(int pixels);

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
/// 上次退出时保存的播放音量，0..100；设置页不再单独提供默认音量入口。
int playbackVolume();
void setPlaybackVolume(int percent);
int playbackMode();                ///< 对应 PlaybackController::Mode
void setPlaybackMode(int mode);

// ---------------------------------------------------------------- 搜索
int searchLimit();                 ///< 单次搜索结果条数
void setSearchLimit(int limit);
/// 最近搜索词（最多 8 个，去重），用于空输入时展示历史
QStringList searchHistory();
void setSearchHistory(const QStringList &history);

/// 新增/更新搜索源时调用的外部解析器。命令与参数支持占位符
/// {site} {type} {mode}；解析器输出 JSON（成功给 source，失败给 error）。
QString sourceParserCommand();
void setSourceParserCommand(const QString &command);
QString sourceParserArguments();
void setSourceParserArguments(const QString &arguments);
int sourceParserTimeoutMs();
void setSourceParserTimeoutMs(int timeoutMs);

// ---------------------------------------------------------------- 下载
/// 单个下载任务允许落盘的最大体积（MB）。默认 8 GB，设置页可选 2/4/8/16 GB。
int downloadLimitMb();
void setDownloadLimitMb(int limitMb);

/// 默认下载根目录。默认是“下载/MusicVideoPlayer”；音乐、影视会分别落到
/// 根目录下的“音乐”“影视”子目录，设置页可以改成其他根目录。
QString downloadDir();
void setDownloadDir(const QString &dir);

/// 下载完成后是否删除该条目的播放缓存与歌词缓存。默认开启。
/// 歌词缓存与播放缓存一起清理，下载文件本身不受影响。
bool removePlaybackCacheAfterDownload();
void setRemovePlaybackCacheAfterDownload(bool enabled);

// ---------------------------------------------------------------- 快捷键
/// 快捷键按动作 id 保存。动作清单与默认值由 AppShortcuts 统一维护，
/// AppSettings 只负责键值落盘，避免界面层直接接触 QSettings。
QString shortcut(const QString &actionId);
bool hasShortcut(const QString &actionId);
void setShortcut(const QString &actionId, const QString &sequence);

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
