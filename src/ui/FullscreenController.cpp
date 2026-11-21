#include "ui/FullscreenController.h"

#include "ui/PanelOverlayController.h"
#include "ui/VideoSurface.h"

#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

FullscreenController::FullscreenController(VideoSurface *surface, QVBoxLayout *stageLayout,
                                           QWidget *transport, QVBoxLayout *centerLayout,
                                           PanelOverlayController *panels,
                                           qreal stageCornerRadius, QObject *parent)
    : QObject(parent)
    , m_surface(surface)
    , m_stageLayout(stageLayout)
    , m_transport(transport)
    , m_centerLayout(centerLayout)
    , m_panels(panels)
    , m_stageCornerRadius(stageCornerRadius)
{
    Q_ASSERT(m_surface != nullptr);
    Q_ASSERT(m_stageLayout != nullptr);
    Q_ASSERT(m_transport != nullptr);
    Q_ASSERT(m_centerLayout != nullptr);
    Q_ASSERT(m_panels != nullptr);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(2600);
    connect(m_hideTimer, &QTimer::timeout, this, &FullscreenController::hideControls);
}

FullscreenController::~FullscreenController()
{
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

    m_surface->setParent(window);
    m_surface->setCornerRadius(0.0);
    layout->addWidget(m_surface);
    m_surface->show();

    // 控制条本来可能挂在主窗口布局里，也可能已经是贴边浮层；先记住原状态再搬。
    m_transportHost = m_transport->parentWidget();
    m_transportWasInLayout = m_centerLayout->indexOf(m_transport) >= 0;
    if (m_transportWasInLayout)
        m_centerLayout->removeWidget(m_transport);

    m_panels->setEdgeDetectionEnabled(false);
    m_transport->setParent(window);
    m_transport->setProperty("fullscreen", true);
    if (QStyle *style = m_transport->style()) {
        style->unpolish(m_transport);
        style->polish(m_transport);
    }
    m_transport->setVisible(true);
    m_transport->raise();

    window->setMouseTracking(true);
    m_surface->setMouseTracking(true);
    m_transport->setMouseTracking(true);
    window->installEventFilter(this);
    m_surface->installEventFilter(this);
    m_transport->installEventFilter(this);

    window->showFullScreen();
    window->setFocus();
    m_window = window;

    updateControlGeometry();
    showControls();
}

void FullscreenController::exitFullScreen()
{
    if (m_window == nullptr)
        return;

    QWidget *window = m_window;
    m_window = nullptr;
    if (m_hideTimer != nullptr)
        m_hideTimer->stop();

    window->removeEventFilter(this);
    m_surface->removeEventFilter(this);
    m_transport->removeEventFilter(this);

    m_transport->hide();
    m_transport->setParent(m_transportHost);
    m_transport->setProperty("fullscreen", false);
    if (QStyle *style = m_transport->style()) {
        style->unpolish(m_transport);
        style->polish(m_transport);
    }
    if (m_transportWasInLayout)
        m_centerLayout->addWidget(m_transport);

    m_stageLayout->addWidget(m_surface);
    m_surface->setCornerRadius(m_stageCornerRadius);
    m_surface->show();

    m_panels->setEdgeDetectionEnabled(true);
    m_panels->reposition();

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

    // 鼠标还在控制条上时不能收，否则用户刚移到音量/进度条就消失。
    const QPoint global = QCursor::pos();
    const QPoint local = m_transport->mapFromGlobal(global);
    if (m_transport->rect().contains(local)) {
        scheduleHideControls();
        return;
    }
    m_transport->hide();
}

bool FullscreenController::eventFilter(QObject *watched, QEvent *event)
{
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

    if (m_window != nullptr && watched == m_transport) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
            m_hideTimer->stop();
            m_transport->show();
            m_transport->raise();
        } else if (event->type() == QEvent::Leave) {
            scheduleHideControls();
        }
    }
    return QObject::eventFilter(watched, event);
}
