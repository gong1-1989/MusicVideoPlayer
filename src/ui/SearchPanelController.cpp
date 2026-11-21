#include "ui/SearchPanelController.h"

#include "core/PlaylistModel.h"
#include "net/HttpClient.h"
#include "net/NetLimits.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "ui/ListMetrics.h"
#include "ui/ThemeManager.h"
#include "ui/UiIcons.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QModelIndex>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

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

        // QSS 给 item 留了 7px 上下内边距，SE_ItemViewItemText 会把可用高度再扣掉，
        // 150% 字体下永远塞不下第二行。这里直接按整行绘制，只留 2px 内缩，
        // 让“标题 + 品质/来源”两行真正放得下。
        const QRect inner = option.rect.adjusted(4, 2, -4, -2);
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
        const QString quality = index.data(PlaylistModel::QualityRole).toString();
        // 品质放在来源栏左侧，例如“1080P · 优酷”“无损 · 网易云”。
        const QString sourceText =
            quality.isEmpty() ? source : quality + QStringLiteral(" · ") + source;

        const int iconWidth = 22;
        const int gap = 6;
        const int textLeft = inner.left() + iconWidth + gap;
        const int titleAreaWidth = qMax(0, inner.right() - textLeft + 1);
        // 搜索列表只有一列，品质和来源拼在一行会被名称挤成省略号。
        // 行高够时改成“标题一行、品质·来源一行”，来源可完整显示；
        // 窄行则退回原来的单行三栏布局。
        const int lineHeight = metrics.height();
        const bool twoLine = !sourceText.isEmpty() && titleAreaWidth > 0
                             && inner.height() >= lineHeight * 2 + 4;
        QRect iconRect(inner.left(), inner.top(), iconWidth, inner.height());
        QRect titleRect;
        QRect sourceRect;
        if (twoLine) {
            const int blockTop = inner.top() + (inner.height() - lineHeight * 2) / 2;
            titleRect = QRect(textLeft, blockTop, titleAreaWidth, lineHeight);
            sourceRect = QRect(textLeft, blockTop + lineHeight, titleAreaWidth, lineHeight);
        } else {
            const int sourceWidth =
                qBound(48, metrics.horizontalAdvance(sourceText) + 10,
                       qMax(48, inner.width() / 2));
            titleRect = QRect(textLeft, inner.top(),
                              qMax(0, inner.width() - iconWidth - gap - sourceWidth - gap),
                              inner.height());
            sourceRect = QRect(inner.right() - sourceWidth, inner.top(), sourceWidth,
                               inner.height());
        }

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
        painter->drawText(sourceRect,
                          (twoLine ? Qt::AlignVCenter | Qt::AlignLeft
                                   : Qt::AlignVCenter | Qt::AlignRight),
                          metrics.elidedText(sourceText, Qt::ElideRight, sourceRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        // 留出标题 + 品质/来源两行的空间，避免 150% 字体下行高被压成一行。
        const QFontMetrics metrics(option.font);
        hint.setHeight(qMax(hint.height(), metrics.height() * 2 + 10));
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
        if (metrics.horizontalAdvance(text) <= rect.width() - 180)
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

/// 粘贴链接识别的常用媒体后缀：命中就直接当直链，不再发多源搜索。
bool isDirectMediaPath(const QString &path)
{
    static const QStringList suffixes = {
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mkv"),
        QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("m3u8"),
        QStringLiteral("mpd")};
    const QString suffix = path.section(QLatin1Char('.'), -1).toLower();
    return suffixes.contains(suffix);
}

bool looksLikeHttpUrl(const QString &text)
{
    const QUrl url(text);
    return url.isValid() && (url.scheme() == QLatin1String("http")
                             || url.scheme() == QLatin1String("https"))
           && !url.host().isEmpty();
}

QString directMediaTitle(const QUrl &url)
{
    QString name = QUrl::fromPercentEncoding(url.fileName().toUtf8());
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        name = name.left(dot);
    if (name.isEmpty())
        name = QObject::tr("网络直链");
    return name;
}

/// 从网页里找最常见的可播放地址：og:video、<video>/<source>、以及播放器
/// 配置里的 m3u8/mp4 字符串。只做静态解析，不执行网页脚本。
QString extractPageMediaUrl(const QString &html, const QUrl &base)
{
    const QRegularExpression::PatternOptions options =
        QRegularExpression::CaseInsensitiveOption;
    const QStringList patterns = {
        QStringLiteral("property=[\"']og:video(?::url)?[\"'][^>]*content=[\"']([^\"']+)[\"']"),
        QStringLiteral("content=[\"']([^\"']+)[\"'][^>]*property=[\"']og:video(?::url)?[\"']"),
        QStringLiteral("<video[^>]+src=[\"']([^\"']+)[\"']"),
        QStringLiteral("<source[^>]+src=[\"']([^\"']+)[\"']"),
        QStringLiteral("[\"'](?:playUrl|videoUrl|url|src)[\"']\\s*:\\s*[\"']([^\"']+\\.(?:m3u8|mp4|flv|webm)[^\"']*)[\"']"),
    };
    for (const QString &pattern : patterns) {
        const QRegularExpression re(pattern, options);
        const QRegularExpressionMatch match = re.match(html);
        if (!match.hasMatch())
            continue;
        const QString candidate = match.captured(1)
                                      .replace(QStringLiteral("\\/"), QStringLiteral("/"))
                                      .replace(QStringLiteral("\\u0026"), QStringLiteral("&"));
        const QUrl resolved = base.resolved(QUrl(candidate));
        if (resolved.isValid() && !resolved.host().isEmpty())
            return resolved.toString();
    }
    return QString();
}

QString pageTitle(const QString &html)
{
    const QRegularExpression re(QStringLiteral("<title[^>]*>([^<]{1,120})</title>"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(html);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

} // namespace

SearchPanelController::SearchPanelController(SearchCoordinator *coordinator,
                                             SourceRegistry *registry, HttpClient *http,
                                             const SearchPanelUi &ui,
                                             QObject *parent)
    : QObject(parent)
    , m_coordinator(coordinator)
    , m_registry(registry)
    , m_http(http)
    , m_ui(ui)
{
    Q_ASSERT(m_coordinator != nullptr);
    Q_ASSERT(m_ui.list != nullptr);
    setupUi();
    setupDiscoveryFeatures();
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
    m_ui.edit->installEventFilter(this);
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
        if (m_completer != nullptr && m_completer->popup() != nullptr)
            m_completer->popup()->hide();
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
                applyFilters(false);
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

void SearchPanelController::setupDiscoveryFeatures()
{
    // 类型/来源/品质筛选：插在搜索框与“搜索结果”标题之间，
    // 不占搜索结果高度，也让用户搜完能马上收窄范围。
    if (m_ui.sidebar != nullptr) {
        if (auto *layout = qobject_cast<QVBoxLayout *>(m_ui.sidebar->layout());
            layout != nullptr) {
            auto *filterBar = new QWidget(m_ui.sidebar);
            filterBar->setObjectName(QStringLiteral("searchFilterBar"));
            auto *filterLayout = new QGridLayout(filterBar);
            filterLayout->setContentsMargins(0, 2, 0, 2);
            filterLayout->setHorizontalSpacing(6);
            filterLayout->setVerticalSpacing(6);

            auto makeCombo = [filterBar](const QString &name) {
                auto *combo = new QComboBox(filterBar);
                combo->setObjectName(name);
                combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                combo->setMinimumContentsLength(3);
                return combo;
            };
            m_typeFilter = makeCombo(QStringLiteral("combo_searchType"));
            m_typeFilter->addItem(tr("类型:全部"), -1);
            m_typeFilter->addItem(tr("音乐"), int(MediaType::Music));
            m_typeFilter->addItem(tr("影视"), int(MediaType::Video));

            m_sourceFilter = makeCombo(QStringLiteral("combo_searchSource"));
            m_qualityFilter = makeCombo(QStringLiteral("combo_searchQuality"));
            m_qualityFilter->addItem(tr("品质:全部"), QString());
            for (const QString &quality : {QStringLiteral("4K"), QStringLiteral("1080P"),
                                           QStringLiteral("720P"), QStringLiteral("HDR"),
                                           QStringLiteral("无损"), QStringLiteral("HQ"),
                                           QStringLiteral("高清")}) {
                m_qualityFilter->addItem(quality, quality);
            }

            filterLayout->addWidget(m_typeFilter, 0, 0);
            filterLayout->addWidget(m_qualityFilter, 0, 1);
            filterLayout->addWidget(m_sourceFilter, 1, 0, 1, 2);
            filterLayout->setColumnStretch(0, 1);
            filterLayout->setColumnStretch(1, 1);
            layout->insertWidget(3, filterBar);

            connect(m_typeFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
            connect(m_sourceFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
            connect(m_qualityFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
        }
    }

    // 历史 + 热门 + 当前结果标题组成联想词库。QCompleter 自带弹窗保留输入焦点，
    // 比自绘浮层更稳，也不会一弹出就让输入框失焦。
    m_suggestModel = new QStringListModel(this);
    m_completer = new QCompleter(m_suggestModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setMaxVisibleItems(8);
    if (m_completer->popup() != nullptr) {
        m_completer->popup()->setObjectName(QStringLiteral("searchSuggestions"));
        m_completer->popup()->setMinimumWidth(qMax(180, m_ui.edit->width()));
    }
    m_ui.edit->setCompleter(m_completer);
    // QCompleter 的弹出列表会抓鼠标：如果等点击时才因失焦关闭，
    // 那一次点击会被弹窗吞掉，分页/列表要点两下。这里在所有控件上监听鼠标移动，
    // 指针一离开输入框和联想列表就先收起弹窗，下一次点击就能直接落到目标控件。
    qApp->installEventFilter(this);
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &text) {
                m_ui.edit->setText(text);
                m_ui.edit->setCursorPosition(text.size());
                toggleSearch();
            });

    rebuildSuggestions();
    // setupUi 阶段筛选下拉还没创建，源名在这里补一次，避免“来源”框空白。
    refreshSourceNames();
}

void SearchPanelController::rebuildSuggestions()
{
    if (m_suggestModel == nullptr)
        return;

    QStringList words = AppSettings::searchHistory();
    static const QStringList hot = {
        QStringLiteral("周杰伦"), QStringLiteral("经典老歌"), QStringLiteral("华语流行"),
        QStringLiteral("Taylor Swift"), QStringLiteral("演唱会"),
        QStringLiteral("复仇者联盟"), QStringLiteral("流浪地球"), QStringLiteral("4K 电影"),
        QStringLiteral("纪录片"), QStringLiteral("无损音乐")};
    for (const QString &word : hot) {
        if (!words.contains(word))
            words.append(word);
    }
    // 当前结果标题也参与联想：搜过一次后，输入演员/片名片段能直接命中。
    for (const MediaItem &item : std::as_const(m_results)) {
        const QString title = item.title.trimmed();
        if (!title.isEmpty() && title.size() <= 24 && !words.contains(title))
            words.append(title);
        if (words.size() >= 40)
            break;
    }
    m_suggestModel->setStringList(words);
    if (m_completer != nullptr)
        m_completer->setCompletionPrefix(m_ui.edit->text().trimmed());
}

void SearchPanelController::rememberKeyword(const QString &keyword)
{
    QStringList history = AppSettings::searchHistory();
    history.removeAll(keyword);
    history.prepend(keyword);
    AppSettings::setSearchHistory(history);
    rebuildSuggestions();
}

void SearchPanelController::applyFilters(bool resetPage)
{
    if (m_typeFilter == nullptr || m_sourceFilter == nullptr || m_qualityFilter == nullptr) {
        m_visibleResults = m_results;
        if (resetPage)
            m_page = 0;
        applyPage();
        return;
    }

    const int typeValue = m_typeFilter->currentData().toInt();
    const QString sourceId = m_sourceFilter->currentData().toString();
    const QString quality = m_qualityFilter->currentData().toString();

    m_visibleResults.clear();
    for (const MediaItem &item : std::as_const(m_results)) {
        if (typeValue >= 0 && int(item.type) != typeValue)
            continue;
        if (!sourceId.isEmpty() && item.sourceId != sourceId)
            continue;
        if (!quality.isEmpty()
            && !item.quality.contains(quality, Qt::CaseInsensitive))
            continue;
        m_visibleResults.append(item);
    }

    if (resetPage)
        m_page = 0;
    applyPage();
}

bool SearchPanelController::tryHandlePastedLink(const QString &text)
{
    if (!looksLikeHttpUrl(text))
        return false;

    const QUrl url(text);
    if (isDirectMediaPath(url.path())) {
        handleDirectMediaLink(url);
        return true;
    }
    handleWebPageLink(url);
    return true;
}

void SearchPanelController::handleDirectMediaLink(const QUrl &url)
{
    const QString suffix = url.path().section(QLatin1Char('.'), -1).toLower();
    const bool audio = QStringList{QStringLiteral("mp3"), QStringLiteral("m4a"),
                                   QStringLiteral("aac"), QStringLiteral("flac"),
                                   QStringLiteral("wav"), QStringLiteral("ogg"),
                                   QStringLiteral("opus")}
                           .contains(suffix);

    MediaItem item;
    item.type = audio ? MediaType::Music : MediaType::Video;
    item.title = directMediaTitle(url);
    item.url = url;
    item.online = true;
    item.sourceId = QStringLiteral("link");
    item.quality = inferQuality(item.title + QLatin1Char(' ') + url.toString(), item.type);
    if (item.quality.isEmpty())
        item.quality = tr("标准");

    m_results = {item};
    m_page = 0;
    setAreaVisible(true);
    applyFilters();
    schedulePageSizeRefresh();
    emit statusMessage(tr("已识别为%1直链：%2（双击播放或右键下载）")
                           .arg(audio ? tr("音乐") : tr("影视"), item.title));
}

void SearchPanelController::handleWebPageLink(const QUrl &url)
{
    if (m_http == nullptr) {
        emit statusMessage(tr("网页链接解析不可用"));
        return;
    }
    emit statusMessage(tr("正在解析网页中的音视频地址…"));

    QPointer<SearchPanelController> self(this);
    m_http->fetch(
        url, QByteArray(), {},
        [self, url](QNetworkReply *reply) {
            if (self.isNull())
                return;
            if (reply == nullptr) {
                emit self->statusMessage(tr("网页打开失败，检查链接是否可访问"));
                return;
            }
            bool exceeded = false;
            const QByteArray body = HttpClient::readAllLimited(reply,
                                                               NetLimits::kHtmlBytes,
                                                               &exceeded);
            if (exceeded) {
                emit self->statusMessage(tr("网页内容过大，未继续解析"));
                return;
            }
            if (!HttpClient::isHttpSuccess(reply)) {
                emit self->statusMessage(
                    tr("网页打开失败：%1").arg(HttpClient::describeError(reply)));
                return;
            }

            const QString html = QString::fromUtf8(body);
            const QString media = extractPageMediaUrl(html, url);
            if (media.isEmpty()) {
                emit self->statusMessage(
                    tr("没有在网页里找到可直接播放的地址，可改用直链或搜索关键词"));
                return;
            }

            const QUrl mediaUrl(media);
            MediaItem item;
            item.type = MediaType::Video;
            item.title = pageTitle(html);
            if (item.title.isEmpty())
                item.title = directMediaTitle(mediaUrl);
            item.url = mediaUrl;
            item.webUrl = url;
            item.online = true;
            item.sourceId = QStringLiteral("link");
            item.headers.insert(QStringLiteral("Referer"), url.toString());
            item.quality = inferQuality(item.title + QLatin1Char(' ') + media, item.type);
            if (item.quality.isEmpty())
                item.quality = tr("标准");

            self->m_results = {item};
            self->m_page = 0;
            self->setAreaVisible(true);
            self->applyFilters();
            self->schedulePageSizeRefresh();
            emit self->statusMessage(tr("已从网页解析到播放地址：%1，双击播放")
                                         .arg(item.title));
        },
        NetLimits::kHtmlBytes);
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
    if (m_completer != nullptr && m_completer->popup() != nullptr)
        m_completer->popup()->hide();
    if (m_coordinator->isBusy()) {
        cancelSearch();
        return;
    }

    const QString keyword = m_ui.edit->text().trimmed();
    if (keyword.isEmpty()) {
        emit statusMessage(tr("请输入搜索关键词"));
        return;
    }

    // 粘贴的是网页/媒体直链时不再走多源关键词搜索，直接解析成一条可播放结果。
    if (tryHandlePastedLink(keyword))
        return;

    if (m_completer != nullptr && m_completer->popup() != nullptr)
        m_completer->popup()->hide();
    rememberKeyword(keyword);
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
    names.insert(QStringLiteral("link"), tr("直链/网页"));
    if (m_delegate != nullptr)
        static_cast<SearchItemDelegate *>(m_delegate)->setSourceNames(names);

    if (m_sourceFilter == nullptr)
        return;
    const QString previous = m_sourceFilter->currentData().toString();
    QSignalBlocker blocker(m_sourceFilter);
    m_sourceFilter->clear();
    m_sourceFilter->addItem(tr("来源:全部"), QString());
    if (m_registry != nullptr) {
        for (const SourceConfig &config : m_registry->sources())
            m_sourceFilter->addItem(config.name, config.id);
    }
    m_sourceFilter->addItem(tr("直链/网页"), QStringLiteral("link"));
    const int restored = m_sourceFilter->findData(previous);
    m_sourceFilter->setCurrentIndex(restored >= 0 ? restored : 0);
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
    if (event->type() == QEvent::MouseMove && m_completer != nullptr
        && m_completer->popup() != nullptr && m_completer->popup()->isVisible()) {
        const QPoint globalPos = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
        const QRect popupRect(m_completer->popup()->mapToGlobal(QPoint(0, 0)),
                             m_completer->popup()->size());
        const QRect editRect(m_ui.edit->mapToGlobal(QPoint(0, 0)), m_ui.edit->size());
        if (!popupRect.contains(globalPos) && !editRect.contains(globalPos))
            m_completer->popup()->hide();
    }

    if (watched == m_ui.edit) {
        if (event->type() == QEvent::FocusIn && m_completer != nullptr) {
            // 空输入也展示历史+热门，用户点一下就能选，不必先打字。
            QTimer::singleShot(0, this, [this] {
                if (m_ui.edit->hasFocus() && m_completer != nullptr) {
                    m_completer->setCompletionPrefix(m_ui.edit->text());
                    m_completer->complete();
                }
            });
        } else if (event->type() == QEvent::FocusOut && m_completer != nullptr
                   && m_completer->popup() != nullptr) {
            // 点分页/结果列表时先收起联想，避免 QCompleter 在失焦时把高亮项
            // 当成用户选择，出现“点下一页却换了搜索词”的误触。
            m_completer->popup()->hide();
        } else if (event->type() == QEvent::KeyPress
                   && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape
                   && m_completer != nullptr && m_completer->popup() != nullptr) {
            m_completer->popup()->hide();
        }
    }
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
    if (m_visibleResults.isEmpty() || m_pageSize <= 0)
        return 0;
    return (m_visibleResults.size() + m_pageSize - 1) / m_pageSize;
}

void SearchPanelController::showResults(const QVector<MediaItem> &items)
{
    // 页数、标题和分页切片必须基于去重后的结果；PlaylistModel::setItems()
    // 还会去重，若这里保留原始条数，会出现“共 N 条但列表更少/空页”。
    m_results = dedupeMediaItems(items);
    m_page = 0;
    setAreaVisible(true);
    rebuildSuggestions();
    applyFilters();
    schedulePageSizeRefresh();
}

void SearchPanelController::applyPage()
{
    const int pages = pageCount();
    if (m_visibleResults.size() == m_results.size()) {
        m_ui.titleLabel->setText(tr("搜索结果（共 %1 条）").arg(m_results.size()));
    } else {
        m_ui.titleLabel->setText(
            tr("搜索结果（显示 %1 / 共 %2 条）")
                .arg(m_visibleResults.size())
                .arg(m_results.size()));
    }
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
    const int end = qMin(begin + m_pageSize, m_visibleResults.size());

    QVector<MediaItem> slice;
    slice.reserve(end - begin);
    for (int i = begin; i < end; ++i)
        slice.append(m_visibleResults.at(i));
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
