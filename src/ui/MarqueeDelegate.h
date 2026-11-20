#ifndef MARQUEEWDELEGATE_H
#define MARQUEEWDELEGATE_H

#include <QStyledItemDelegate>
#include <QTimer>

class QListView;

/// 列表行文字横向滚动：放不下时让“悬停/当前”那一行滚动，其余行保持省略号。
/// 独立成组件，避免这类纯绘制逻辑继续堆在主窗里。
class MarqueeDelegate : public QStyledItemDelegate
{
public:
    explicit MarqueeDelegate(QListView *view);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

private:
    void advance();

    QListView *m_view = nullptr;
    QTimer m_timer;
    int m_hoveredRow = -1;
    int m_phase = 0;
};

#endif // MARQUEEWDELEGATE_H
