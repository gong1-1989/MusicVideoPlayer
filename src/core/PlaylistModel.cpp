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
        // 来源放在最前面，长标题被省略时也不会把“本地/网络”截掉。
        text = (item.online ? QStringLiteral("[网络] ") : QStringLiteral("[本地] ")) + text;
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
    case QualityRole:
        return item.quality;
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

bool PlaylistModel::refreshItem(int row, const MediaItem &item)
{
    if (row < 0 || row >= m_items.size() || !item.isValid())
        return false;
    if (mediaIdentity(m_items.at(row)) != mediaIdentity(item))
        return false;

    MediaItem merged = m_items.at(row);
    bool changed = false;
    const auto takeString = [&](QString &target, const QString &value) {
        if (!value.isEmpty() && target != value) {
            target = value;
            changed = true;
        }
    };
    const auto takeUrl = [&](QUrl &target, const QUrl &value) {
        if (!value.isEmpty() && value.isValid() && target != value) {
            target = value;
            changed = true;
        }
    };

    // 标题、来源等展示字段有值就刷新；直链签名每次搜索都会变，必须替换。
    takeString(merged.title, item.title);
    takeString(merged.artist, item.artist);
    if (merged.url != item.url) {
        merged.url = item.url;
        changed = true;
    }
    if (merged.alternateUrls != item.alternateUrls) {
        merged.alternateUrls = item.alternateUrls;
        changed = true;
    }
    if (!item.headers.isEmpty() && merged.headers != item.headers) {
        merged.headers = item.headers;
        changed = true;
    }
    takeUrl(merged.lyricUrl, item.lyricUrl);
    takeUrl(merged.posterUrl, item.posterUrl);
    takeUrl(merged.webUrl, item.webUrl);
    takeUrl(merged.sourceOrigin, item.sourceOrigin);
    takeString(merged.sourceId, item.sourceId);
    takeString(merged.year, item.year);
    takeString(merged.area, item.area);
    takeString(merged.genre, item.genre);
    takeString(merged.description, item.description);
    takeString(merged.quality, item.quality);
    if (merged.online != item.online) {
        merged.online = item.online;
        changed = true;
    }
    if (item.durationMs > 0 && merged.durationMs != item.durationMs) {
        merged.durationMs = item.durationMs;
        changed = true;
    }

    // positionMs 不从新搜索结果覆盖，用户看到一半的视频仍从原位置继续。
    // 永久条目优先：试听刷新不能把已加入列表的条目重新变成临时条目。
    const bool nextPreviewOnly = merged.previewOnly && item.previewOnly;
    if (merged.previewOnly != nextPreviewOnly) {
        merged.previewOnly = nextPreviewOnly;
        changed = true;
    }

    if (!changed)
        return false;
    m_items[row] = merged;
    const QModelIndex changedIndex = index(row, 0);
    emit dataChanged(changedIndex, changedIndex);
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

bool PlaylistModel::updateItem(int row, const MediaItem &item)
{
    if (row < 0 || row >= m_items.size() || !item.isValid())
        return false;
    // 进度更新只是替换字段；如果新条目 identity 已存在于另一行，拒绝更新，
    // 避免这个接口被用来把同一内容复制成两行。
    const QString identity = mediaIdentity(item);
    for (int i = 0; i < m_items.size(); ++i) {
        if (i != row && mediaIdentity(m_items.at(i)) == identity)
            return false;
    }
    // 播放进度只改当前行数据，不重排、不重置模型，列表选中状态不受影响。
    m_items[row] = item;
    const QModelIndex changed = index(row, 0);
    emit dataChanged(changed, changed);
    return true;
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
