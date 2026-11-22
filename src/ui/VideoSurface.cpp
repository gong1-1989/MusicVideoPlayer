#include "ui/VideoSurface.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

VideoSurface::VideoSurface(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoWidget"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    m_videoWidget = new QVideoWidget(this);
    m_videoWidget->setObjectName(QStringLiteral("videoView"));
    // 视频按比例显示，黑边由 QVideoWidget 自己补，不再由 QPainter 算目标矩形
    m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    m_videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 交给原生视频窗口显示；Qt 不会再走 toImage 的 GPU 读回路径
    m_videoWidget->hide();
    layout->addWidget(m_videoWidget);

    connect(m_videoWidget->videoSink(), &QVideoSink::videoFrameChanged, this,
            &VideoSurface::onFrame);
}

QVideoSink *VideoSurface::sink() const
{
    return m_videoWidget != nullptr ? m_videoWidget->videoSink() : nullptr;
}

void VideoSurface::setCornerRadius(qreal radius)
{
    m_radius = qMax(0.0, radius);
    update();
}

void VideoSurface::clearFrame()
{
    m_framesBlocked = true;
    m_hasFrame = false;
    // 直接隐藏原生视频窗口，最后一帧不会继续留在舞台上
    m_videoWidget->hide();
    update();
}

void VideoSurface::resumeFrames()
{
    m_framesBlocked = false;
    // 暂停中切回时播放器不会立刻送新帧；如果之前已有有效帧，主动把原生
    // 视频窗口显示回来，避免只听到声音、舞台一直保持黑屏。
    if (m_hasFrame && !m_videoWidget->isVisible()
        && (window() == nullptr || !window()->isMinimized())) {
        m_videoWidget->show();
    }
}

void VideoSurface::onFrame(const QVideoFrame &frame)
{
    if (m_framesBlocked)
        return;
    if (!frame.isValid())
        return;
    m_hasFrame = true;
    if (window() != nullptr && window()->isMinimized())
        return;
    if (!m_videoWidget->isVisible())
        m_videoWidget->show();
}

void VideoSurface::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);

    if (event->type() != QEvent::WindowStateChange || m_videoWidget == nullptr)
        return;

    if (window() != nullptr && window()->isMinimized()) {
        // 最小化时原生视频窗口不再参与合成，后台只解码不送显
        m_videoWidget->hide();
    } else if (!m_framesBlocked) {
        m_videoWidget->show();
    }
}

QSize VideoSurface::sizeHint() const
{
    return QSize(640, 360);
}

void VideoSurface::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath clip;
    if (m_radius > 0.0)
        clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), m_radius, m_radius);
    else
        clip.addRect(QRectF(rect()));
    painter.setClipPath(clip);
    painter.fillRect(rect(), Qt::black);
}
