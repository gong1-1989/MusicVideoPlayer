#ifndef VIDEOSURFACE_H
#define VIDEOSURFACE_H

#include <QImage>
#include <QWidget>

class QVideoFrame;
class QVideoSink;

/// 自绘视频面：从 QVideoSink 收帧，用 QPainter 画到控件上。
/// 用 QVideoWidget 时视频渲染在独立原生窗口里，QSS 圆角和 setMask 都裁不动；
/// 自己画才能真正做圆角裁切，顺带把等比缩放/黑边也收进来。
class VideoSurface : public QWidget
{
    Q_OBJECT

public:
    explicit VideoSurface(QWidget *parent = nullptr);

    QVideoSink *sink() const { return m_sink; }

    void setCornerRadius(qreal radius);
    qreal cornerRadius() const { return m_radius; }

    void clearFrame();

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;

private:
    void onFrame(const QVideoFrame &frame);

    QVideoSink *m_sink = nullptr;
    QImage m_image;
    qreal m_radius = 0.0;
};

#endif // VIDEOSURFACE_H
