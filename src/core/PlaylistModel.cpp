/// @file PlaylistModel.cpp
/// @brief 播放列表模型的行数据、判重与局部刷新实现。
#include "core/PlaylistModel.h"

/// @brief 构造空列表模型。
PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

/// @brief 返回顶层行数；有效父索引下没有子行。
int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

/// @brief 按角色返回行数据。
/// @note DisplayRole 统一加 “[本地]/[网络]/[临时]” 前缀；网络视频再加播放符号。
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
        // 搜索临时条目只存在于本次运行，必须显式标出；[临时] 不等于内容为试听，
        // 内容范围由 metaLine 的“试听/电台”标签表达。
        if (item.previewOnly)
            text = QStringLiteral("[临时] ") + text;
        else
            text = (item.online ? QStringLiteral("[网络] ") : QStringLiteral("[本地] ")) + text;
        if (item.online && item.type == MediaType::Video)
            return QStringLiteral("▶ ") + text;
        return text;
    }
    case Qt::ToolTipRole:
        if (item.previewOnly)
            return item.detailText()
                + QStringLiteral("\n临时条目：本次运行有效，不会写入播放列表；右键可“保存到播放列表”。");
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
    case GroupRole:
        return item.searchSectionLabel;
    default:
        return QVariant();
    }
}

/// @brief 追加一条有效且未重复的条目。
/// @return 真正插入返回 true；无效或已存在返回 false。
/// @note 判重按 mediaIdentity，而不是 URL：签名直链每次搜索都会变。
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

/// @brief 用新搜索结果刷新同一行，但保留用户本地状态与播放进度。
/// @return 字段确有变化返回 true；identity 不同或无变化返回 false。
/// @note 已下载条目不会被改回在线；positionMs 永远保留。
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
    // 但已经下载/导入成“本地文件”的条目不能被后续搜索结果改回在线：否则
    // 用户点同一首歌会重新走网络，已下载的文件也随之被遗忘。
    const bool existingLocal = !merged.online;
    takeString(merged.title, item.title);
    takeString(merged.artist, item.artist);
    if (!existingLocal) {
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
        // 只允许在线条目转成本地条目；本地条目永远不因搜索结果变回在线。
        if (merged.online && !item.online) {
            merged.online = false;
            changed = true;
        }
        // 内容范围随搜索结果更新：同一身份从试听/电台变成完整曲目时，
        // 必须清掉旧标签，否则列表会一直显示“试听”。已下载的本地文件
        // 不参与这一步，避免被在线试听结果重新标成片段。
        if (merged.streamScope != item.streamScope) {
            merged.streamScope = item.streamScope;
            changed = true;
        }
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
    // 分组元数据也随搜索刷新。旧列表里已经平铺保存的同剧条目会因此重新
    // 拿到同一 collectionKey，树模型即可把它们折叠成一部剧，不必让用户
    // 删掉重加。只有非空值才覆盖，避免单条刷新把已有分组拆散。
    if (!item.collectionKey.isEmpty()) {
        if (merged.collectionKey != item.collectionKey) {
            merged.collectionKey = item.collectionKey;
            changed = true;
        }
        if (!item.collectionTitle.isEmpty()
            && merged.collectionTitle != item.collectionTitle) {
            merged.collectionTitle = item.collectionTitle;
            changed = true;
        }
        if (item.collectionSize > 0 && merged.collectionSize != item.collectionSize) {
            merged.collectionSize = item.collectionSize;
            changed = true;
        }
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

/// @brief 更新视频条目的播放进度与时长。
/// @return 确有变化返回 true；非视频行/越界返回 false。
/// @note 进度是本地状态，不参与排序和模型重置。
bool PlaylistModel::updatePlaybackPosition(int row, qint64 positionMs, qint64 durationMs)
{
    if (row < 0 || row >= m_items.size())
        return false;
    MediaItem &item = m_items[row];
    if (item.type != MediaType::Video)
        return false;

    bool changed = false;
    if (durationMs > 0 && item.durationMs != durationMs) {
        item.durationMs = durationMs;
        changed = true;
    }
    const qint64 bounded = qMax<qint64>(0, positionMs);
    if (item.positionMs != bounded) {
        item.positionMs = bounded;
        changed = true;
    }
    if (!changed)
        return false;

    const QModelIndex indexAtRow = index(row, 0);
    emit dataChanged(indexAtRow, indexAtRow);
    return true;
}

/// @brief 删除指定行。
bool PlaylistModel::removeRow(int row)
{
    if (row < 0 || row >= m_items.size())
        return false;

    beginRemoveRows(QModelIndex(), row, row);
    m_items.remove(row);
    endRemoveRows();
    return true;
}

/// @brief 重置模型并清空全部行。
void PlaylistModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
}

/// @brief 整表载入并去重（旧版本落盘数据可能已有重复条目）。
void PlaylistModel::setItems(const QVector<MediaItem> &items)
{
    beginResetModel();
    // 旧版本存下来的列表里可能已经有重复条目，恢复时一并清理
    m_items = dedupeMediaItems(items);
    endResetModel();
}

/// @brief 返回指定行的条目副本；越界返回空条目。
MediaItem PlaylistModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return MediaItem();
    return m_items.at(row);
}

/// @brief 用同 identity 的新条目替换整行。
/// @return 更新成功返回 true；越界/无效/与其它行重复返回 false。
/// @note 只改行数据，不重排、不重置模型，选中状态不受影响。
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

/// @brief 按 mediaIdentity 查找行号。
/// @return 命中行号；不存在返回 -1。
int PlaylistModel::indexOf(const MediaItem &item) const
{
    const QString identity = mediaIdentity(item);
    for (int i = 0; i < m_items.size(); ++i) {
        if (mediaIdentity(m_items.at(i)) == identity)
            return i;
    }
    return -1;
}
