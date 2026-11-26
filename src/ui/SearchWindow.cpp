/// @file SearchWindow.cpp
/// @brief 独立搜索发现窗口：装配搜索面板控制器、状态提示与主题标题栏。
#include "ui/SearchWindow.h"
#include "ui_SearchWindow.h"

#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "ui/SearchPanelController.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QShowEvent>
#include <QTimer>

/// @brief 构造搜索窗口并创建内部搜索面板控制器。
/// @param coordinator 搜索协调器。
/// @param registry 搜索源注册表。
/// @param http 共享 HTTP 客户端。
/// @param parent 父窗口。
/// @note 窗口是非模态对话框；字体放大后会在下一拍按最小尺寸扩窗，100% 字体保持 560×640。
SearchWindow::SearchWindow(SearchCoordinator *coordinator, SourceRegistry *registry,
                           HttpClient *http, QWidget *parent)
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
    , ui(new Ui::SearchWindow)
{
    ui->setupUi(this);
    m_edit = ui->edit_search;
    m_suggestList = ui->list_searchSuggestions;
    m_filterHost = ui->searchFilterHost;
    m_list = ui->list_search;
    m_searchButton = ui->btn_search;
    m_searchHint = ui->label_searchHint;
    m_statusLabel = ui->label_searchStatus;

    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(tr("搜索发现"));
    // 独立窗口默认拿到一个空白系统图标，和主窗口标题栏不一致；显式继承应用图标。
    setWindowIcon(QApplication::windowIcon());
    setWindowModality(Qt::NonModal);
    ThemeManager::instance().applyWindowChrome(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { ThemeManager::instance().applyWindowChrome(this); });

    SearchPanelUi panelUi;
    panelUi.list = m_list;
    panelUi.edit = m_edit;
    panelUi.suggestList = m_suggestList;
    panelUi.searchButton = m_searchButton;
    panelUi.pager = ui->frame_searchPager;
    panelUi.prevButton = ui->btn_searchPrev;
    panelUi.nextButton = ui->btn_searchNext;
    panelUi.titleLabel = ui->label_searchTitle;
    panelUi.pageEdit = ui->edit_searchPage;
    panelUi.pageInfoLabel = ui->label_searchPageInfo;
    panelUi.filterHost = m_filterHost;
    // 布局顺序：搜索行、筛选行、提示、结果标题、结果列表、状态、分页。
    panelUi.sidebar = this;
    m_controller = new SearchPanelController(coordinator, registry, http, panelUi, this);

    // 字体放大后搜索窗的固定 560x640 会把筛选行/状态行挤出底边；
    // 等筛选行插入完成后按布局最小尺寸兜底，100% 字体下保持原尺寸不变。
    QTimer::singleShot(0, this, [this] {
        const QSize needed = minimumSizeHint();
        setMinimumSize(qMax(440, needed.width()), qMax(480, needed.height()));
        resize(qMax(width(), qMax(560, needed.width())),
               qMax(height(), qMax(640, needed.height())));
    });

    // 搜索一开始就把初始提示收起，把高度让给结果列表。
    connect(coordinator, &SearchCoordinator::searchStarted, this,
            [this] { m_searchHint->hide(); });
}

/// @brief 销毁 UI 对象。
SearchWindow::~SearchWindow()
{
    delete ui;
}

/// @brief 在状态栏显示一条临时提示，8 秒后自动清除；空文本立即隐藏。
void SearchWindow::setStatusMessage(const QString &text)
{
    if (m_statusLabel == nullptr)
        return;

    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
    m_statusLabel->setVisible(!text.isEmpty());

    if (m_statusTimer == nullptr) {
        m_statusTimer = new QTimer(this);
        m_statusTimer->setSingleShot(true);
        connect(m_statusTimer, &QTimer::timeout, this, [this] {
            m_statusLabel->clear();
            m_statusLabel->setToolTip(QString());
            m_statusLabel->hide();
        });
    }
    if (text.isEmpty())
        m_statusTimer->stop();
    else
        m_statusTimer->start(8000);
}

/// @brief 显示窗口并把焦点放到搜索框、全选现有文本。
void SearchWindow::focusSearch()
{    show();
    raise();
    activateWindow();
    QTimer::singleShot(0, this, [this] {
        if (m_edit == nullptr)
            return;
        m_edit->setFocus(Qt::ShortcutFocusReason);
        m_edit->selectAll();
    });
}

/// @brief 显示时重新应用原生标题栏主题，并在事件循环下一拍聚焦搜索框。
void SearchWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 窗口真正显示前 DWM 可能重新套用默认标题栏；显示后再上一次主题色，
    // 保证和主窗口标题栏一致。
    ThemeManager::instance().applyWindowChrome(this);
    QTimer::singleShot(0, this, [this] {
        if (m_edit != nullptr)
            m_edit->setFocus(Qt::ShortcutFocusReason);
    });
}
