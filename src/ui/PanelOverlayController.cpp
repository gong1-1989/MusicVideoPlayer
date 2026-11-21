#include "ui/PanelOverlayController.h"

#include "settings/AppSettings.h"

#include <QBoxLayout>
#include <QCursor>
#include <QPoint>
#include <QRect>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

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

    m_edgeTimer = new QTimer(this);
    m_edgeTimer->setInterval(120);
    connect(m_edgeTimer, &QTimer::timeout, this, &PanelOverlayController::updateEdgePanels);
    m_edgeTimer->start();
}

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

void PanelOverlayController::reposition()
{
    positionSidebarOverlay();
    positionControlOverlay();
}

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

void PanelOverlayController::updateEdgePanels()
{
    if (!m_edgeDetectionEnabled)
        return;
    if (!m_sidebarAutoHide && !m_controlsAutoHide)
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

void PanelOverlayController::setSidebarFloating(bool floating)
{
    if (floating == m_sidebarFloating)
        return;

    m_sidebarFloating = floating;
    m_sidebarPeek = false;

    if (floating) {
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

void PanelOverlayController::positionControlOverlay()
{
    if (!m_controlsFloating)
        return;

    // 舞台是 centralwidget 的直接子控件，控制条挂在同一层，坐标可以直接对齐
    const QRect stage = m_ui.stage->geometry();
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
