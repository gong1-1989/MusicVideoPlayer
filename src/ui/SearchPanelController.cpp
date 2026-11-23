/// @file SearchPanelController.cpp
/// @brief 独立搜索窗的界面搭建、联想词、结果筛选分页与链接直达处理。
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
#include "util/HeaderPolicy.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QBoxLayout>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QModelIndex>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QPoint>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
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
        // 用 delegate 自己做 context：delegate 是 view 的子对象，view 销毁时
        // 这个连接会随 delegate 一起断开，不会留下捕获 this 的悬空回调。
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            if (m_view != nullptr)
                advance();
        });
        // 40ms 计时只为悬停标题滚动服务；没有悬停/没有溢出时不应该常驻空转。
        m_timer.stop();

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
                // 只有真正悬停到某一行时才启动滚动计时，移出/切页立刻停。
                if (m_hoveredRow >= 0) {
                    if (!m_timer.isActive())
                        m_timer.start();
                } else {
                    m_timer.stop();
                }
            } else if (event->type() == QEvent::Leave) {
                m_hoveredRow = -1;
                m_timer.stop();
            } else if (event->type() == QEvent::Hide || event->type() == QEvent::HideToParent) {
                // 切到另一个列表页后不再 40ms 空转；重新显示后等 MouseMove 再启动。
                m_timer.stop();
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
        if (row < 0) {
            m_timer.stop();
            return;
        }
        const QModelIndex index = m_view->model() ? m_view->model()->index(row, 0, QModelIndex())
                                                  : QModelIndex();
        if (!index.isValid()) {
            m_timer.stop();
            return;
        }
        const QRect rect = m_view->visualRect(index);
        if (!rect.isValid()) {
            m_timer.stop();
            return;
        }
        // 名称 + 前后两栏的真实宽度，够放就不用动
        const QFontMetrics metrics(m_view->font());
        const QString title = index.data(PlaylistModel::TitleRole).toString();
        const QString artist = index.data(PlaylistModel::ArtistRole).toString();
        const QString text = artist.isEmpty() ? title
                                              : title + QStringLiteral(" - ") + artist;
        // 与 paint() 保持同一套可用宽度：两行布局时来源在下一行，
        // 标题几乎占满整行；单行布局才给右侧来源留位置。
        const QRect inner = rect.adjusted(4, 2, -4, -2);
        const int iconWidth = 22;
        const int gap = 6;
        const int textLeft = inner.left() + iconWidth + gap;
        const int titleAreaWidth = qMax(0, inner.right() - textLeft + 1);
        const int lineHeight = metrics.height();
        const bool twoLine = inner.height() >= lineHeight * 2 + 4;
        int available = titleAreaWidth;
        if (!twoLine) {
            const QString source = m_sourceNames.value(
                index.data(PlaylistModel::SourceRole).toString(), tr("本地"));
            const QString quality = index.data(PlaylistModel::QualityRole).toString();
            const QString sourceText =
                quality.isEmpty() ? source : quality + QStringLiteral(" · ") + source;
            const int sourceWidth =
                qBound(48, metrics.horizontalAdvance(sourceText) + 10,
                       qMax(48, inner.width() / 2));
            available = qMax(0, inner.width() - iconWidth - gap - sourceWidth - gap);
        }
        if (metrics.horizontalAdvance(text) <= available) {
            // 没有溢出就没有滚动需求，停掉计时器，避免列表静止时持续唤醒事件循环。
            m_timer.stop();
            return;
        }

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

QString decodeAttributeEntities(const QString &raw)
{
    QString text = raw;
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    // 网页里的播放地址常写成 &#x2F; 这种数字实体；不还原会让 QUrl 解析出坏路径。
    static const QRegularExpression numeric(
        QStringLiteral("&#(?:[xX]([0-9a-fA-F]{1,6})|([0-9]{1,7}));"));
    QString result;
    int last = 0;
    QRegularExpressionMatchIterator it = numeric.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const uint code = match.captured(1).isEmpty()
                              ? match.captured(2).toUInt(&ok, 10)
                              : match.captured(1).toUInt(&ok, 16);
        const bool valid = ok && code > 0 && code <= 0x10FFFF
                           && !(code >= 0xD800 && code <= 0xDFFF);
        result += text.mid(last, match.capturedStart() - last);
        if (valid) {
            const char32_t point = static_cast<char32_t>(code);
            result += QString::fromUcs4(&point, 1);
        } else {
            result += match.captured(0);
        }
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
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
        const QString candidate = decodeAttributeEntities(match.captured(1))
                                      .replace(QStringLiteral("\\/"), QStringLiteral("/"))
                                      .replace(QStringLiteral("\\u0026"), QStringLiteral("&"));
        const QUrl resolved = base.resolved(QUrl(candidate));
        // 只接受 http/https：网页里可能混入 javascript:/ftp:/data: 地址。
        if (resolved.isValid()
            && (resolved.scheme() == QLatin1String("http")
                || resolved.scheme() == QLatin1String("https"))
            && !resolved.host().isEmpty())
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

/// @brief 构造搜索面板控制器并搭建界面。
/// @param coordinator 搜索协调器，聚合多个搜索源。
/// @param registry 搜索源注册表，用于来源名与凭据。
/// @param http 共享 HTTP 客户端，网页链接解析用。
/// @param ui 搜索窗控件集合。
/// @param parent 父对象。
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

/// @brief 初始化结果列表模型、委托、事件过滤和主题响应。
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
        m_suggestionsSuppressed = true;
        hideSuggestions();
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

                // showResults 已按 URL 去重，状态栏统计必须用去重后的 m_results；
                // 否则“共 N 条”会比列表实际条数多。
                int musicCount = 0;
                int videoCount = 0;
                for (const MediaItem &item : std::as_const(m_results)) {
                    if (item.type == MediaType::Video)
                        ++videoCount;
                    else
                        ++musicCount;
                }

                QString text;
                if (m_results.isEmpty()) {
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

/// @brief 创建类型/来源/品质筛选栏，并接上联想词列表。
/// @note 筛选栏挂在宿主容器的现有布局上，不写死布局类型和插入位置，
///       避免布局调整后筛选栏丢失或跑到窗口底部；联想列表用普通子控件而非
///       QCompleter 顶层 popup，防止独立窗口里 popup 抢焦点吞点击。
void SearchPanelController::setupDiscoveryFeatures()
{
    // 类型/来源/品质筛选：固定放在搜索框正下方的宿主容器里。
    // 不再按根布局索引插入，避免标题/品牌行增删后跑到窗口底部。
    QWidget *filterHost = m_ui.filterHost;
    if (filterHost == nullptr)
        filterHost = m_ui.sidebar;
    if (filterHost != nullptr) {
        // 宿主容器的布局可能是 QVBoxLayout，也可能是 QGridLayout/QHBoxLayout，
        // 因此只按 QLayout 基类能力处理，不写死具体布局类型。
        if (auto *layout = filterHost->layout(); layout != nullptr) {
            auto *filterBar = new QWidget(filterHost);
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
            if (m_ui.filterHost != nullptr) {
                layout->addWidget(filterBar);
            } else if (auto *box = qobject_cast<QBoxLayout *>(layout)) {
                // 只有线性布局能按索引插入；QLayout 基类没有 insertWidget。
                box->insertWidget(qMin(3, box->count()), filterBar);
            } else {
                layout->addWidget(filterBar);
            }

            connect(m_typeFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
            connect(m_sourceFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
            connect(m_qualityFilter, &QComboBox::currentIndexChanged, this,
                    [this] { applyFilters(); });
        }
    }

    // 历史 + 热门 + 当前结果标题组成联想词库。联想列表是搜索窗内的普通子控件，
    // 不使用 QCompleter 的顶层 popup：独立对话框里 popup 会变成最上层窗口，
    // 抢走焦点和鼠标并吞掉点击，用户看到的就是整个搜索窗无响应。
    connect(m_ui.edit, &QLineEdit::textChanged, this, [this] {
        // 用户实际改字时恢复联想；toggleSearch 之后程序不再改文本，
        // 所以结果返回时不会被 rebuildSuggestions() 重新弹出来。
        m_suggestionsSuppressed = false;
        updateSuggestions(false);
    });
    if (m_ui.suggestList != nullptr) {
        connect(m_ui.suggestList, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *item) { applySuggestion(item); });
    }
    if (m_ui.sidebar != nullptr)
        m_ui.sidebar->installEventFilter(this);

    rebuildSuggestions();
    // setupUi 阶段筛选下拉还没创建，源名在这里补一次，避免“来源”框空白。
    refreshSourceNames();
}

/// @brief 用“搜索历史 + 内置热词 + 当前结果标题”重建联想词库（最多 40 条）。
void SearchPanelController::rebuildSuggestions()
{
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
    m_suggestions = words;
    // 打开窗口时是程序聚焦，不弹历史/热门；用户用鼠标点进输入框时才展开。
    updateSuggestions(false);
}

/// @brief 按当前输入前缀过滤联想词并显示，最多 8 条。
/// @param showAll true 时即使输入为空也展示全部词（用于点击输入框展开历史）。
void SearchPanelController::updateSuggestions(bool showAll)
{
    if (m_ui.suggestList == nullptr || m_ui.edit == nullptr)
        return;

    // 搜索已发起/结果已返回：联想层保持关闭，不能挡住结果列表。
    if (m_suggestionsSuppressed) {
        hideSuggestions();
        return;
    }

    const QString prefix = m_ui.edit->text().trimmed();
    if (prefix.isEmpty() && !showAll) {
        hideSuggestions();
        return;
    }

    QStringList matches;
    for (const QString &word : std::as_const(m_suggestions)) {
        if (prefix.isEmpty() || word.contains(prefix, Qt::CaseInsensitive))
            matches.append(word);
        if (matches.size() >= 8)
            break;
    }

    if (matches.isEmpty()) {
        hideSuggestions();
        return;
    }

    m_ui.suggestList->clear();
    for (const QString &word : std::as_const(matches))
        m_ui.suggestList->addItem(word);
    updateSuggestionGeometry();
    m_ui.suggestList->show();
    m_ui.suggestList->raise();
}

/// @brief 把联想列表贴在搜索框下方，并按行数/可用高度限制其高度。
void SearchPanelController::updateSuggestionGeometry()
{
    if (m_ui.suggestList == nullptr || m_ui.edit == nullptr || m_ui.sidebar == nullptr)
        return;

    const QPoint topLeft =
        m_ui.edit->mapTo(m_ui.sidebar, QPoint(0, m_ui.edit->height() + 4));
    int rowHeight = m_ui.suggestList->sizeHintForRow(0);
    if (rowHeight <= 0)
        rowHeight = 34;
    int height = m_ui.suggestList->count() * rowHeight + 12;
    height = qMin(height, 8 * rowHeight + 12);
    height = qMin(height, m_ui.sidebar->height() - topLeft.y() - 8);
    // 搜索框贴近窗口底部时可用高度可能为负；负高度会让列表反向绘制/闪一下。
    height = qMax(0, height);
    if (height <= 0) {
        hideSuggestions();
        return;
    }
    m_ui.suggestList->setGeometry(topLeft.x(), topLeft.y(), m_ui.edit->width(), height);
}

/// @brief 隐藏联想列表。
void SearchPanelController::hideSuggestions()
{
    if (m_ui.suggestList != nullptr)
        m_ui.suggestList->hide();
}

/// @brief 点击联想词：填词并立即发起搜索。
/// @param item 被点击的联想词项。
void SearchPanelController::applySuggestion(QListWidgetItem *item)
{
    if (item == nullptr || m_ui.edit == nullptr)
        return;
    const QString text = item->text();
    hideSuggestions();
    {
        // 填词过程不要再触发一次联想刷新，否则列表会在搜索前闪回来。
        QSignalBlocker blocker(m_ui.edit);
        m_ui.edit->setText(text);
        m_ui.edit->setCursorPosition(text.size());
    }
    toggleSearch();
}

/// @brief 把关键词放到搜索历史最前并重建联想。
/// @param keyword 用户实际搜索的关键词。
void SearchPanelController::rememberKeyword(const QString &keyword)
{
    QStringList history = AppSettings::searchHistory();
    history.removeAll(keyword);
    history.prepend(keyword);
    AppSettings::setSearchHistory(history);
    rebuildSuggestions();
}

/// @brief 按类型/来源/品质过滤结果并重排分页。
/// @param resetPage true 时回到第一页（筛选条件变化时用）。
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

/// @brief 若输入是 http(s) 地址，按直链或网页解析处理。
/// @param text 搜索框内容。
/// @return true 表示已按链接处理，不再走关键词搜索。
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

/// @brief 把音视频直链直接变成一条可播放结果（扩展名决定音乐/影视）。
/// @param url 直链地址。
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
    item.sourceOrigin = HeaderPolicy::originOnly(url);
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

/// @brief 拉取网页并从中提取可播放的音视频地址，生成一条结果。
/// @param url 网页地址。
/// @note 用序号 m_linkParseSerial 丢弃过期回调；响应体受 NetLimits::kHtmlBytes 限制。
void SearchPanelController::handleWebPageLink(const QUrl &url)
{
    if (m_http == nullptr) {
        emit statusMessage(tr("网页链接解析不可用"));
        return;
    }
    emit statusMessage(tr("正在解析网页中的音视频地址…"));

    const int serial = ++m_linkParseSerial;
    QPointer<SearchPanelController> self(this);
    m_http->fetch(
        url, QByteArray(), {},
        [self, url, serial](QNetworkReply *reply) {
            if (self.isNull()) {
                if (reply != nullptr)
                    reply->deleteLater();
                return;
            }
            if (serial != self->m_linkParseSerial) {
                // 已作废的回调仍要释放 reply，否则每次换搜索都会漏一个已完成的响应。
                if (reply != nullptr)
                    reply->deleteLater();
                return;
            }
            if (reply == nullptr) {
                emit self->statusMessage(tr("网页打开失败，检查链接是否可访问"));
                return;
            }
            if (!HttpClient::isHttpSuccess(reply)) {
                const QString error = HttpClient::describeError(reply);
                reply->deleteLater();
                emit self->statusMessage(tr("网页打开失败：%1").arg(error));
                return;
            }
            bool exceeded = false;
            const QByteArray body = HttpClient::readAllLimited(reply,
                                                               NetLimits::kHtmlBytes,
                                                               &exceeded);
            reply->deleteLater();
            if (exceeded) {
                emit self->statusMessage(tr("网页内容过大，未继续解析"));
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
            item.sourceOrigin = HeaderPolicy::originOnly(url);
            item.online = true;
            item.sourceId = QStringLiteral("link");
            item.headers.insert(QStringLiteral("Referer"),
                                HeaderPolicy::refererHeaderValue(url));
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

/// @brief 按搜索进行中/空闲切换搜索与翻页按钮的图标和提示。
/// @param busy 当前是否有搜索在途。
void SearchPanelController::applyButtonIcons(bool busy)
{
    // 搜索键有渐变底色，图标必须强制白色，不能跟随主题前景色。
    UiIcons::apply(m_ui.searchButton, busy ? QStringLiteral("cancel")
                                                  : QStringLiteral("search"),
                   busy ? tr("取消搜索") : tr("搜索"), 18, QColor(Qt::white));
    UiIcons::apply(m_ui.prevButton, QStringLiteral("page-prev"), tr("上一页"), 18);
    UiIcons::apply(m_ui.nextButton, QStringLiteral("page-next"), tr("下一页"), 18);
}

/// @brief 搜索按钮/回车入口：忙时取消，否则按链接或关键词发起搜索。
void SearchPanelController::toggleSearch()
{
    // 从点击搜索/回车开始，联想层就不再参与，直到用户重新编辑关键词。
    m_suggestionsSuppressed = true;
    hideSuggestions();
    // 新的搜索/链接操作会作废上一条网页解析回调。
    ++m_linkParseSerial;
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

    hideSuggestions();
    rememberKeyword(keyword);
    m_coordinator->search(keyword, AppSettings::searchLimit());
}

/// @brief 取消进行中的聚合搜索。
void SearchPanelController::cancelSearch()
{
    if (!m_coordinator->isBusy())
        return;
    m_coordinator->cancel();
    emit statusMessage(tr("搜索已取消"));
}

/// @brief 用源注册表刷新“来源”筛选下拉和结果委托里的源名显示。
/// @note 当前选中的源被删除时退回“全部”，并重新过滤一次列表。
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
    // 当前筛选的源被删除后，下拉会退回“全部”；此时必须重新过滤，
    // 否则列表仍停在只显示已删除源结果的状态。
    if (!previous.isEmpty() && restored < 0)
        applyFilters(false);
}

/// @brief 折叠或展开整个搜索结果区（标题、列表、分页）。
/// @param visible true 展开并重算每页条数。
void SearchPanelController::setAreaVisible(bool visible)
{
    m_ui.titleLabel->setVisible(visible);
    m_ui.list->setVisible(visible);
    m_ui.pager->setVisible(visible);
    if (visible)
        schedulePageSizeRefresh();
}

/// @brief 统一处理搜索框、侧栏和结果视口的事件：联想展开/收起、尺寸变化重算分页。
bool SearchPanelController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_ui.sidebar
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)
        && m_ui.suggestList != nullptr && m_ui.suggestList->isVisible()) {
        updateSuggestionGeometry();
    }

    if (watched == m_ui.edit) {
        if (event->type() == QEvent::FocusIn) {
            // 只有鼠标点进输入框才展开历史/热门；打开窗口时的程序聚焦不弹列表。
            if (static_cast<QFocusEvent *>(event)->reason() == Qt::MouseFocusReason) {
                m_suggestionsSuppressed = false;
                updateSuggestions(true);
            }
        } else if (event->type() == QEvent::MouseButtonPress
                   && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            // 输入框已经持有焦点时再点一次不会再发 FocusIn，这里补一次展开。
            m_suggestionsSuppressed = false;
            updateSuggestions(true);
        } else if (event->type() == QEvent::FocusOut) {
            hideSuggestions();
        } else if (event->type() == QEvent::KeyPress
                   && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            hideSuggestions();
        }
    }
    if (watched == m_ui.list->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::FontChange)) {
        schedulePageSizeRefresh();
    }
    return QObject::eventFilter(watched, event);
}

/// @brief 把连续的尺寸变化合并到下一拍，只重算一次每页条数。
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

/// @brief 按结果视口高度和行高重算每页条数，并保持当前首条所在页。
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

/// @brief 计算过滤后结果的总页数。
/// @return 页数，无结果时为 0。
int SearchPanelController::pageCount() const
{
    if (m_visibleResults.isEmpty() || m_pageSize <= 0)
        return 0;
    return (m_visibleResults.size() + m_pageSize - 1) / m_pageSize;
}

/// @brief 接收一次搜索的原始结果：去重、展开结果区、刷新联想并回到第一页。
/// @param items 各源返回的原始结果，可能含重复条目。
void SearchPanelController::showResults(const QVector<MediaItem> &items)
{
    // 页数、标题和分页切片必须基于去重后的结果；PlaylistModel::setItems()
    // 还会去重，若这里保留原始条数，会出现“共 N 条但列表更少/空页”。
    m_results = dedupeMediaItems(items);
    m_page = 0;
    setAreaVisible(true);
    rebuildSuggestions();
    // rebuildSuggestions() 更新词库但不再主动弹层；这里再明确收一次，
    // 防止结果返回时鼠标恰好仍停在输入框上把联想层重新显示出来。
    hideSuggestions();
    applyFilters();
    schedulePageSizeRefresh();
}

/// @brief 按当前页号把过滤结果切片送入模型，并更新标题、页号和翻页按钮状态。
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

/// @brief 结果列表右键菜单：播放、加入对应类型列表、下载。
/// @param pos 视口内点击位置。
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
    // 类型与目标列表必须匹配：视频条目不能加进音乐列表，反之亦然。
    addMusicAction->setEnabled(item.type == MediaType::Music);
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

/// @brief 双击/回车激活结果项时请求播放。
/// @param index 被激活的模型索引。
void SearchPanelController::handleActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    const MediaItem item = m_model->itemAt(index.row());
    if (item.isValid())
        emit mediaActivated(item);
}
