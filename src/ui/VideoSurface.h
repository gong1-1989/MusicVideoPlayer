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

    /// 原生视频子控件。全屏时要给它也装事件过滤器，否则鼠标在画面上移动
    /// 时事件只落到这个子控件，外层 VideoSurface 收不到。
    QWidget *videoWidget() const;

    /// 保留接口给 VideoStage；QVideoWidget 是原生窗口，内层圆角由外层舞台边框负责
    void setCornerRadius(qreal radius);

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
    /// 是否曾经收到过有效帧；恢复收帧时据此决定要不要重新显示原生窗口。
    bool m_hasFrame = false;
};

#endif // VIDEOSURFACE_H
