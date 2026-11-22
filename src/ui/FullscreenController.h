#ifndef FULLSCREENCONTROLLER_H
#define FULLSCREENCONTROLLER_H

#include <QObject>
#include <QPointer>

class PanelOverlayController;
class QEvent;
class QTimer;
class QVBoxLayout;
class QWidget;
class VideoSurface;

/// 全屏播放窗口：把视频面从舞台临时搬到独立顶层窗，退出时放回原布局。
/// 控制条复用主窗口已经接好播放信号的 panelTransport，全屏时浮在底边，
/// 鼠标贴近屏幕底部才弹出，避免另做一套按钮导致状态不同步。
class FullscreenController : public QObject
{
    Q_OBJECT

public:
    FullscreenController(VideoSurface *surface, QVBoxLayout *stageLayout,
                         QWidget *transport, QVBoxLayout *centerLayout,
                         PanelOverlayController *panels,
                         qreal stageCornerRadius = 16.0, QObject *parent = nullptr);
    ~FullscreenController() override;

public slots:
    void toggle();
    void enterFullScreen();
    void exitFullScreen();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateControlGeometry();
    void showControls();
    void scheduleHideControls();
    void hideControls();

    VideoSurface *m_surface = nullptr;
    QVBoxLayout *m_stageLayout = nullptr;
    QWidget *m_transport = nullptr;
    QVBoxLayout *m_centerLayout = nullptr;
    PanelOverlayController *m_panels = nullptr;
    qreal m_stageCornerRadius = 16.0;
    QWidget *m_window = nullptr;
    /// VideoSurface 内部的原生 QVideoWidget；鼠标事件主要落在它身上
    QWidget *m_videoWidget = nullptr;
    QPointer<QWidget> m_transportHost;
    QTimer *m_hideTimer = nullptr;
    bool m_transportWasInLayout = false;
};

#endif // FULLSCREENCONTROLLER_H
