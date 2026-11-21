#include "ui/SearchWindow.h"

#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "ui/SearchPanelController.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
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
    // 独立窗口默认拿到一个空白系统图标，和主窗口标题栏不一致；显式继承应用图标。
    setWindowIcon(QApplication::windowIcon());
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
    ui.suggestList = m_suggestList;
    ui.searchButton = m_searchButton;
    ui.pager = findChild<QWidget *>(QStringLiteral("frame_searchPager"));
    ui.prevButton = findChild<QPushButton *>(QStringLiteral("btn_searchPrev"));
    ui.nextButton = findChild<QPushButton *>(QStringLiteral("btn_searchNext"));
    ui.titleLabel = findChild<QLabel *>(QStringLiteral("label_searchTitle"));
    ui.pageLabel = findChild<QLabel *>(QStringLiteral("label_searchPage"));
    ui.pageInfoLabel = findChild<QLabel *>(QStringLiteral("label_searchPageInfo"));
    ui.filterHost = m_filterHost;
    // 布局顺序：搜索行、筛选行、提示、结果标题、结果列表、状态、分页。
    ui.sidebar = this;
    m_controller = new SearchPanelController(coordinator, registry, http, ui, this);

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

SearchWindow::~SearchWindow() = default;

void SearchWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 16);
    root->setSpacing(12);

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

    // 筛选行固定宿主：控制器只往这里加控件，不再依赖根布局索引。
    m_filterHost = new QWidget(this);
    m_filterHost->setObjectName(QStringLiteral("searchFilterHost"));
    auto *filterHostLayout = new QVBoxLayout(m_filterHost);
    filterHostLayout->setContentsMargins(0, 0, 0, 0);
    filterHostLayout->setSpacing(0);
    root->addWidget(m_filterHost);

    // 联想列表是窗口内的普通子控件，不用 QCompleter 的顶层 popup。
    // 顶层 popup 在独立对话框中会抢焦点和鼠标，表现为整个窗口点不动。
    m_suggestList = new QListWidget(this);
    m_suggestList->setObjectName(QStringLiteral("list_searchSuggestions"));
    m_suggestList->setFocusPolicy(Qt::NoFocus);
    m_suggestList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_suggestList->setUniformItemSizes(true);
    m_suggestList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_suggestList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_suggestList->hide();

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

    // 搜索进度和错误原来显示在主窗口进度条上方；搜索已经是独立窗口后，
    // 这些信息应在搜索窗内就地反馈，主窗口只保留播放状态。
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("label_searchStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_statusLabel->hide();
    root->addWidget(m_statusLabel);

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
