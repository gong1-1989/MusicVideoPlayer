#ifndef PLAYLISTMODEL_H
#define PLAYLISTMODEL_H

#include "media/MediaItem.h"

#include <QAbstractListModel>
#include <QVector>

/// 播放列表数据模型：唯一数据源，界面通过 QListView 直接绑定，
/// 不再维护"QList + QListWidget"两份平行数据。
class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        UrlRole,
        OnlineRole,
        DurationRole,
        PosterRole,
        YearRole,
        AreaRole,
        GenreRole,
        DescriptionRole,
        MetaRole,
        /// 搜索结果分栏用：类型（MediaType）与来源（MediaItem::sourceId）
        TypeRole,
        SourceRole,
        /// 清晰度/音质（4K、1080P、无损等）
        QualityRole
    };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    /// 按 mediaIdentity 去重后追加；重复返回 false。
    bool addItem(const MediaItem &item);
    /// 同 identity 条目的字段刷新：更新直链/请求头/清晰度等可变字段，
    /// 保留用户视频进度；试听条目遇到永久条目时仍保持永久。
    /// 返回是否真的发生了字段变化。
    bool refreshItem(int row, const MediaItem &item);
    bool removeRow(int row);
    void clear();

    /// 整体替换（恢复上次会话的列表用），会重置模型并顺手去掉重复条目
    void setItems(const QVector<MediaItem> &items);
    QVector<MediaItem> items() const { return m_items; }

    MediaItem itemAt(int row) const;
    /// 就地更新一行（例如保存视频播放进度），身份和行号不变，界面收到 dataChanged。
    bool updateItem(int row, const MediaItem &item);
    /// 只更新视频播放进度。播放进度是可变的运行时状态，不能混进
    /// 搜索结果刷新逻辑，否则刷新直链时会把用户看到一半的位置覆盖掉。
    bool updatePlaybackPosition(int row, qint64 positionMs, qint64 durationMs);
    int indexOf(const MediaItem &item) const;
    int size() const { return m_items.size(); }
    bool isEmpty() const { return m_items.isEmpty(); }

private:
    QVector<MediaItem> m_items;
};

#endif // PLAYLISTMODEL_H
