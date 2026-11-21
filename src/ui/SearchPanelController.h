#ifndef SEARCHPANELCONTROLLER_H
#define SEARCHPANELCONTROLLER_H

#include "media/MediaItem.h"

#include <QObject>
#include <QVector>

class PlaylistModel;
class SearchCoordinator;
class SourceRegistry;
class QEvent;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPoint;
class QPushButton;
class QStyledItemDelegate;
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
};

/// 搜索面板：发起/取消搜索、增量结果、分页、结果右键菜单。
/// 播放、加入播放列表、下载仍由 MainWindow 处理，这里只发请求，避免控制器反向依赖窗口。
class SearchPanelController : public QObject
{
    Q_OBJECT

public:
    SearchPanelController(SearchCoordinator *coordinator, SourceRegistry *registry,
                          const SearchPanelUi &ui, QObject *parent = nullptr);

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
    SearchPanelUi m_ui;
    PlaylistModel *m_model = nullptr;
    QStyledItemDelegate *m_delegate = nullptr;

    /// 完整结果留在内存，模型里只放当前页切片
    QVector<MediaItem> m_results;
    int m_page = 0;
    /// 每页条数按列表视口高度动态计算，不再从设置/下拉框读取
    int m_pageSize = 20;
    int m_pageSizeSerial = 0;
};

#endif // SEARCHPANELCONTROLLER_H
