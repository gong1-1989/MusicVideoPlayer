#include "ui/LyricsView.h"

#include "ui/ThemeManager.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>

LyricsView::LyricsView(QWidget *parent)
    : QListWidget(parent)
{
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { viewport()->update(); });
}

void LyricsView::setFadeExtent(int pixels)
{
    m_fade = qMax(0, pixels);
    viewport()->update();
}

void LyricsView::paintEvent(QPaintEvent *event)
{
    QListWidget::paintEvent(event);

    if (m_fade <= 0)
        return;

    const QRect area = viewport()->rect();
    if (area.height() <= m_fade * 2)
        return;

    const QColor base = ThemeManager::instance().current().fadeColor;

    QPainter painter(viewport());
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(Qt::NoPen);

    // 顶部：从底色实色过渡到全透明
    QLinearGradient top(area.left(), area.top(), area.left(), area.top() + m_fade);
    QColor solid = base;
    solid.setAlpha(240);
    QColor clear = base;
    clear.setAlpha(0);
    top.setColorAt(0.0, solid);
    top.setColorAt(1.0, clear);
    painter.setBrush(top);
    painter.drawRect(QRect(area.left(), area.top(), area.width(), m_fade));

    // 底部：反向过渡
    QLinearGradient bottom(area.left(), area.bottom() - m_fade, area.left(), area.bottom());
    bottom.setColorAt(0.0, clear);
    bottom.setColorAt(1.0, solid);
    painter.setBrush(bottom);
    painter.drawRect(QRect(area.left(), area.bottom() - m_fade, area.width(), m_fade));
}
