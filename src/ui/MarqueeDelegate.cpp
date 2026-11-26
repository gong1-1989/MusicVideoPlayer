/// @file MarqueeDelegate.cpp
/// @brief 列表行文字跑马灯委托实现。
#include "ui/MarqueeDelegate.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QEvent>
#include <QFontMetrics>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

/// @brief 构造委托并启动 40ms 推进定时器。
/// @param view 目标列表视图，必须非空；委托不拥有它。
/// @note 关闭横向滚动条、开启鼠标跟踪，并在 viewport 上安装过滤器
///       以感知悬停行和显示/隐藏状态。
MarqueeDelegate::MarqueeDelegate(QAbstractItemView *view)
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

/// @brief 跟踪悬停行并在列表隐藏时停表，避免后台空转。
bool MarqueeDelegate::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_view->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            m_hoveredIndex = m_view->indexAt(pos);
        } else if (event->type() == QEvent::Leave) {
            m_hoveredIndex = QPersistentModelIndex();
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

/// @brief 绘制列表行；只有悬停行或当前行且文本超宽时才走跑马灯。
/// @note 未滚动的情况完全交给基类/QSS，保证选中色与主题一致；
///       滚动时背景仍由样式绘制，文字按相位横向偏移并双份拼接实现无缝循环。
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
        index == m_hoveredIndex
        || (!m_hoveredIndex.isValid() && index == m_view->currentIndex());

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

/// @brief 推进滚动相位并只重绘需要滚动的那一行。
/// @note 文本放得下时不推进、不重绘；相位超过 100000 归零，避免整数无限增长。
void MarqueeDelegate::advance()
{
    QModelIndex index = m_hoveredIndex.isValid() ? QModelIndex(m_hoveredIndex)
                                                 : m_view->currentIndex();
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
