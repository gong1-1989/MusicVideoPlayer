#include "ui/SearchPanelController.h"

#include "core/PlaylistModel.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "ui/ListMetrics.h"
#include "ui/ThemeManager.h"
#include "ui/UiIcons.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QWidget>

namespace {

/// 搜索结果的三栏绘制：类型图案 | 名称（放不下时悬停滚动） | 来源。
/// 名称与来源各自的可用宽度先算好，来源栏最多占三分之一，免得长站名把名称挤没。
class SearchItemDelegate : public QStyledItemDelegate
{
public:
    explicit SearchItemDelegate(QListView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
        m_timer.setInterval(40);
        QObject::connect(&m_timer, &QTimer::timeout, view->viewport(), [this] { advance(); });
        m_timer.start();

        m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_view->setMouseTracking(true);
        m_view->viewport()->setMouseTracking(true);
        m_view->viewport()->installEventFilter(this);
    }

    /// 源名要跟着"搜索源管理"里的改动刷新
    void setSourceNames(const QHash<QString, QString> &names)
    {
        m_sourceNames = names;
        m_view->viewport()->update();
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == m_view->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
                const QModelIndex index = m_view->indexAt(pos);
                m_hoveredRow = index.isValid() ? index.row() : -1;
            } else if (event->type() == QEvent::Leave) {
                m_hoveredRow = -1;
            } else if (event->type() == QEvent::Hide || event->type() == QEvent::HideToParent) {
                // 切到另一个列表页后不再 40ms 空转，重新显示时再启动
                m_timer.stop();
            } else if (event->type() == QEvent::Show || event->type() == QEvent::ShowToParent) {
                if (!m_timer.isActive())
                    m_timer.start();
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();

        const QWidget *widget = opt.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        // 背景、悬停、选中态完全交给主题画，文字自己按三栏排
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const QPalette::ColorGroup group =
            (option.state & QStyle::State_Enabled) != 0 ? QPalette::Normal : QPalette::Disabled;
        const QColor textColor =
            opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        QColor dimColor = textColor;
        dimColor.setAlpha(160);

        const QRect inner =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -2, 0);
        const QFontMetrics metrics(opt.font);

        const bool isVideo = index.data(PlaylistModel::TypeRole).toInt()
                             == static_cast<int>(MediaType::Video);
        const QString title = index.data(PlaylistModel::TitleRole).toString();
        const QString artist = index.data(PlaylistModel::ArtistRole).toString();
        const QString text = artist.isEmpty() ? title
                                              : title + QStringLiteral(" - ") + artist;
        QString source = m_sourceNames.value(index.data(PlaylistModel::SourceRole).toString());
        if (source.isEmpty())
            source = tr("本地");

        const int iconWidth = 22;
        const int gap = 6;
        const int sourceWidth =
            qBound(48, metrics.horizontalAdvance(source) + 10, qMax(48, inner.width() / 3));
        const QRect iconRect(inner.left(), inner.top(), iconWidth, inner.height());
        const QRect titleRect(iconRect.right() + gap, inner.top(),
                              qMax(0, inner.width() - iconWidth - gap - sourceWidth - gap),
                              inner.height());
        const QRect sourceRect(inner.right() - sourceWidth, inner.top(), sourceWidth,
                               inner.height());

        painter->save();

        // 类型图案：视频三角、音乐音符，颜色取主题强调色
        painter->setPen(opt.palette.color(QPalette::Normal, QPalette::Highlight));
        QFont iconFont = opt.font;
        iconFont.setPointSizeF(qMax(10.0, opt.font.pointSizeF()));
        painter->setFont(iconFont);
        painter->drawText(iconRect, Qt::AlignCenter,
                          isVideo ? QStringLiteral("▶") : QStringLiteral("♪"));

        painter->setFont(opt.font);
        painter->setPen(textColor);
        const bool scrollRow =
            index.row() == m_hoveredRow
            || (m_hoveredRow < 0 && index.row() == m_view->currentIndex().row());
        const int textWidth = metrics.horizontalAdvance(text);
        if (scrollRow && textWidth > titleRect.width()) {
            painter->setClipRect(titleRect);
            const int tailGap = 36;
            const int period = textWidth + tailGap;
            const int offset = m_phase % period;
            painter->drawText(QRect(titleRect.x() - offset, titleRect.y(), textWidth,
                                    titleRect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, text);
            painter->drawText(QRect(titleRect.x() - offset + period, titleRect.y(), textWidth,
                                    titleRect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, text);
        } else {
            painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                              metrics.elidedText(text, Qt::ElideRight, titleRect.width()));
        }

        painter->setPen(dimColor);
        painter->drawText(sourceRect, Qt::AlignVCenter | Qt::AlignRight,
                          metrics.elidedText(source, Qt::ElideMiddle, sourceRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(qMax(hint.height(), 30));
        return hint;
    }

private:
    void advance()
    {
        const int row = m_hoveredRow >= 0 ? m_hoveredRow : m_view->currentIndex().row();
        if (row < 0)
            return;
        const QModelIndex index = m_view->model() ? m_view->model()->index(row, 0, QModelIndex())
                                                  : QModelIndex();
        if (!index.isValid())
            return;
        const QRect rect = m_view->visualRect(index);
        if (!rect.isValid())
            return;
        // 名称 + 前后两栏的真实宽度，够放就不用动
        const QFontMetrics metrics(m_view->font());
        const QString title = index.data(PlaylistModel::TitleRole).toString();
        const QString artist = index.data(PlaylistModel::ArtistRole).toString();
        const QString text = artist.isEmpty() ? title
                                              : title + QStringLiteral(" - ") + artist;
        if (metrics.horizontalAdvance(text) <= rect.width() - 140)
            return;

        m_phase += 2;
        if (m_phase > 100000)
            m_phase = 0;
        m_view->viewport()->update(rect);
    }

    QListView *m_view;
    QTimer m_timer;
    QHash<QString, QString> m_sourceNames;
    int m_hoveredRow = -1;
    int m_phase = 0;
};

} // namespace

SearchPanelController::SearchPanelController(SearchCoordinator *coordinator,
                                             SourceRegistry *registry, const SearchPanelUi &ui,
                                             QObject *parent)
    : QObject(parent)
    , m_coordinator(coordinator)
    , m_registry(registry)
    , m_ui(ui)
{
    Q_ASSERT(m_coordinator != nullptr);
    Q_ASSERT(m_ui.list != nullptr);
    setupUi();
}

void SearchPanelController::setupUi()
{
    m_model = new PlaylistModel(this);
    m_ui.list->setModel(m_model);
    m_ui.list->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *delegate = new SearchItemDelegate(m_ui.list);
    m_delegate = delegate;
    m_ui.list->setItemDelegate(delegate);

    // 每页条数不再是固定配置：由 applyPage/refreshPageSize 按列表实际可见高度计算。
    m_pageSize = 20;

    // 还没搜索过：整块搜索列表先折叠起来，省下高度给播放列表
    setAreaVisible(false);

    m_ui.list->viewport()->installEventFilter(this);
    // 字体缩放会改变行高，主题重新应用后要重新算一页能放几行。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const ThemeInfo &) {
                applyButtonIcons(m_coordinator != nullptr && m_coordinator->isBusy());
                schedulePageSizeRefresh();
            });

    if (m_registry != nullptr) {
        connect(m_registry, &SourceRegistry::changed, this,
                &SearchPanelController::refreshSourceNames);
    }
    refreshSourceNames();
    applyButtonIcons(false);

    connect(m_ui.searchButton, &QPushButton::clicked,
            this, &SearchPanelController::toggleSearch);
    connect(m_ui.edit, &QLineEdit::returnPressed,
            this, &SearchPanelController::toggleSearch);

    connect(m_ui.prevButton, &QPushButton::clicked, this, [this] {
        if (m_page <= 0)
            return;
        --m_page;
        applyPage();
    });
    connect(m_ui.nextButton, &QPushButton::clicked, this, [this] {
        if (m_page + 1 >= pageCount())
            return;
        ++m_page;
        applyPage();
    });

    connect(m_ui.list, &QWidget::customContextMenuRequested,
            this, &SearchPanelController::handleContextMenu);
    connect(m_ui.list, &QAbstractItemView::doubleClicked,
            this, &SearchPanelController::handleActivated);

    connect(m_coordinator, &SearchCoordinator::searchStarted, this, [this] {
        m_ui.searchButton->setEnabled(true);
        applyButtonIcons(true);
        emit statusMessage(tr("搜索中…"));
        // 一发起搜索就把折叠着的搜索列表放出来
        setAreaVisible(true);
    });
    connect(m_coordinator, &SearchCoordinator::searchPartial, this,
            [this](const QVector<MediaItem> &items, int filtered, int done, int total) {
                m_results = items;
                setAreaVisible(true);
                applyPage();
                schedulePageSizeRefresh();
                emit statusMessage(
                    tr("搜索中… 已完成 %1/%2 个源，当前 %3 条%4")
                        .arg(done)
                        .arg(total)
                        .arg(items.size())
                        .arg(filtered > 0 ? tr("（已过滤 %1 条）").arg(filtered) : QString()));
            });
    connect(m_coordinator, &SearchCoordinator::searchSucceeded, this,
            [this](const QVector<MediaItem> &items, int filtered, const QStringList &failed) {
                showResults(items);

                int musicCount = 0;
                int videoCount = 0;
                for (const MediaItem &item : items) {
                    if (item.type == MediaType::Video)
                        ++videoCount;
                    else
                        ++musicCount;
                }

                QString text;
                if (items.isEmpty()) {
                    text = filtered > 0
                               ? tr("未搜索到可播放结果（已过滤 %1 条）").arg(filtered)
                               : tr("未搜索到结果");
                } else {
                    text = tr("搜索完成：音乐 %1 首，视频 %2 个，共 %3 页")
                               .arg(musicCount)
                               .arg(videoCount)
                               .arg(pageCount());
                    if (filtered > 0)
                        text += tr("（已过滤 %1 条）").arg(filtered);
                }
                if (!failed.isEmpty()) {
                    // 状态栏只放前两条，且单条过长时截断，避免把整串技术失败原因铺满
                    QStringList shown;
                    for (int i = 0; i < failed.size() && i < 2; ++i) {
                        QString detail = failed.at(i);
                        if (detail.size() > 90)
                            detail = detail.left(90) + QStringLiteral("…");
                        shown.append(detail);
                    }
                    text += tr("；失败：%1").arg(shown.join(QStringLiteral("、")));
                    if (failed.size() > shown.size())
                        text += tr(" 等 %1 个源").arg(failed.size());
                }
                emit statusMessage(text);
            });
    connect(m_coordinator, &SearchCoordinator::searchFinished, this, [this] {
        m_ui.searchButton->setEnabled(true);
        applyButtonIcons(false);
    });
}

void SearchPanelController::applyButtonIcons(bool busy)
{
    // 搜索键有渐变底色，图标必须强制白色，不能跟随主题前景色。
    UiIcons::apply(m_ui.searchButton, busy ? QStringLiteral("cancel")
                                                  : QStringLiteral("search"),
                   busy ? tr("取消搜索") : tr("搜索"), 18, QColor(Qt::white));
    UiIcons::apply(m_ui.prevButton, QStringLiteral("page-prev"), tr("上一页"), 18);
    UiIcons::apply(m_ui.nextButton, QStringLiteral("page-next"), tr("下一页"), 18);
}

void SearchPanelController::toggleSearch()
{
    if (m_coordinator->isBusy()) {
        cancelSearch();
        return;
    }

    const QString keyword = m_ui.edit->text().trimmed();
    if (keyword.isEmpty()) {
        emit statusMessage(tr("请输入搜索关键词"));
        return;
    }
    m_coordinator->search(keyword, AppSettings::searchLimit());
}

void SearchPanelController::cancelSearch()
{
    if (!m_coordinator->isBusy())
        return;
    m_coordinator->cancel();
    emit statusMessage(tr("搜索已取消"));
}

void SearchPanelController::refreshSourceNames()
{
    QHash<QString, QString> names;
    if (m_registry != nullptr) {
        for (const SourceConfig &config : m_registry->sources())
            names.insert(config.id, config.name);
    }
    if (m_delegate != nullptr)
        static_cast<SearchItemDelegate *>(m_delegate)->setSourceNames(names);
}

void SearchPanelController::setAreaVisible(bool visible)
{
    m_ui.titleLabel->setVisible(visible);
    m_ui.list->setVisible(visible);
    m_ui.pager->setVisible(visible);
    if (visible)
        schedulePageSizeRefresh();
}

bool SearchPanelController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_ui.list->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::FontChange)) {
        schedulePageSizeRefresh();
    }
    return QObject::eventFilter(watched, event);
}

void SearchPanelController::schedulePageSizeRefresh()
{
    // 视图显示/窗口拖动会连续触发 resize，合并到下一拍只算一次。
    const int serial = ++m_pageSizeSerial;
    QTimer::singleShot(0, this, [this, serial] {
        if (serial != m_pageSizeSerial)
            return;
        refreshPageSize();
    });
}

void SearchPanelController::refreshPageSize()
{
    if (m_results.isEmpty() || !m_ui.list->isVisible())
        return;

    int rowHeight = m_ui.list->sizeHintForRow(0);
    if (rowHeight <= 0) {
        // 当前页模型为空时 sizeHintForRow 返回 -1，用字体行高加内边距兜底。
        rowHeight = qMax(30, m_ui.list->fontMetrics().height() + 12);
    }
    const int nextSize = ListMetrics::pageSizeForViewport(
        m_ui.list->viewport()->height(), rowHeight, m_ui.list->spacing());
    if (nextSize == m_pageSize)
        return;

    // 保持当前页第一条不跳走：窗口变高/变矮后重新算它落在第几页。
    const int firstIndex = m_page * m_pageSize;
    m_pageSize = nextSize;
    m_page = firstIndex / m_pageSize;
    applyPage();
}

int SearchPanelController::pageCount() const
{
    if (m_results.isEmpty() || m_pageSize <= 0)
        return 0;
    return (m_results.size() + m_pageSize - 1) / m_pageSize;
}

void SearchPanelController::showResults(const QVector<MediaItem> &items)
{
    // 页数、标题和分页切片必须基于去重后的结果；PlaylistModel::setItems()
    // 还会去重，若这里保留原始条数，会出现“共 N 条但列表更少/空页”。
    m_results = dedupeMediaItems(items);
    m_page = 0;
    setAreaVisible(true);
    applyPage();
    schedulePageSizeRefresh();
}

void SearchPanelController::applyPage()
{
    const int pages = pageCount();
    m_ui.titleLabel->setText(tr("搜索结果（共 %1 条）").arg(m_results.size()));
    if (m_ui.pageInfoLabel != nullptr)
        m_ui.pageInfoLabel->setText(tr("每页 %1 条").arg(m_pageSize));

    if (pages <= 0) {
        m_page = 0;
        m_model->clear();
        m_ui.pageLabel->setText(tr("0 / 0"));
        m_ui.prevButton->setEnabled(false);
        m_ui.nextButton->setEnabled(false);
        return;
    }

    m_page = qBound(0, m_page, pages - 1);
    const int begin = m_page * m_pageSize;
    const int end = qMin(begin + m_pageSize, m_results.size());

    QVector<MediaItem> slice;
    slice.reserve(end - begin);
    for (int i = begin; i < end; ++i)
        slice.append(m_results.at(i));
    m_model->setItems(slice);

    m_ui.pageLabel->setText(tr("%1 / %2").arg(m_page + 1).arg(pages));
    m_ui.prevButton->setEnabled(m_page > 0);
    m_ui.nextButton->setEnabled(m_page + 1 < pages);
}

void SearchPanelController::handleContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_ui.list->indexAt(pos);
    if (!index.isValid())
        return;

    const MediaItem item = m_model->itemAt(index.row());
    if (!item.isValid())
        return;

    QMenu menu(m_ui.list);
    QAction *playAction = menu.addAction(tr("播放"));
    QAction *addMusicAction = menu.addAction(tr("加入音乐列表"));
    QAction *addVideoAction = menu.addAction(tr("加入视频列表"));
    addVideoAction->setEnabled(item.type == MediaType::Video);
    QAction *downloadAction = menu.addAction(tr("下载"));

    QAction *chosen = menu.exec(m_ui.list->viewport()->mapToGlobal(pos));
    if (chosen == nullptr)
        return;

    if (chosen == playAction) {
        emit mediaActivated(item);
    } else if (chosen == addMusicAction) {
        emit addToPlaylistRequested(item, MediaType::Music);
    } else if (chosen == addVideoAction) {
        emit addToPlaylistRequested(item, MediaType::Video);
    } else if (chosen == downloadAction) {
        emit downloadRequested(item);
    }
}

void SearchPanelController::handleActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const MediaItem item = m_model->itemAt(index.row());
    if (item.isValid())
        emit mediaActivated(item);
}
