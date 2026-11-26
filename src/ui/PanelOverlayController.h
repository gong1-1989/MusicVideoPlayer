#ifndef PANELOVERLAYCONTROLLER_H
#define PANELOVERLAYCONTROLLER_H

#include <QObject>
#include <QRect>

class QBoxLayout;
class QSplitter;
class QTimer;
class QVBoxLayout;
class QWidget;

/// 侧栏 / 控制条两种形态所需的控件与布局。
struct PanelOverlayUi
{
    QWidget *centralWidget = nullptr;
    QWidget *sidebar = nullptr;
    QWidget *stage = nullptr;
    QWidget *transport = nullptr;
    QBoxLayout *rootLayout = nullptr;
    /// 侧栏与播放区之间的可拖动分隔条；有它时侧栏浮层切换走 splitter，
    /// 不再从根布局 removeWidget/insertWidget。
    QSplitter *sidebarSplitter = nullptr;
    QVBoxLayout *centerLayout = nullptr;
};

/// 侧栏与控制条的“常显占布局 / 收起浮层贴边弹出”状态机。
/// 定时器只做鼠标贴边检测；两侧都常显时会自动停掉。
class PanelOverlayController : public QObject
{
    Q_OBJECT

public:
    explicit PanelOverlayController(const PanelOverlayUi &ui, QObject *parent = nullptr);

    void applyVisibility();
    void reposition();
    /// 全屏窗口接管控制条时暂停主窗口的贴边检测，退出全屏后再恢复
    void setEdgeDetectionEnabled(bool enabled);

private:
    void updateEdgePanels();
    void positionSidebarOverlay();
    void positionControlOverlay();
    void setSidebarFloating(bool floating);
    void setControlsFloating(bool floating);

    PanelOverlayUi m_ui;
    QRect m_controlsRect;
    QTimer *m_edgeTimer = nullptr;
    bool m_sidebarPeek = false;
    bool m_sidebarAutoHide = true;
    bool m_controlsAutoHide = true;
    bool m_sidebarFloating = false;
    bool m_controlsFloating = false;
    /// 常显模式下用户拖出的侧栏宽度；浮层往返时用它复位 splitter。
    int m_sidebarLayoutWidth = 0;
    bool m_edgeDetectionEnabled = true;
};

#endif // PANELOVERLAYCONTROLLER_H
