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
        DurationRole
    };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    /// 按 url 去重后追加；重复返回 false。
    bool addItem(const MediaItem &item);
    bool removeRow(int row);
    void clear();

    /// 整体替换（恢复上次会话的列表用），会重置模型
    void setItems(const QVector<MediaItem> &items);
    QVector<MediaItem> items() const { return m_items; }

    MediaItem itemAt(int row) const;
    int indexOf(const QUrl &url) const;
    int size() const { return m_items.size(); }
    bool isEmpty() const { return m_items.isEmpty(); }

private:
    QVector<MediaItem> m_items;
};

#endif // PLAYLISTMODEL_H
