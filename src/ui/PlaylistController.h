#ifndef PLAYLISTCONTROLLER_H
#define PLAYLISTCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QStringList>

class PlaybackController;
class PlaylistModel;
class QListView;
class QPoint;
class QTabWidget;

/// 播放列表面板依赖的控件：只给两个列表和音乐/视频页签。
struct PlaylistUi
{
    QListView *musicView = nullptr;
    QListView *videoView = nullptr;
    QTabWidget *tabs = nullptr;
};

/// 播放列表状态与持久化：模型、恢复/保存、本地文件导入、删除/清空、右键菜单。
/// 播放本身仍交给 PlaybackController；删除当前项后的进度/歌词清理通过信号交给窗口。
class PlaylistController : public QObject
{
    Q_OBJECT

public:
    PlaylistController(PlaybackController *player, const PlaylistUi &ui,
                       QObject *parent = nullptr);

    PlaylistModel *musicModel() const { return m_musicModel; }
    PlaylistModel *videoModel() const { return m_videoModel; }
    PlaylistModel *modelFor(MediaType type) const;

    PlaylistModel *activeModel() const;
    QListView *activeView() const;

    void restore();
    bool save();
    /// 用户主动保存（Ctrl+S 或右键“保存播放列表”）：没有修改时给出明确反馈
    bool saveNow();
    /// 上次保存失败后保留内存修改；这里重试写盘，成功才清除未保存标记
    bool retrySave();
    /// 主文件读不了且无法备份时为 true：此时任何保存都会覆盖原文件，必须禁止
    bool storageBlocked() const { return m_storageBlocked; }
    bool hasUnsavedChanges() const { return m_dirty; }
    void ensurePlaylist();

    /// 搜索结果试听：已存在则定位，不存在则临时加入内存模型（不立即持久化）
    void playItem(const MediaItem &item);
    /// 搜索结果加入指定列表（类型以参数为准，支持把视频加入音乐列表）
    void addSearchItem(const MediaItem &item, MediaType type);
    /// 本地文件导入：自动找同名 .lrc
    void addLocalFiles(MediaType type, const QStringList &files);
    /// 下载完成后把列表里同一来源条目更新为本地文件；找不到则忽略
    bool updateDownloadedItem(const MediaItem &source, const QString &localPath);
    /// 弹出文件选择框并导入本地音乐/视频，目录记忆走 AppSettings
    void chooseLocalFiles(MediaType type);

signals:
    void statusMessage(const QString &text);
    /// 当前播放项被删除或列表被清空：窗口需要复位进度、时间与歌词
    void playbackInvalidated();
    /// 用户在右键菜单要求把网络/试听条目下载到本地；由窗口转交 DownloadController
    void downloadRequested(const MediaItem &item);

private:
    void bindView(QListView *view, PlaylistModel *model, MediaType type);
    void removeRow(MediaType type, int row);
    void clearPlaylist(MediaType type);
    void showContextMenu(MediaType type, QListView *view, PlaylistModel *model,
                         const QPoint &pos);
    void playRow(MediaType type, int row);
    /// 把搜索结果试听行提升为永久条目并立即写回列表
    bool promotePreviewRow(MediaType type, int row);
    void setDirty(bool dirty);
    void refreshTabTitles();
    /// 删除条目时同步清理其播放缓存。正在播放的文件可能仍被播放器占用，
    /// 失败时延迟重试，避免 Windows 上文件句柄未释放导致删不干净。
    void removeCacheAfterPlayback(const MediaItem &item, int attempt = 0);

    PlaybackController *m_player = nullptr;
    PlaylistUi m_ui;
    PlaylistModel *m_musicModel = nullptr;
    PlaylistModel *m_videoModel = nullptr;
    QString m_musicTabTitle;
    QString m_videoTabTitle;
    /// 保存失败后界面仍保留修改；页签加 * 并在右键菜单提供重试
    bool m_dirty = false;
    /// 读取失败且无法备份原文件：保存被冻结，避免空模型覆盖
    bool m_storageBlocked = false;
};

#endif // PLAYLISTCONTROLLER_H
