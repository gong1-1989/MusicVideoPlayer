#ifndef FULLSCREENCONTROLLER_H
#define FULLSCREENCONTROLLER_H

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPoint>
#include <QPointer>

class PanelOverlayController;
class QEvent;
class QMediaPlayer;
class QTimer;
class QVBoxLayout;
class QWidget;
class QVideoWidget;
class VideoSurface;

/// 全屏播放窗口：新建独立视频控件并接管播放器视频输出，
/// 不再搬运主窗里的原生 QVideoWidget，避免 Windows 全屏切换后丢失呈现目标。
/// 控制条复用主窗口已经接好播放信号的 panelTransport，全屏时浮在底边。
class FullscreenController : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    FullscreenController(VideoSurface *surface, QMediaPlayer *player,
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
#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;
#endif

private:
    void updateControlGeometry();
    void showControls();
    void scheduleHideControls();
    void hideControls();
    /// 全屏空闲时连系统鼠标一起隐藏；有鼠标活动立即恢复箭头。
    void setCursorHidden(bool hidden);

    VideoSurface *m_surface = nullptr;
    QMediaPlayer *m_player = nullptr;
    QWidget *m_transport = nullptr;
    QVBoxLayout *m_centerLayout = nullptr;
    PanelOverlayController *m_panels = nullptr;
    qreal m_stageCornerRadius = 16.0;
    QWidget *m_window = nullptr;
    /// 全屏窗里的独立视频控件；不搬运主窗原生子控件，避免 Windows 丢失呈现目标。
    QVideoWidget *m_fsVideo = nullptr;
    /// 全屏期间的原生视频控件，接收鼠标事件
    QWidget *m_videoWidget = nullptr;
    QPointer<QWidget> m_transportHost;
    QTimer *m_hideTimer = nullptr;
    bool m_transportWasInLayout = false;
    /// 应用级覆盖光标是否生效；用于保证只 restore 一次，避免污染其他窗口。
    bool m_cursorOverrideActive = false;
    /// Win32 显示计数器需要步进的次数；恢复时必须同样步进回去，不能重复或漏还。
    int m_cursorHideSteps = 0;
    /// 隐藏前的真实鼠标位置；只有坐标变化才算用户移动，过滤光标设置产生的合成事件。
    QPoint m_lastMouseGlobalPos;
};

#endif // FULLSCREENCONTROLLER_H
