#ifndef VIDEOSURFACE_H
#define VIDEOSURFACE_H

#include <QWidget>

class QVideoFrame;
class QVideoSink;
class QVideoWidget;

/// 视频面：QVideoWidget 负责零拷贝显示，本类只做停播黑场、最小化停绘和首帧恢复。
/// 不再逐帧 toImage()：那会把 GPU 帧反复读回系统内存，D3D 设备一旦丢失就会整窗卡死。
class VideoSurface : public QWidget
{
    Q_OBJECT

public:
    explicit VideoSurface(QWidget *parent = nullptr);

    QVideoSink *sink() const;

    /// 保留接口给 VideoStage；QVideoWidget 是原生窗口，内层圆角由外层舞台边框负责
    void setCornerRadius(qreal radius);
    qreal cornerRadius() const { return m_radius; }

    void clearFrame();
    /// 新视频真正开始播放后恢复收帧；首帧到达前视频窗口保持隐藏，避免旧帧残留
    void resumeFrames();

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    QSize sizeHint() const override;

private:
    void onFrame(const QVideoFrame &frame);

    QVideoWidget *m_videoWidget = nullptr;
    qreal m_radius = 0.0;
    bool m_framesBlocked = false;
};

#endif // VIDEOSURFACE_H
