#include "ui/MarqueeDelegate.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QListView>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

MarqueeDelegate::MarqueeDelegate(QListView *view)
    : QStyledItemDelegate(view)
    , m_view(view)
{
    Q_ASSERT(m_view != nullptr);

    m_timer.setInterval(40);
    QObject::connect(&m_timer, &QTimer::timeout, m_view->viewport(), [this] { advance(); });
    m_timer.start();

    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setMouseTracking(true);
    m_view->viewport()->setMouseTracking(true);
    m_view->viewport()->installEventFilter(this);
}

bool MarqueeDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_view->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            const QModelIndex index = m_view->indexAt(pos);
            m_hoveredRow = index.isValid() ? index.row() : -1;
        } else if (event->type() == QEvent::Leave) {
            m_hoveredRow = -1;
        } else if (event->type() == QEvent::Hide || event->type() == QEvent::HideToParent) {
            // 切到另一个列表页后不再 40ms 空转，重新显示时再启动
            m_timer.stop();
        } else if (event->type() == QEvent::Show || event->type() == QEvent::ShowToParent) {
            if (!m_timer.isActive())
                m_timer.start();
        }
    }
    return QStyledItemDelegate::eventFilter(object, event);
}

void MarqueeDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    const QWidget *widget = opt.widget;
    QStyle *style = widget != nullptr ? widget->style() : QApplication::style();

    const QFontMetrics metrics(opt.font);
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const int textWidth = metrics.horizontalAdvance(text);
    const bool scrollRow =
        index.row() == m_hoveredRow || (m_hoveredRow < 0 && index.row() == m_view->currentIndex().row());

    if (textRect.isEmpty() || text.isEmpty() || !scrollRow || textWidth <= textRect.width()) {
        // 放得下就整行走主题默认绘制，选中态颜色完全跟 QSS 一致
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // 背景、悬停/选中态交给主题去画，文字留空后自己按横向偏移画
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const bool selected = (option.state & QStyle::State_Selected) != 0;
    const QPalette::ColorGroup group =
        (option.state & QStyle::State_Enabled) != 0 ? QPalette::Normal : QPalette::Disabled;

    painter->save();
    painter->setClipRect(textRect);
    painter->setPen(opt.palette.color(group,
                                     selected ? QPalette::HighlightedText : QPalette::Text));

    // 两段首尾相接，滚到末尾时无缝接回开头
    const int gap = 36;
    const int period = textWidth + gap;
    const int offset = m_phase % period;
    painter->drawText(QRect(textRect.x() - offset, textRect.y(), textWidth, textRect.height()),
                      Qt::AlignVCenter | Qt::AlignLeft, text);
    painter->drawText(
        QRect(textRect.x() - offset + period, textRect.y(), textWidth, textRect.height()),
        Qt::AlignVCenter | Qt::AlignLeft, text);
    painter->restore();
}

void MarqueeDelegate::advance()
{
    const int row = m_hoveredRow >= 0 ? m_hoveredRow : m_view->currentIndex().row();
    if (row < 0)
        return;
    const QModelIndex index = m_view->model() ? m_view->model()->index(row, 0, QModelIndex())
                                              : QModelIndex();
    if (!index.isValid())
        return;
    const QRect rect = m_view->visualRect(index);
    if (!rect.isValid())
        return;
    // 放得下就不用动，省掉无意义的重绘
    const QFontMetrics metrics(m_view->font());
    if (metrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) <= rect.width() - 24)
        return;

    m_phase += 2;
    if (m_phase > 100000)
        m_phase = 0;
    m_view->viewport()->update(rect);
}
