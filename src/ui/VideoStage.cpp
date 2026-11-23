/// @file VideoStage.cpp
/// @brief 视频舞台容器的圆角边框绘制与子控件联动实现。
#include "ui/VideoStage.h"

#include "ui/ThemeManager.h"
#include "ui/VideoSurface.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>

/// @brief 构造视频舞台容器。
/// @param parent 父窗口。
/// @note 容器本身不承载视频帧，只负责外圈圆角边框和把半径同步给
///       VideoSurface；视频仍由 QVideoWidget 原生窗口显示。
VideoStage::VideoStage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoStage"));
    setAttribute(Qt::WA_StyledBackground, false);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { update(); });
}

/// @brief 设置舞台圆角半径，并同步给内层视频面。
/// @param radius 半径（像素）；负值按 0 处理。
/// @note 半径变化会触发重绘，但不改变视频面的几何尺寸。
void VideoStage::setCornerRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius, radius))
        return;
    m_radius = qMax(0.0, radius);
    applyRadiusToSurface();
    update();
}

/// @brief 查找直接子级中的视频面。
/// @return 命中返回 VideoSurface 指针；尚未 setupUi 或结构变化时返回 nullptr。
VideoSurface *VideoStage::surface() const
{
    return findChild<VideoSurface *>(QString(), Qt::FindDirectChildrenOnly);
}

/// @brief 把当前圆角半径下发给视频面。
/// @note 子控件可能由 .ui 延迟创建，找不到时静默跳过，由 showEvent 再补。
void VideoStage::applyRadiusToSurface()
{
    if (VideoSurface *view = surface())
        view->setCornerRadius(m_radius);
}

/// @brief 尺寸变化时重绘外圈边框。
void VideoStage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

/// @brief 首次显示时绑定视频面的几何变化并补发圆角。
/// @note setupUi 在构造函数之后执行，构造阶段子控件还不存在，因此这里
///       重新安装事件过滤器，避免漏掉第一次 resize/move。
void VideoStage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // setupUi 之后子控件才就位，这里补一次监听与半径下发
    if (VideoSurface *view = surface()) {
        view->removeEventFilter(this);
        view->installEventFilter(this);
        view->setCornerRadius(m_radius);
    }
    update();
}

/// @brief 监听视频面的几何变化，保证外圈边框跟随内层窗口移动/缩放。
bool VideoStage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == surface()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move
            || event->type() == QEvent::Show)) {
        update();
    }
    return QWidget::eventFilter(watched, event);
}

/// @brief 绘制视频面外圈的 1.2 像素主题色圆角边框。
/// @note 只画边框、不填充，避免盖住原生视频窗口；圆角路径按视频面当前
///       geometry 计算，因此子控件移动后必须重绘。
void VideoStage::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    VideoSurface *view = surface();
    if (view == nullptr)
        return;

    const ThemeInfo theme = ThemeManager::instance().current();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(QRectF(view->geometry()), m_radius, m_radius);

    QColor border = theme.stageBorder;
    border.setAlpha(200);
    QPen pen(border, 1.2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}
