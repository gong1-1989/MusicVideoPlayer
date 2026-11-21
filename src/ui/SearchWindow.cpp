#include "ui/SearchWindow.h"

#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "ui/SearchPanelController.h"
#include "ui/ThemeManager.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

SearchWindow::SearchWindow(SearchCoordinator *coordinator, SourceRegistry *registry,
                           HttpClient *http, QWidget *parent)
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    setObjectName(QStringLiteral("SearchWindow"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(tr("搜索发现"));
    setWindowModality(Qt::NonModal);
    resize(560, 640);
    setMinimumSize(440, 480);

    buildUi();
    ThemeManager::instance().applyWindowChrome(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { ThemeManager::instance().applyWindowChrome(this); });

    SearchPanelUi ui;
    ui.list = m_list;
    ui.edit = m_edit;
    ui.searchButton = m_searchButton;
    ui.pager = findChild<QWidget *>(QStringLiteral("frame_searchPager"));
    ui.prevButton = findChild<QPushButton *>(QStringLiteral("btn_searchPrev"));
    ui.nextButton = findChild<QPushButton *>(QStringLiteral("btn_searchNext"));
    ui.titleLabel = findChild<QLabel *>(QStringLiteral("label_searchTitle"));
    ui.pageLabel = findChild<QLabel *>(QStringLiteral("label_searchPage"));
    ui.pageInfoLabel = findChild<QLabel *>(QStringLiteral("label_searchPageInfo"));
    // 筛选行由控制器在运行时插到搜索框下方；这里把窗口自身当容器，
    // 布局顺序是品牌头、分割线、搜索行、提示、结果标题、结果列表、分页。
    ui.sidebar = this;
    m_controller = new SearchPanelController(coordinator, registry, http, ui, this);

    // 搜索一开始就把初始提示收起，把高度让给结果列表。
    connect(coordinator, &SearchCoordinator::searchStarted, this,
            [this] { m_searchHint->hide(); });
}

SearchWindow::~SearchWindow() = default;

void SearchWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 16);
    root->setSpacing(12);

    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *brand = new QLabel(tr("NEBULA  SEARCH"), this);
    brand->setObjectName(QStringLiteral("label_searchBrand"));
    header->addWidget(brand);
    header->addStretch(1);
    auto *subtitle = new QLabel(tr("搜索发现"), this);
    subtitle->setObjectName(QStringLiteral("label_searchSubtitle"));
    header->addWidget(subtitle);
    root->addLayout(header);

    auto *line = new QFrame(this);
    line->setObjectName(QStringLiteral("brandLine"));
    line->setFrameShape(QFrame::NoFrame);
    root->addWidget(line);

    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("edit_search"));
    m_edit->setMinimumHeight(44);
    m_edit->setPlaceholderText(tr("搜索歌曲 / 歌手 / 视频，或粘贴播放链接"));
    m_edit->setClearButtonEnabled(true);
    searchRow->addWidget(m_edit, 1);

    m_searchButton = new QPushButton(this);
    m_searchButton->setObjectName(QStringLiteral("btn_search"));
    m_searchButton->setFixedSize(44, 44);
    m_searchButton->setToolTip(tr("搜索"));
    searchRow->addWidget(m_searchButton);
    root->addLayout(searchRow);

    m_searchHint = new QLabel(tr("输入关键词开始搜索，支持历史/热门联想与粘贴链接解析"), this);
    m_searchHint->setObjectName(QStringLiteral("label_searchHint"));
    m_searchHint->setAlignment(Qt::AlignCenter);
    m_searchHint->setWordWrap(true);
    // 结果区未展开时由提示文字吸收剩余高度并垂直居中，避免整块内容被均摊拉散。
    m_searchHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_searchHint->setMinimumHeight(120);
    root->addWidget(m_searchHint);

    auto *title = new QLabel(tr("搜索结果"), this);
    title->setObjectName(QStringLiteral("label_searchTitle"));
    root->addWidget(title);

    m_list = new QListView(this);
    m_list->setObjectName(QStringLiteral("list_search"));
    m_list->setMinimumHeight(220);
    root->addWidget(m_list, 1);

    auto *pager = new QFrame(this);
    pager->setObjectName(QStringLiteral("frame_searchPager"));
    pager->setFrameShape(QFrame::NoFrame);
    pager->setFixedHeight(38);
    auto *pagerLayout = new QHBoxLayout(pager);
    pagerLayout->setContentsMargins(0, 6, 0, 0);
    pagerLayout->setSpacing(8);

    auto *prev = new QPushButton(pager);
    prev->setObjectName(QStringLiteral("btn_searchPrev"));
    prev->setFixedSize(30, 26);
    prev->setToolTip(tr("上一页"));
    pagerLayout->addWidget(prev);

    auto *page = new QLabel(QStringLiteral("0 / 0"), pager);
    page->setObjectName(QStringLiteral("label_searchPage"));
    page->setAlignment(Qt::AlignCenter);
    pagerLayout->addWidget(page);

    auto *next = new QPushButton(pager);
    next->setObjectName(QStringLiteral("btn_searchNext"));
    next->setFixedSize(30, 26);
    next->setToolTip(tr("下一页"));
    pagerLayout->addWidget(next);
    pagerLayout->addStretch(1);

    auto *pageInfo = new QLabel(tr("每页 -- 条"), pager);
    pageInfo->setObjectName(QStringLiteral("label_searchPageInfo"));
    pageInfo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pageInfo->setToolTip(tr("每页条数按列表当前可见高度自动计算，窗口变高会显示更多结果"));
    pagerLayout->addWidget(pageInfo);
    root->addWidget(pager);
}

void SearchWindow::focusSearch()
{
    show();
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
    QTimer::singleShot(0, this, [this] {
        if (m_edit != nullptr)
            m_edit->setFocus(Qt::ShortcutFocusReason);
    });
}
