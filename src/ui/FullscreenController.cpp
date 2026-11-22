#include "ui/FullscreenController.h"

#include "ui/PanelOverlayController.h"
#include "ui/VideoSurface.h"

#include <QCursor>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLayout>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

FullscreenController::FullscreenController(VideoSurface *surface, QMediaPlayer *player,
                                           QWidget *transport, QVBoxLayout *centerLayout,
                                           PanelOverlayController *panels,
                                           qreal stageCornerRadius, QObject *parent)
    : QObject(parent)
    , m_surface(surface)
    , m_player(player)
    , m_transport(transport)
    , m_centerLayout(centerLayout)
    , m_panels(panels)
    , m_stageCornerRadius(stageCornerRadius)
{
    Q_ASSERT(m_surface != nullptr);
    Q_ASSERT(m_player != nullptr);
    Q_ASSERT(m_transport != nullptr);
    Q_ASSERT(m_centerLayout != nullptr);
    Q_ASSERT(m_panels != nullptr);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(2600);
    connect(m_hideTimer, &QTimer::timeout, this, &FullscreenController::hideControls);

#ifdef Q_OS_WIN
    // 原生视频窗口会在 WM_SETCURSOR 里把自己的箭头光标恢复回来；
    // 应用级事件过滤器拦截这条消息，全屏空闲时才能真正隐藏系统光标。
    QGuiApplication::instance()->installNativeEventFilter(this);
#endif
}

FullscreenController::~FullscreenController()
{
#ifdef Q_OS_WIN
    if (QGuiApplication::instance() != nullptr)
        QGuiApplication::instance()->removeNativeEventFilter(this);
#endif
    if (m_window != nullptr)
        exitFullScreen();
}

void FullscreenController::toggle()
{
    if (m_window != nullptr)
        exitFullScreen();
    else
        enterFullScreen();
}

void FullscreenController::enterFullScreen()
{
    if (m_window != nullptr)
        return;

    auto *window = new QWidget(nullptr, Qt::Window);
    window->setObjectName(QStringLiteral("fullScreenWindow"));
    window->setWindowTitle(tr("全屏播放"));
    window->setStyleSheet(QStringLiteral("QWidget#fullScreenWindow { background: black; }"));

    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 主窗里的 QVideoWidget 不搬动：Qt 在 Windows 上重挂原生视频子窗后，
    // 播放过程中会再次丢失呈现目标，表现为首次全屏过几秒变黑。
    // 这里在独立全屏窗内新建视频控件，并把播放器的视频输出切过来。
    m_fsVideo = new QVideoWidget(window);
    m_fsVideo->setObjectName(QStringLiteral("fullScreenVideo"));
    m_fsVideo->setAspectRatioMode(Qt::KeepAspectRatio);
    m_fsVideo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_fsVideo->setMouseTracking(true);
    m_fsVideo->setStyleSheet(QStringLiteral("background: black;"));
    layout->addWidget(m_fsVideo);
    m_videoWidget = m_fsVideo;

    if (QWidget *mainVideoWidget = m_surface->videoWidget())
        mainVideoWidget->hide();
    m_player->setVideoSink(m_fsVideo->videoSink());

    // 控制条本来可能挂在主窗口布局里，也可能已经是贴边浮层；先记住原状态再搬。
    m_transportHost = m_transport->parentWidget();
    m_transportWasInLayout = m_centerLayout->indexOf(m_transport) >= 0;
    if (m_transportWasInLayout)
        m_centerLayout->removeWidget(m_transport);

    m_panels->setEdgeDetectionEnabled(false);
    // QVideoWidget 是原生子窗口，普通 QWidget 兄弟即使 raise() 也压不住它；
    // 控制条必须自己也成为原生窗口，才能在全屏视频画面上正确悬浮。
    m_transport->setAttribute(Qt::WA_NativeWindow, true);
    m_transport->setParent(window);
    m_transport->setProperty("fullscreen", true);
    if (QStyle *style = m_transport->style()) {
        style->unpolish(m_transport);
        style->polish(m_transport);
    }
    m_transport->setVisible(true);
    m_transport->raise();

    window->setMouseTracking(true);
    m_transport->setMouseTracking(true);
    window->installEventFilter(this);
    m_transport->installEventFilter(this);

    // QVideoWidget 是原生子窗口，鼠标事件不会冒泡到外层；单独监听它。
    if (m_videoWidget != nullptr) {
        m_videoWidget->setMouseTracking(true);
        m_videoWidget->installEventFilter(this);
    }

    window->showFullScreen();
    window->raise();
    window->activateWindow();
    window->setFocus();
    m_window = window;
    m_lastMouseGlobalPos = QCursor::pos();
    setCursorHidden(false);

    updateControlGeometry();
    showControls();

    QTimer::singleShot(0, window, [this, window] {
        if (m_window != window)
            return;
        if (QLayout *windowLayout = window->layout())
            windowLayout->activate();
        if (m_fsVideo != nullptr) {
            m_fsVideo->show();
            m_fsVideo->raise();
            m_fsVideo->update();
        }
        updateControlGeometry();
    });
    QTimer::singleShot(120, window, [this, window] {
        if (m_window != window)
            return;
        if (m_fsVideo != nullptr)
            m_fsVideo->update();
    });
}

void FullscreenController::exitFullScreen()
{
    if (m_window == nullptr)
        return;

    QWidget *window = m_window;
    if (m_hideTimer != nullptr)
        m_hideTimer->stop();
    setCursorHidden(false);
    m_window = nullptr;

    window->removeEventFilter(this);
    m_transport->removeEventFilter(this);
    if (m_videoWidget != nullptr) {
        m_videoWidget->removeEventFilter(this);
        m_videoWidget = nullptr;
    }

    // 先把视频输出切回主窗，再关闭全屏窗；否则播放器仍指向即将销毁的控件。
    m_player->setVideoSink(m_surface->sink());
    m_surface->resumeFrames();
    m_surface->update();

    m_transport->hide();
    m_transport->setParent(m_transportHost);
    m_transport->setProperty("fullscreen", false);
    if (QStyle *style = m_transport->style()) {
        style->unpolish(m_transport);
        style->polish(m_transport);
    }
    if (m_transportWasInLayout)
        m_centerLayout->addWidget(m_transport);

    m_surface->setCornerRadius(m_stageCornerRadius);
    m_surface->show();

    m_panels->setEdgeDetectionEnabled(true);
    m_panels->reposition();

    if (m_fsVideo != nullptr) {
        m_fsVideo->hide();
        m_fsVideo = nullptr;
    }
    window->close();
    window->deleteLater();
}

void FullscreenController::updateControlGeometry()
{
    if (m_window == nullptr)
        return;

    // 留出视频画面本身的宽度，控制条只占底部中间一段，避免全屏时铺满整个屏宽。
    const int availableWidth = qMax(320, m_window->width() - 48);
    const int width = qMin(1120, availableWidth);
    const int height = qBound(112, m_transport->sizeHint().height(), 150);
    const int x = (m_window->width() - width) / 2;
    const int y = m_window->height() - height - 18;
    m_transport->setGeometry(x, y, width, height);
    m_transport->raise();
}

void FullscreenController::showControls()
{
    if (m_window == nullptr)
        return;

    setCursorHidden(false);
    updateControlGeometry();
    m_transport->show();
    m_transport->raise();
    scheduleHideControls();
}

void FullscreenController::scheduleHideControls()
{
    if (m_window == nullptr)
        return;
    m_hideTimer->start();
}

void FullscreenController::hideControls()
{
    if (m_window == nullptr || m_transport == nullptr)
        return;

    // 正在拖动进度条/音量滑块时不能收；只要鼠标按住，就让控制条继续留着。
    if (QGuiApplication::mouseButtons() != Qt::NoButton) {
        scheduleHideControls();
        return;
    }
    // 鼠标停在控制条上也算“无操作”，到时间同样隐藏；
    // 下一次移动会重新显示，避免控制条永久压在画面上。
    m_transport->hide();
    // 控制条和鼠标一起收起，全屏观影时不再有箭头停在画面中央。
    m_lastMouseGlobalPos = QCursor::pos();
    setCursorHidden(true);
    qInfo() << "全屏控制条已隐藏，光标隐藏状态：" << m_cursorOverrideActive;
}

void FullscreenController::setCursorHidden(bool hidden)
{
    if (m_window == nullptr)
        return;

    if (hidden) {
        if (m_cursorOverrideActive)
            return;
        m_cursorOverrideActive = true;
        m_window->setCursor(Qt::BlankCursor);
        if (m_surface != nullptr)
            m_surface->setCursor(Qt::BlankCursor);
        if (m_videoWidget != nullptr)
            m_videoWidget->setCursor(Qt::BlankCursor);
        // QVideoWidget 是原生子窗口，WM_SETCURSOR 可能绕开 Qt 的控件光标；
        // 再用应用级覆盖光标和 Win32 原生光标兜底，确保空闲时真正看不到箭头。
        QGuiApplication::setOverrideCursor(Qt::BlankCursor);
#ifdef Q_OS_WIN
        // 只改光标形状仍可能被原生视频子窗的 WM_SETCURSOR 复原；
        // 同时把 Win32 光标显示计数减一，确保系统级真正不可见。
        if (m_cursorHideSteps == 0) {
            int count = ::ShowCursor(FALSE);
            m_cursorHideSteps = 1;
            // Windows 的显示计数可能被其他 UI/远程控制工具抬到正数，
            // 只减一次仍可见；一直减到负数再开始恢复，隐藏才可靠。
            while (count > 0) {
                count = ::ShowCursor(FALSE);
                ++m_cursorHideSteps;
            }
        }
        ::SetCursor(nullptr);
#endif
        QTimer::singleShot(80, m_window, [this] {
            if (m_window == nullptr || !m_cursorOverrideActive)
                return;
#ifdef Q_OS_WIN
            ::SetCursor(nullptr);
#endif
        });
        return;
    }

    if (!m_cursorOverrideActive)
        return;
    qInfo() << "全屏光标恢复显示";
    m_cursorOverrideActive = false;
    QGuiApplication::restoreOverrideCursor();
    m_window->setCursor(Qt::ArrowCursor);
    if (m_surface != nullptr)
        m_surface->setCursor(Qt::ArrowCursor);
    if (m_videoWidget != nullptr)
        m_videoWidget->setCursor(Qt::ArrowCursor);
#ifdef Q_OS_WIN
    while (m_cursorHideSteps > 0) {
        ::ShowCursor(TRUE);
        --m_cursorHideSteps;
    }
    ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
#endif
}

#ifdef Q_OS_WIN
bool FullscreenController::nativeEventFilter(const QByteArray &eventType, void *message,
                                             qintptr *result)
{
    Q_UNUSED(eventType)
    if (message == nullptr)
        return false;

    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_MOUSEMOVE) {
        // 原生视频子窗会吞掉部分 Qt 鼠标事件；用窗口消息兜底，
        // 保证隐藏光标后移动鼠标能恢复，并且靠近底边能唤出控制条。
        if (m_window == nullptr || !m_window->isFullScreen())
            return false;
        const QPoint global(msg->pt.x, msg->pt.y);
        if (global == m_lastMouseGlobalPos)
            return false;
        m_lastMouseGlobalPos = global;
        setCursorHidden(false);
        const QPoint local = m_window->mapFromGlobal(global);
        if (local.y() >= m_window->height() - 90)
            showControls();
        else
            scheduleHideControls();
        return false;
    }
    if (!m_cursorOverrideActive)
        return false;
    if (msg->message == WM_SETCURSOR) {
        // 返回 TRUE 表示已处理，阻止 Qt/原生视频子窗把箭头光标重新设回来。
        ::SetCursor(nullptr);
        if (result != nullptr)
            *result = 1;
        return true;
    }
    return false;
}
#endif

bool FullscreenController::eventFilter(QObject *watched, QEvent *event){
    // 任何真实的鼠标活动都先恢复光标；空闲计时结束后再一起隐藏。
    if (m_window != nullptr) {
        if (event->type() == QEvent::MouseMove) {
            // 设置光标形状本身会触发一批合成 MouseMove，坐标没有变化，
            // 不能把它们当成用户活动，否则光标刚藏起来就被自己弹回来。
            const QPoint global = QCursor::pos();
            if (global != m_lastMouseGlobalPos) {
                m_lastMouseGlobalPos = global;
                setCursorHidden(false);
                scheduleHideControls();
            }
        } else if (event->type() == QEvent::MouseButtonPress
                   || event->type() == QEvent::MouseButtonRelease
                   || event->type() == QEvent::Wheel) {
            setCursorHidden(false);
            scheduleHideControls();
        }
    }

    if (m_window != nullptr && watched == m_window) {
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                exitFullScreen();
                return true;
            }
        } else if (event->type() == QEvent::Close) {
            exitFullScreen();
            return true;
        } else if (event->type() == QEvent::Resize) {
            updateControlGeometry();
        } else if (event->type() == QEvent::MouseMove) {
            const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
            if (pos.y() >= m_window->height() - 90)
                showControls();
            else if (m_transport->isVisible()
                     && pos.y() < m_transport->geometry().top() - 24)
                scheduleHideControls();
        } else if (event->type() == QEvent::Leave) {
            const QPoint global = QCursor::pos();
            if (!m_window->frameGeometry().contains(global))
                m_transport->hide();
        }
        return QObject::eventFilter(watched, event);
    }

    if (m_window != nullptr && watched == m_surface) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint surfacePos =
                static_cast<QMouseEvent *>(event)->position().toPoint();
            const QPoint windowPos = m_window->mapFromGlobal(
                m_surface->mapToGlobal(surfacePos));
            if (windowPos.y() >= m_window->height() - 90)
                showControls();
            else if (m_transport->isVisible()
                     && windowPos.y() < m_transport->geometry().top() - 24)
                scheduleHideControls();
        } else if (event->type() == QEvent::Leave) {
            scheduleHideControls();
        } else if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                exitFullScreen();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

    if (m_window != nullptr && m_videoWidget != nullptr && watched == m_videoWidget) {
        if (event->type() == QEvent::MouseMove) {
            const QPoint surfacePos =
                static_cast<QMouseEvent *>(event)->position().toPoint();
            const QPoint windowPos = m_window->mapFromGlobal(
                m_videoWidget->mapToGlobal(surfacePos));
            // 画面是主要活动区域：靠近底边弹出控制条，其余位置只重置隐藏计时。
            if (windowPos.y() >= m_window->height() - 90)
                showControls();
            else if (m_transport->isVisible())
                scheduleHideControls();
        } else if (event->type() == QEvent::Leave) {
            scheduleHideControls();
        }
        return QObject::eventFilter(watched, event);
    }

    if (m_window != nullptr && watched == m_transport) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
            // 鼠标在控制条上活动时保持可见，但停止移动 2.6 秒后仍然自动隐藏。
            m_transport->show();
            m_transport->raise();
            scheduleHideControls();
        } else if (event->type() == QEvent::Leave) {
            scheduleHideControls();
        }
    }
    return QObject::eventFilter(watched, event);
}
