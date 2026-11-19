#include "ui/VideoSurface.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QVideoFrame>
#include <QVideoSink>

VideoSurface::VideoSurface(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoWidget"));
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);

    m_sink = new QVideoSink(this);
    connect(m_sink, &QVideoSink::videoFrameChanged, this, &VideoSurface::onFrame);
}

void VideoSurface::setCornerRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius, radius))
        return;
    m_radius = qMax(0.0, radius);
    update();
}

void VideoSurface::clearFrame()
{
    // 停播时 QVideoSink 仍可能排着最后一帧；只清 QImage 会被它立刻画回来
    m_framesBlocked = true;
    m_image = QImage();
    update();
}

void VideoSurface::resumeFrames()
{
    m_framesBlocked = false;
}

QSize VideoSurface::sizeHint() const
{
    return QSize(640, 360);
}

void VideoSurface::onFrame(const QVideoFrame &frame)
{
    if (m_framesBlocked)
        return;
    if (!frame.isValid()) {
        return;
    }
    // 转成 QImage 后由我们自己绘制；这里一次性转换，绘制时不再重复解码
    const QImage image = frame.toImage();
    if (!image.isNull())
        m_image = image;
    update();
}

void VideoSurface::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath clip;
    if (m_radius > 0.0)
        clip.addRoundedRect(bounds, m_radius, m_radius);
    else
        clip.addRect(bounds);
    painter.setClipPath(clip);

    // 圆角之外的区域保持不画，露出父控件的面板底色
    painter.fillRect(rect(), Qt::black);

    if (m_image.isNull())
        return;

    const QSizeF imageSize(m_image.size());
    QSizeF scaled = imageSize;
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);

    const QRectF target(bounds.left() + (bounds.width() - scaled.width()) / 2.0,
                        bounds.top() + (bounds.height() - scaled.height()) / 2.0,
                        scaled.width(), scaled.height());
    painter.drawImage(target, m_image);
}
