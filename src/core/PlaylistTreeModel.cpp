/// @file PlaylistTreeModel.cpp
/// @brief 视频播放列表的剧集折叠树实现。
#include "core/PlaylistTreeModel.h"

#include "core/PlaylistModel.h"

#include <QFont>
#include <QRegularExpression>
#include <QStandardItem>

#include <utility>

namespace {

/// 剧名后面常带“ · 第 3 集”或“ 第 3 集”；折叠父行只保留基础剧名。
QString stripEpisodeSuffix(const QString &title)
{
    static const QRegularExpression suffix(
        QStringLiteral("\\s*(?:[·:：\\-]\\s*)?第\\s*\\d+\\s*[集话期].*$"));
    QString base = title;
    base.remove(suffix);
    return base.trimmed();
}

QString stripCollectionCount(const QString &title)
{
    static const QRegularExpression count(
        QStringLiteral("\\s*[（(]\\s*共\\s*\\d+\\s*集\\s*[）)]\\s*$"));
    QString base = title;
    base.remove(count);
    return base.trimmed();
}

} // namespace

PlaylistTreeModel::PlaylistTreeModel(PlaylistModel *source, QObject *parent)
    : QStandardItemModel(parent)
    , m_source(source)
{
    setColumnCount(1);
    if (m_source == nullptr)
        return;

    connect(m_source, &QAbstractItemModel::rowsInserted, this, &PlaylistTreeModel::rebuild);
    connect(m_source, &QAbstractItemModel::rowsRemoved, this, &PlaylistTreeModel::rebuild);
    connect(m_source, &QAbstractItemModel::modelReset, this, &PlaylistTreeModel::rebuild);
    connect(m_source, &QAbstractItemModel::dataChanged, this,
            &PlaylistTreeModel::onSourceDataChanged);
    rebuild();
}

int PlaylistTreeModel::sourceRow(const QModelIndex &index) const
{
    if (!index.isValid())
        return -1;
    return index.data(SourceRowRole).toInt();
}

bool PlaylistTreeModel::isGroup(const QModelIndex &index) const
{
    return index.isValid() && index.data(IsGroupRole).toBool();
}

QString PlaylistTreeModel::groupKey(const QModelIndex &index) const
{
    return index.isValid() ? index.data(GroupKeyRole).toString() : QString();
}

int PlaylistTreeModel::groupSize(const QModelIndex &index) const
{
    return index.isValid() ? index.data(GroupSizeRole).toInt() : 0;
}

QModelIndex PlaylistTreeModel::indexForSourceRow(int row) const
{
    if (row < 0)
        return QModelIndex();

    for (int top = 0; top < rowCount(); ++top) {
        const QModelIndex indexAt = index(top, 0);
        if (indexAt.data(SourceRowRole).toInt() == row && !indexAt.data(IsGroupRole).toBool())
            return indexAt;
        if (!indexAt.data(IsGroupRole).toBool())
            continue;

        const QStandardItem *groupItem = itemFromIndex(indexAt);
        if (groupItem == nullptr)
            continue;
        for (int child = 0; child < groupItem->rowCount(); ++child) {
            const QModelIndex childIndex = index(child, 0, indexAt);
            if (childIndex.data(SourceRowRole).toInt() == row)
                return childIndex;
        }
    }
    return QModelIndex();
}

QModelIndex PlaylistTreeModel::indexForGroup(const QString &key) const
{
    if (key.isEmpty())
        return QModelIndex();
    for (int top = 0; top < rowCount(); ++top) {
        const QModelIndex indexAt = index(top, 0);
        if (indexAt.data(IsGroupRole).toBool() && indexAt.data(GroupKeyRole).toString() == key)
            return indexAt;
    }
    return QModelIndex();
}

QVector<int> PlaylistTreeModel::sourceRowsForGroup(const QString &key) const
{
    QVector<int> rows;
    const QModelIndex groupIndex = indexForGroup(key);
    if (!groupIndex.isValid())
        return rows;

    const QStandardItem *groupItem = itemFromIndex(groupIndex);
    if (groupItem == nullptr)
        return rows;
    rows.reserve(groupItem->rowCount());
    for (int child = 0; child < groupItem->rowCount(); ++child) {
        const QModelIndex childIndex = index(child, 0, groupIndex);
        const int row = childIndex.data(SourceRowRole).toInt();
        if (row >= 0)
            rows.append(row);
    }
    return rows;
}

void PlaylistTreeModel::setGroupExpanded(const QString &key, bool expanded)
{
    if (key.isEmpty())
        return;
    if (expanded)
        m_expandedKeys.insert(key);
    else
        m_expandedKeys.remove(key);
}

bool PlaylistTreeModel::isGroupExpanded(const QString &key) const
{
    return m_expandedKeys.contains(key);
}

QStringList PlaylistTreeModel::expandedGroupKeys() const
{
    QStringList result;
    result.reserve(m_expandedKeys.size());
    for (const QString &key : m_expandedKeys)
        result.append(key);
    return result;
}

void PlaylistTreeModel::rebuild()
{
    clear();
    setColumnCount(1);
    m_sourceDisplay.clear();
    if (m_source == nullptr) {
        emit structureRebuilt();
        return;
    }

    QHash<QString, int> countByKey;
    for (int row = 0; row < m_source->size(); ++row) {
        const MediaItem item = m_source->itemAt(row);
        if (item.type == MediaType::Video && !item.collectionKey.isEmpty())
            ++countByKey[item.collectionKey];
    }

    QSet<QString> builtGroups;
    for (int row = 0; row < m_source->size(); ++row) {
        const MediaItem sourceItem = m_source->itemAt(row);
        const QString key = sourceItem.type == MediaType::Video ? sourceItem.collectionKey
                                                                : QString();
        if (key.isEmpty() || countByKey.value(key) < 2) {
            QString leafText =
                m_source->data(m_source->index(row, 0), Qt::DisplayRole).toString();
            // 播放列表原来是 QListView，用文本 “▶” 冒充视频标记；换成真正的
            // 树以后左侧已有展开箭头，再保留 “▶” 会出现两个三角。
            if (sourceItem.type == MediaType::Video
                && leafText.startsWith(QStringLiteral("▶ "))) {
                leafText.remove(0, 2);
            }
            QStandardItem *leaf = new QStandardItem(leafText);
            leaf->setEditable(false);
            leaf->setData(row, SourceRowRole);
            leaf->setData(false, IsGroupRole);
            leaf->setData(sourceItem.type == MediaType::Video ? sourceItem.collectionKey
                                                              : QString(),
                          GroupKeyRole);
            leaf->setData(1, GroupSizeRole);
            leaf->setToolTip(m_source->data(m_source->index(row, 0), Qt::ToolTipRole).toString());
            invisibleRootItem()->appendRow(leaf);
            m_sourceDisplay.insert(
                row, m_source->data(m_source->index(row, 0), Qt::DisplayRole).toString()
                         + sourceItem.collectionKey + sourceItem.collectionTitle
                         + QString::number(sourceItem.collectionSize));
            continue;
        }

        if (builtGroups.contains(key))
            continue;
        builtGroups.insert(key);

        QVector<int> rows;
        for (int candidate = 0; candidate < m_source->size(); ++candidate) {
            const MediaItem item = m_source->itemAt(candidate);
            if (item.type == MediaType::Video && item.collectionKey == key)
                rows.append(candidate);
        }

        QStandardItem *groupItem = new QStandardItem(groupTitle(rows));
        groupItem->setEditable(false);
        groupItem->setData(rows.first(), SourceRowRole);
        groupItem->setData(true, IsGroupRole);
        groupItem->setData(key, GroupKeyRole);
        groupItem->setData(rows.size(), GroupSizeRole);
        QFont groupFont = groupItem->font();
        groupFont.setBold(true);
        groupItem->setFont(groupFont);
        groupItem->setToolTip(tr("%1\n来源：%2；点击左侧箭头展开/收起，双击播放第 1 集。")
                                  .arg(groupTitle(rows),
                                       rows.first() >= 0 && m_source->itemAt(rows.first()).online
                                           ? tr("网络")
                                           : tr("本地")));
        invisibleRootItem()->appendRow(groupItem);

        for (int sourceRow : std::as_const(rows)) {
            const MediaItem item = m_source->itemAt(sourceRow);
            QStandardItem *child = new QStandardItem(episodeTitle(sourceRow));
            child->setEditable(false);
            child->setData(sourceRow, SourceRowRole);
            child->setData(false, IsGroupRole);
            child->setData(key, GroupKeyRole);
            child->setData(rows.size(), GroupSizeRole);
            child->setToolTip(
                m_source->data(m_source->index(sourceRow, 0), Qt::ToolTipRole).toString());
            groupItem->appendRow(child);
            m_sourceDisplay.insert(
                sourceRow, m_source->data(m_source->index(sourceRow, 0), Qt::DisplayRole).toString()
                               + item.collectionKey + item.collectionTitle
                               + QString::number(item.collectionSize));
        }
    }
    emit structureRebuilt();
}

void PlaylistTreeModel::onSourceDataChanged(const QModelIndex &topLeft,
                                            const QModelIndex &bottomRight)
{
    if (m_source == nullptr || !topLeft.isValid())
        return;
    const int last = qMin(bottomRight.row(), m_source->size() - 1);
    for (int row = topLeft.row(); row <= last; ++row) {
        const MediaItem item = m_source->itemAt(row);
        const QString fingerprint =
            m_source->data(m_source->index(row, 0), Qt::DisplayRole).toString()
            + item.collectionKey + item.collectionTitle + QString::number(item.collectionSize);
        if (m_sourceDisplay.value(row) != fingerprint) {
            rebuild();
            return;
        }
    }
}

QString PlaylistTreeModel::groupTitle(const QVector<int> &rows) const
{
    if (rows.isEmpty())
        return QString();
    const MediaItem first = m_source->itemAt(rows.first());
    QString base = stripCollectionCount(first.collectionTitle);
    if (base.isEmpty()) {
        base = stripEpisodeSuffix(first.title);
        if (base.isEmpty())
            base = first.title;
    }
    // 播放列表面板只有约 270px 宽，分组行再带 “[网络]” 前缀会把
    // “共 N 集”挤掉；组头优先保证剧名和集数可读，来源放在 tooltip 里。
    QString title = base;
    if (first.previewOnly)
        title = tr("[临时] ") + title;
    return title + tr("（共%1集）").arg(rows.size());
}

QString PlaylistTreeModel::episodeTitle(int row) const
{
    const MediaItem item = m_source->itemAt(row);
    QString name = item.title;
    const QString base = item.collectionTitle.trimmed();
    if (!base.isEmpty() && name.startsWith(base)) {
        name = name.mid(base.size()).trimmed();
        while (!name.isEmpty()
               && QStringLiteral("·-:： ").contains(name.front())) {
            name.remove(0, 1);
            name = name.trimmed();
        }
    }
    if (name.isEmpty())
        name = item.displayName();
    if (item.previewOnly)
        name = tr("[临时] ") + name;
    return name;
}
