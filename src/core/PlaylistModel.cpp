#include "core/PlaylistModel.h"

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();

    const MediaItem &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return item.displayName();
    case Qt::ToolTipRole:
        return item.online ? item.url.toString() : item.url.toLocalFile();
    case TitleRole:
        return item.title;
    case ArtistRole:
        return item.artist;
    case UrlRole:
        return item.url;
    case OnlineRole:
        return item.online;
    case DurationRole:
        return item.durationMs;
    default:
        return QVariant();
    }
}

bool PlaylistModel::addItem(const MediaItem &item)
{
    if (!item.isValid() || indexOf(item.url) >= 0)
        return false;

    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(item);
    endInsertRows();
    return true;
}

bool PlaylistModel::removeRow(int row)
{
    if (row < 0 || row >= m_items.size())
        return false;

    beginRemoveRows(QModelIndex(), row, row);
    m_items.remove(row);
    endRemoveRows();
    return true;
}

void PlaylistModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
}

void PlaylistModel::setItems(const QVector<MediaItem> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

MediaItem PlaylistModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return MediaItem();
    return m_items.at(row);
}

int PlaylistModel::indexOf(const QUrl &url) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).url == url)
            return i;
    }
    return -1;
}
