#ifndef PLAYLISTTREEMODEL_H
#define PLAYLISTTREEMODEL_H

#include <QHash>
#include <QSet>
#include <QStandardItemModel>
#include <QStringList>
#include <QVector>

class PlaylistModel;

/// 视频播放列表的展示层：把扁平 PlaylistModel 里同一部剧的多集折叠成
/// 一个父节点，展开后再逐集显示。数据与播放仍以 PlaylistModel 的扁平行号
/// 为准，本模型只负责层级展示和展示行到源行的映射。
class PlaylistTreeModel : public QStandardItemModel
{
    Q_OBJECT

public:
    enum Role {
        /// 该展示节点对应的 PlaylistModel 行号；分组节点存第一集行号。
        SourceRowRole = Qt::UserRole + 200,
        GroupKeyRole,
        IsGroupRole,
        GroupSizeRole,
    };

    explicit PlaylistTreeModel(PlaylistModel *source, QObject *parent = nullptr);

    PlaylistModel *sourceModel() const { return m_source; }

    /// 展示节点对应的源行；分组节点返回第一集，非法节点返回 -1。
    int sourceRow(const QModelIndex &index) const;
    bool isGroup(const QModelIndex &index) const;
    QString groupKey(const QModelIndex &index) const;
    int groupSize(const QModelIndex &index) const;
    QModelIndex indexForSourceRow(int row) const;
    QModelIndex indexForGroup(const QString &key) const;
    /// 分组下全部真实剧集的源行；非分组返回空。
    QVector<int> sourceRowsForGroup(const QString &key) const;

    /// 记住用户展开过的分组；模型重建（增删/恢复）后由视图恢复展开态。
    void setGroupExpanded(const QString &key, bool expanded);
    QStringList expandedGroupKeys() const;

signals:
    /// 重建了顶层/子级结构，视图可在此后恢复展开态。
    void structureRebuilt();

private:
    void rebuild();
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    QString groupTitle(const QVector<int> &rows) const;
    QString episodeTitle(int row) const;

    PlaylistModel *m_source = nullptr;
    QSet<QString> m_expandedKeys;
    /// 上次构建时每条源条目的 DisplayRole；只在展示字段变化时重建，
    /// 播放进度高频 dataChanged 不会导致树反复重置。
    QHash<int, QString> m_sourceDisplay;
};

#endif // PLAYLISTTREEMODEL_H
