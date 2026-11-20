#ifndef FULLSCREENCONTROLLER_H
#define FULLSCREENCONTROLLER_H

#include <QObject>

class QEvent;
class QVBoxLayout;
class QWidget;
class VideoSurface;

/// 全屏播放窗口：把视频面从舞台临时搬到独立顶层窗，退出时放回原布局。
/// 只负责窗口搬运、Esc/关闭处理和圆角恢复，不碰播放状态。
class FullscreenController : public QObject
{
    Q_OBJECT

public:
    FullscreenController(VideoSurface *surface, QVBoxLayout *stageLayout,
                         qreal stageCornerRadius = 16.0, QObject *parent = nullptr);
    ~FullscreenController() override;

    bool isFullScreen() const { return m_window != nullptr; }

public slots:
    void toggle();
    void enterFullScreen();
    void exitFullScreen();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    VideoSurface *m_surface = nullptr;
    QVBoxLayout *m_stageLayout = nullptr;
    qreal m_stageCornerRadius = 16.0;
    QWidget *m_window = nullptr;
};

#endif // FULLSCREENCONTROLLER_H
