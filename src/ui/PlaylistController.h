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
    void ensurePlaylist();

    /// 搜索结果直接播放：已存在则定位，不存在则先加入并保存
    void playItem(const MediaItem &item);
    /// 搜索结果加入指定列表（类型以参数为准，支持把视频加入音乐列表）
    void addSearchItem(const MediaItem &item, MediaType type);
    /// 本地文件导入：自动找同名 .lrc
    void addLocalFiles(MediaType type, const QStringList &files);
    /// 弹出文件选择框并导入本地音乐/视频，目录记忆走 AppSettings
    void chooseLocalFiles(MediaType type);

signals:
    void statusMessage(const QString &text);
    /// 当前播放项被删除或列表被清空：窗口需要复位进度、时间与歌词
    void playbackInvalidated();

private:
    void bindView(QListView *view, PlaylistModel *model, MediaType type);
    void removeRow(MediaType type, int row);
    void clearPlaylist(MediaType type);
    void showContextMenu(MediaType type, QListView *view, PlaylistModel *model,
                         const QPoint &pos);
    void playRow(MediaType type, int row);

    PlaybackController *m_player = nullptr;
    PlaylistUi m_ui;
    PlaylistModel *m_musicModel = nullptr;
    PlaylistModel *m_videoModel = nullptr;
};

#endif // PLAYLISTCONTROLLER_H
