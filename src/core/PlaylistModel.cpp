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
    case Qt::DisplayRole: {
        // 搜索结果里音乐和视频混在一起，加前缀便于区分，避免误入另一类列表
        QString text = item.displayName();
        const QString meta = item.metaLine();
        if (!meta.isEmpty())
            text += QStringLiteral("  ·  ") + meta;
        if (item.online && item.type == MediaType::Video)
            return QStringLiteral("▶ ") + text;
        return text;
    }
    case Qt::ToolTipRole:
        return item.detailText();
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
    case PosterRole:
        return item.posterUrl;
    case YearRole:
        return item.year;
    case AreaRole:
        return item.area;
    case GenreRole:
        return item.genre;
    case DescriptionRole:
        return item.description;
    case MetaRole:
        return item.metaLine();
    case TypeRole:
        return static_cast<int>(item.type);
    case SourceRole:
        return item.sourceId;
    default:
        return QVariant();
    }
}

bool PlaylistModel::addItem(const MediaItem &item)
{
    // 同一条内容只保留一条：直链地址可能每次搜索都变（B站签名），
    // 所以按 mediaIdentity 判重，而不是直接比 url
    if (!item.isValid() || indexOf(item) >= 0)
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
    // 旧版本存下来的列表里可能已经有重复条目，恢复时一并清理
    m_items = dedupeMediaItems(items);
    endResetModel();
}

MediaItem PlaylistModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return MediaItem();
    return m_items.at(row);
}

int PlaylistModel::indexOf(const MediaItem &item) const
{
    const QString identity = mediaIdentity(item);
    for (int i = 0; i < m_items.size(); ++i) {
        if (mediaIdentity(m_items.at(i)) == identity)
            return i;
    }
    return -1;
}
