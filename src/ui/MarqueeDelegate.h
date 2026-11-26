#ifndef MARQUEEWDELEGATE_H
#define MARQUEEWDELEGATE_H

#include <QPersistentModelIndex>
#include <QStyledItemDelegate>
#include <QTimer>

class QAbstractItemView;

/// 列表行文字横向滚动：放不下时让“悬停/当前”那一行滚动，其余行保持省略号。
/// 独立成组件，避免这类纯绘制逻辑继续堆在主窗里。
class MarqueeDelegate : public QStyledItemDelegate
{
public:
    explicit MarqueeDelegate(QAbstractItemView *view);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

private:
    void advance();

    QAbstractItemView *m_view = nullptr;
    QTimer m_timer;
    /// 树形列表里不同父节点下的 index.row() 会重复，必须按 index 而不是行号
    /// 记住悬停项，否则展开剧集后高亮/跑马灯会错到另一行。
    QPersistentModelIndex m_hoveredIndex;
    int m_phase = 0;
};

#endif // MARQUEEWDELEGATE_H
