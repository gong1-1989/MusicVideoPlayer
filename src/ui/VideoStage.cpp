#include "ui/VideoStage.h"

#include "ui/ThemeManager.h"
#include "ui/VideoSurface.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>

VideoStage::VideoStage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoStage"));
    setAttribute(Qt::WA_StyledBackground, false);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { update(); });
}

void VideoStage::setCornerRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius, radius))
        return;
    m_radius = qMax(0.0, radius);
    applyRadiusToSurface();
    update();
}

VideoSurface *VideoStage::surface() const
{
    return findChild<VideoSurface *>(QString(), Qt::FindDirectChildrenOnly);
}

void VideoStage::applyRadiusToSurface()
{
    if (VideoSurface *view = surface())
        view->setCornerRadius(m_radius);
}

void VideoStage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void VideoStage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // setupUi 之后子控件才就位，这里补一次监听与半径下发
    if (VideoSurface *view = surface()) {
        view->removeEventFilter(this);
        view->installEventFilter(this);
        view->setCornerRadius(m_radius);
    }
    update();
}

bool VideoStage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == surface()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move
            || event->type() == QEvent::Show)) {
        update();
    }
    return QWidget::eventFilter(watched, event);
}

void VideoStage::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    VideoSurface *view = surface();
    if (view == nullptr)
        return;

    const ThemeInfo theme = ThemeManager::instance().current();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(QRectF(view->geometry()), m_radius, m_radius);

    QColor border = theme.stageBorder;
    border.setAlpha(200);
    QPen pen(border, 1.2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}
