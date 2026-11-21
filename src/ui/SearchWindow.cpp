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
    panelUi.pageLabel = ui->label_searchPage;
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

SearchWindow::~SearchWindow()
{
    delete ui;
}

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
