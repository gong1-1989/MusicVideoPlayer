#ifndef SEARCHPANELCONTROLLER_H
#define SEARCHPANELCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QStringList>
#include <QVector>

class PlaylistModel;
class SearchCoordinator;
class SourceRegistry;
class HttpClient;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QPoint;
class QPushButton;
class QStyledItemDelegate;
class QUrl;
class QWidget;

/// 搜索面板依赖的控件集合：只暴露搜索区自己的控件，不把整个 MainWindow.ui 传进来。
struct SearchPanelUi
{
    QListView *list = nullptr;
    QLineEdit *edit = nullptr;
    QPushButton *searchButton = nullptr;
    QWidget *pager = nullptr;
    QPushButton *prevButton = nullptr;
    QPushButton *nextButton = nullptr;
    QLabel *titleLabel = nullptr;
    QLabel *pageLabel = nullptr;
    QLabel *pageInfoLabel = nullptr;
    /// 搜索框下方的历史/热门联想列表，作为搜索窗子控件内嵌显示
    QListWidget *suggestList = nullptr;
    /// 筛选行的固定宿主容器：避免按布局索引插入，增删标题/品牌行时不会错位
    QWidget *filterHost = nullptr;
    /// 左侧搜索栏容器：用于联想列表定位与边缘面板显隐；筛选控件宿主请用 filterHost
    QWidget *sidebar = nullptr;
};

/// 搜索面板：发起/取消搜索、增量结果、分页、结果右键菜单。
/// 播放、加入播放列表、下载仍由 MainWindow 处理，这里只发请求，避免控制器反向依赖窗口。
class SearchPanelController : public QObject
{
    Q_OBJECT

public:
    SearchPanelController(SearchCoordinator *coordinator, SourceRegistry *registry,
                          HttpClient *http, const SearchPanelUi &ui,
                          QObject *parent = nullptr);

public slots:
    void toggleSearch();
    void cancelSearch();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void statusMessage(const QString &text);
    void mediaActivated(const MediaItem &item);
    void addToPlaylistRequested(const MediaItem &item, MediaType type);
    void downloadRequested(const MediaItem &item);

private:
    void setupUi();
    /// 历史/热门联想、类型/来源/品质筛选、粘贴直链识别
    void setupDiscoveryFeatures();
    void rebuildSuggestions();
    void updateSuggestions(bool showAll = false);
    void updateSuggestionGeometry();
    void hideSuggestions();
    void applySuggestion(QListWidgetItem *item);
    void rememberKeyword(const QString &keyword);
    void applyFilters(bool resetPage = true);
    bool tryHandlePastedLink(const QString &text);
    void handleDirectMediaLink(const QUrl &url);
    void handleWebPageLink(const QUrl &url);
    void applyButtonIcons(bool busy);
    void refreshSourceNames();
    void setAreaVisible(bool visible);
    void showResults(const QVector<MediaItem> &items);
    void applyPage();
    void schedulePageSizeRefresh();
    void refreshPageSize();
    int pageCount() const;
    void handleContextMenu(const QPoint &pos);
    void handleActivated(const QModelIndex &index);

    SearchCoordinator *m_coordinator = nullptr;
    SourceRegistry *m_registry = nullptr;
    HttpClient *m_http = nullptr;
    SearchPanelUi m_ui;
    PlaylistModel *m_model = nullptr;
    QStyledItemDelegate *m_delegate = nullptr;
    QComboBox *m_typeFilter = nullptr;
    QComboBox *m_sourceFilter = nullptr;
    QComboBox *m_qualityFilter = nullptr;
    /// 联想候选词库；不再使用 QCompleter 的原生弹出窗口，避免顶层 popup 抢焦点
    QStringList m_suggestions;

    /// 完整结果留在内存，筛选后的可见结果再按页切片
    QVector<MediaItem> m_results;
    QVector<MediaItem> m_visibleResults;
    int m_page = 0;
    /// 每页条数按列表视口高度动态计算，不再从设置/下拉框读取
    int m_pageSize = 20;
    int m_pageSizeSerial = 0;
};

#endif // SEARCHPANELCONTROLLER_H
