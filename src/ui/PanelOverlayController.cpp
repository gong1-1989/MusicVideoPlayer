/// @file PanelOverlayController.cpp
/// @brief 侧栏与底部控制条的贴边浮层/常显布局切换实现。
#include "ui/PanelOverlayController.h"

#include "settings/AppSettings.h"

#include <QBoxLayout>
#include <QCursor>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

/// @brief 构造浮层控制器并启动 120ms 贴边检测定时器。
/// @param ui 主窗控件集合，所有指针必须非空。
/// @param parent 父对象。
PanelOverlayController::PanelOverlayController(const PanelOverlayUi &ui, QObject *parent)
    : QObject(parent)
    , m_ui(ui)
{
    Q_ASSERT(m_ui.centralWidget != nullptr);
    Q_ASSERT(m_ui.sidebar != nullptr);
    Q_ASSERT(m_ui.stage != nullptr);
    Q_ASSERT(m_ui.transport != nullptr);
    Q_ASSERT(m_ui.rootLayout != nullptr);
    Q_ASSERT(m_ui.centerLayout != nullptr);

    // 浮层/常显切换会拆装侧栏；记住常显模式下用户拖出的宽度。
    m_sidebarLayoutWidth = AppSettings::sidebarWidth();
    m_edgeTimer = new QTimer(this);
    m_edgeTimer->setInterval(120);
    connect(m_edgeTimer, &QTimer::timeout, this, &PanelOverlayController::updateEdgePanels);
    m_edgeTimer->start();
}

/// @brief 按当前设置切换侧栏/控制条的浮层或常显状态，并同步贴边检测。
/// @note 收起时控件脱离布局、贴边计算；常显时回布局占位。
void PanelOverlayController::applyVisibility()
{
    m_sidebarAutoHide = AppSettings::sidebarAutoHide();
    m_controlsAutoHide = AppSettings::controlsAutoHide();

    // 收起 = 浮层（贴边弹出、不参与布局）；常显 = 回布局（正常占位）
    setSidebarFloating(m_sidebarAutoHide);
    setControlsFloating(m_controlsAutoHide);

    m_sidebarPeek = false;
    m_ui.sidebar->setVisible(!m_sidebarAutoHide);
    m_ui.transport->setVisible(!m_controlsAutoHide);

    if (m_sidebarAutoHide)
        positionSidebarOverlay();
    if (m_controlsAutoHide)
        positionControlOverlay();

    // 两侧都常显时没有任何贴边检测需求，停掉 120ms 定时器
    if (m_edgeDetectionEnabled && (m_sidebarAutoHide || m_controlsAutoHide)) {
        if (!m_edgeTimer->isActive())
            m_edgeTimer->start();
    } else {
        m_edgeTimer->stop();
    }

    if (m_ui.sidebar->isVisible())
        m_ui.sidebar->raise();
    if (m_ui.transport->isVisible())
        m_ui.transport->raise();
}

/// @brief 重新计算侧栏和控制条的浮层几何。
void PanelOverlayController::reposition()
{
    positionSidebarOverlay();
    positionControlOverlay();
}

/// @brief 全屏等场景临时关闭/恢复贴边检测。
/// @param enabled false 时停表；true 时按当前设置重建浮层状态。
void PanelOverlayController::setEdgeDetectionEnabled(bool enabled)
{
    if (m_edgeDetectionEnabled == enabled)
        return;
    m_edgeDetectionEnabled = enabled;
    if (!enabled)
        m_edgeTimer->stop();

    // 全屏期间控制条归全屏窗口管理，恢复时按当前设置重新贴回主窗口
    if (enabled)
        applyVisibility();
}

/// @brief 每 120ms 检测鼠标是否贴到左边缘/底边，显示或收起浮层。
/// @note 窗口最小化/隐藏时直接返回，避免后台空转；鼠标在控制条范围或底边内时保持显示。
void PanelOverlayController::updateEdgePanels()
{
    if (!m_edgeDetectionEnabled)
        return;
    if (!m_sidebarAutoHide && !m_controlsAutoHide)
        return;

    // 最小化/隐藏时 QCursor::pos() 和窗口几何都没有意义，继续 120ms 取光标只会
    // 在后台空转；恢复显示后定时器仍在，下一拍自然继续检测。
    const QWidget *window = m_ui.centralWidget->window();
    if (window != nullptr && (!window->isVisible() || window->isMinimized()))
        return;

    const QPoint local = m_ui.centralWidget->mapFromGlobal(QCursor::pos());
    const bool inside = m_ui.centralWidget->rect().contains(local);

    // 左侧栏：鼠标贴到窗口左边缘弹出，移出侧栏范围就收回去
    if (m_sidebarAutoHide) {
        if (!m_sidebarPeek && inside && local.x() <= 10) {
            positionSidebarOverlay();
            m_ui.sidebar->show();
            m_ui.sidebar->raise();
            m_sidebarPeek = true;
        } else if (m_sidebarPeek
                   && (!inside || local.x() > m_ui.sidebar->width() + 8)) {
            m_ui.sidebar->hide();
            m_sidebarPeek = false;
        }
    }

    // 底部控制条：贴到底边弹出，鼠标离开控制条范围就收回
    if (m_controlsAutoHide) {
        const bool controlsVisible = m_ui.transport->isVisible();
        if (!controlsVisible && inside
            && local.y() >= m_ui.centralWidget->height() - 20) {
            positionControlOverlay();
            m_ui.transport->show();
            m_ui.transport->raise();
        } else if (controlsVisible && !inside) {
            m_ui.transport->hide();
        } else if (controlsVisible) {
            const bool keepShown =
                m_controlsRect.adjusted(-8, -8, 8, 8).contains(local)
                || local.y() >= m_ui.centralWidget->height() - 20;
            if (!keepShown)
                m_ui.transport->hide();
        }
    }
}

/// @brief 侧栏在“浮层”和“布局占位”之间切换。
/// @param floating true 脱离布局并挂到 centralWidget；false 插回根布局最左。
void PanelOverlayController::setSidebarFloating(bool floating)
{
    if (floating == m_sidebarFloating)
        return;

    m_sidebarFloating = floating;
    m_sidebarPeek = false;

    if (m_ui.sidebarSplitter != nullptr) {
        if (floating) {
            // 先记下常显时的实际宽度，浮层收回去再展开仍能回到同一宽度。
            const int index = m_ui.sidebarSplitter->indexOf(m_ui.sidebar);
            if (index >= 0) {
                const int section = m_ui.sidebarSplitter->sizes().value(index, 0);
                if (section > 0)
                    m_sidebarLayoutWidth = section;
            }
            if (m_sidebarLayoutWidth <= 0)
                m_sidebarLayoutWidth = 300;
            // QSplitter 没有 removeWidget：改父对象即从 splitter 子控件中摘除。
            m_ui.sidebar->setParent(m_ui.centralWidget);
        } else {
            if (m_ui.sidebarSplitter->indexOf(m_ui.sidebar) < 0)
                m_ui.sidebarSplitter->insertWidget(0, m_ui.sidebar);
            const int total = m_ui.sidebarSplitter->width();
            if (total > 0) {
                const int minWidth = m_ui.sidebar->minimumWidth();
                const int maxWidth = qMax(minWidth, total - 320);
                const int width = qBound(minWidth, m_sidebarLayoutWidth, maxWidth);
                m_ui.sidebarSplitter->setSizes(QList<int>{width, total - width});
            }
        }
    } else if (floating) {
        // 浮层：从布局里摘出来，挂到 centralwidget 上，由定位函数贴边
        m_ui.rootLayout->removeWidget(m_ui.sidebar);
        m_ui.sidebar->setParent(m_ui.centralWidget);
    } else {
        // 占布局：插回原来的位置（布局最左）
        m_ui.rootLayout->insertWidget(0, m_ui.sidebar);
    }

    // QSS 用 [floating="true"] 区分两种底色：浮层压画面要更实，占布局保持淡玻璃
    m_ui.sidebar->setProperty("floating", floating);
    if (QStyle *style = m_ui.sidebar->style()) {
        style->unpolish(m_ui.sidebar);
        style->polish(m_ui.sidebar);
    }
    m_ui.sidebar->update();

    positionSidebarOverlay();
}

/// @brief 底部控制条在“浮层”和“布局占位”之间切换。
/// @param floating true 挂到 centralWidget 并贴舞台底边；false 回中心布局舞台下方。
void PanelOverlayController::setControlsFloating(bool floating)
{
    if (floating == m_controlsFloating)
        return;

    m_controlsFloating = floating;

    if (floating) {
        m_ui.centerLayout->removeWidget(m_ui.transport);
        m_ui.transport->setParent(m_ui.centralWidget);
        positionControlOverlay();
    } else {
        // 回布局时放在舞台下方，恢复原来的上下排布
        m_ui.centerLayout->addWidget(m_ui.transport);
    }
}

/// @brief 浮层模式下把侧栏贴到 centralWidget 左上，四周留 16px。
/// @note 宽度取侧栏最小宽度，窗口更窄时才等比缩到可用宽度。
void PanelOverlayController::positionSidebarOverlay()
{
    if (!m_sidebarFloating)
        return;

    const int margin = 16;
    // 侧栏宽度由字体/标签栏算出的最小宽度为准，窗口更窄时才跟着缩。
    const int preferred = qMax(0, m_ui.sidebar->minimumWidth());
    const int width = qMin(preferred, qMax(0, m_ui.centralWidget->width() - margin * 2));
    m_ui.sidebar->setGeometry(margin, margin, width,
                              qMax(0, m_ui.centralWidget->height() - margin * 2));
}

/// @brief 浮层模式下把控制条贴在舞台底部，左右各留 8px。
/// @note 高度由 sizeHint 决定，仅限制在 112..舞台高度之间，防止小窗口盖满画面；
///       同时记录 m_controlsRect 供贴边检测判断鼠标是否仍在控制条上。
void PanelOverlayController::positionControlOverlay()
{
    if (!m_controlsFloating)
        return;

    // 舞台现在挂在中心承载控件里，geometry() 是相对父控件的坐标；
    // 控制条要贴到 centralwidget 上，先把舞台矩形换算到同一坐标系。
    const QRect stage(m_ui.stage->mapTo(m_ui.centralWidget, QPoint(0, 0)),
                      m_ui.stage->size());
    const int margin = 8;
    // 控制条高度完全交给字号撑开的 sizeHint，不再用 150 的硬上限截断；
    // 只限制不超过舞台高度，避免小窗口浮层盖住整幅画面。
    const int maxHeight = qMax(112, stage.height() - margin * 2);
    const int height = qBound(112, m_ui.transport->sizeHint().height(), maxHeight);
    const QRect target(stage.left() + margin, stage.bottom() - height - margin,
                       qMax(0, stage.width() - margin * 2), height);
    m_controlsRect = target;
    m_ui.transport->setGeometry(target);
    m_ui.transport->raise();
}
