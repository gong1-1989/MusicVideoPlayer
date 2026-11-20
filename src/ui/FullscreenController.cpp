#include "ui/FullscreenController.h"

#include "ui/VideoSurface.h"

#include <QEvent>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QWidget>

FullscreenController::FullscreenController(VideoSurface *surface, QVBoxLayout *stageLayout,
                                           qreal stageCornerRadius, QObject *parent)
    : QObject(parent)
    , m_surface(surface)
    , m_stageLayout(stageLayout)
    , m_stageCornerRadius(stageCornerRadius)
{
    Q_ASSERT(m_surface != nullptr);
    Q_ASSERT(m_stageLayout != nullptr);
}

FullscreenController::~FullscreenController()
{
    // 正常关闭路径会先 exit，这里兜底处理非正常析构，避免顶层窗残留。
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

    // 视频面是自绘控件，不依赖原生全屏窗口；搬进独立顶层窗即可。
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

    window->installEventFilter(this);
    window->showFullScreen();
    window->setFocus();

    m_window = window;
}

void FullscreenController::exitFullScreen()
{
    if (m_window == nullptr)
        return;

    QWidget *window = m_window;
    // 先清空状态：window->close() 会再次触发过滤器，此时不能再递归 exit。
    m_window = nullptr;

    window->removeEventFilter(this);
    m_stageLayout->addWidget(m_surface);
    m_surface->setCornerRadius(m_stageCornerRadius);
    m_surface->show();

    window->close();
    window->deleteLater();
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
            // Alt+F4 关闭全屏窗时，也要把视频面放回舞台。
            exitFullScreen();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
