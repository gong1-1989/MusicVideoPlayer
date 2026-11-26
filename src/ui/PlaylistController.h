#ifndef PLAYLISTCONTROLLER_H
#define PLAYLISTCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QPoint>
#include <QStringList>

class PlaybackController;
class PlaylistModel;
class PlaylistTreeModel;
class QAbstractItemView;
class QEvent;
class QListView;
class QModelIndex;
class QTabWidget;
class QTreeView;

/// 播放列表面板依赖的控件：音乐仍是平铺列表，视频用树形列表展示“剧集折叠”。
struct PlaylistUi
{
    QListView *musicView = nullptr;
    QTreeView *videoView = nullptr;
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
    /// 视频页签的折叠树视图（音乐页仍是平铺列表）。
    QTreeView *videoView() const { return m_ui.videoView; }
    PlaylistModel *modelFor(MediaType type) const;

    PlaylistModel *activeModel() const;
    QAbstractItemView *activeView() const;
    /// 当前活动列表选中的真实源行；视频分组节点取第一集，未选中返回 -1。
    int activeSourceRow() const;
    /// 按源行定位并选中：视频会展开所属剧集分组，保证当前播放项可见。
    void selectSourceRow(MediaType type, int row);
    /// 视图 index 到扁平模型源行的映射；视频分组节点返回第一集行号。
    int sourceRowForIndex(MediaType type, const QModelIndex &index) const;
    /// 视频折叠树的分组父节点；用于区分“单击展开/收起”和“双击播放单集”。
    bool isGroupIndex(MediaType type, const QModelIndex &index) const;

    void restore();
    /// 退出前记录当前正在播放的列表条目，供下次启动默认选中。
    /// 只在播放的条目属于持久化播放列表时写入；搜索试听等临时条目不记录。
    void rememberPlayingItem();
    /// 启动恢复列表后调用：按上次记录的 mediaIdentity 找回条目并选中。
    /// 视频条目会展开所属剧集分组，保证选中行可见；找不到时保持原状。
    void restoreLastSelection();
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
    /// 整组加入：多集剧只写盘一次，最后汇总新增/更新/重复的集数。
    void addSearchItems(const QVector<MediaItem> &items, MediaType type);
    /// 本地文件导入：自动找同名 .lrc
    void addLocalFiles(MediaType type, const QStringList &files);
    /// 下载完成后把列表里同一来源条目更新为本地文件；找不到则忽略
    bool updateDownloadedItem(const MediaItem &source, const QString &localPath);
    /// 弹出文件选择框并导入本地音乐/视频，目录记忆走 AppSettings
    void chooseLocalFiles(MediaType type);
    /// 本地文件选择器的过滤器；独立成静态函数便于测试专用格式分组
    static QString localFileDialogFilter(MediaType type);

signals:
    void statusMessage(const QString &text);
    /// 当前播放项被删除或列表被清空：窗口需要复位进度、时间与歌词
    void playbackInvalidated();
    /// 用户在右键菜单要求把网络/试听条目下载到本地；由窗口转交 DownloadController
    void downloadRequested(const MediaItem &item);

protected:
    /// 视频树：单击剧头正文切换展开/收起；左侧箭头仍交给 QTreeView 原生处理。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void bindView(QAbstractItemView *view, PlaylistModel *model, MediaType type);
    void removeIndex(MediaType type, const QModelIndex &index);
    void removeCollection(MediaType type, const QString &key);
    void removeRow(MediaType type, int row);
    void clearPlaylist(MediaType type);
    void showContextMenu(MediaType type, QAbstractItemView *view, PlaylistModel *model,
                         const QPoint &pos);
    void playRow(MediaType type, int row);
    /// 把搜索结果试听行提升为永久条目并立即写回列表
    bool promotePreviewRow(MediaType type, int row);
    /// 进度写回是高频事件：合并到 300ms 后一次落盘，退出时由窗口再同步 flush。
    void scheduleSave();
    void setDirty(bool dirty);
    void refreshTabTitles();
    /// 删除条目时同步清理其播放缓存。正在播放的文件可能仍被播放器占用，
    /// 失败时延迟重试，避免 Windows 上文件句柄未释放导致删不干净。
    void removeCacheAfterPlayback(const MediaItem &item, int attempt = 0);
    /// 点击位置是否落在 QTreeView 左侧展开箭头上；落在箭头上时不重复切换。
    bool pointOnDisclosure(const QPoint &pos, const QModelIndex &index) const;

    PlaybackController *m_player = nullptr;
    PlaylistUi m_ui;
    PlaylistModel *m_musicModel = nullptr;
    PlaylistModel *m_videoModel = nullptr;
    /// 视频页签的折叠树：只做展示，真实数据/播放行号仍以 m_videoModel 为准。
    PlaylistTreeModel *m_videoTreeModel = nullptr;
    QString m_musicTabTitle;
    QString m_videoTabTitle;
    /// 保存失败后界面仍保留修改；页签加 * 并在右键菜单提供重试
    bool m_dirty = false;
    /// 读取失败且无法备份原文件：保存被冻结，避免空模型覆盖
    bool m_storageBlocked = false;
    /// 递增后让更早排队的防抖保存失效，避免一次操作重复写盘。
    int m_saveGeneration = 0;
    /// 单击剧头的按下状态：只有同一分组、未拖动、且不是点在左侧箭头上才切换。
    QString m_pressedGroupKey;
    QPoint m_pressedGroupPos;
    bool m_pressedOnDisclosure = false;
    /// 双击时第一次单击已经切换过；忽略双击产生的第二次释放，避免来回抵消。
    bool m_skipGroupRelease = false;
};

#endif // PLAYLISTCONTROLLER_H
