#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "core/PlaybackController.h"
#include "lyrics/LrcParser.h"
#include "media/MediaItem.h"

#include <QMainWindow>
#include <QHash>
#include <QPoint>
#include <QPointer>
#include <QStringList>
#include <QVector>

class PlaylistModel;
class SearchCoordinator;
class SourceRegistry;
class LyricsService;
class HttpClient;
class DownloadTask;
class QListView;
class QCloseEvent;
class QResizeEvent;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_btn_search_clicked();
    void openLocalMusic();
    void openLocalVideo();
    void on_btn_prev_clicked();
    void on_btn_play_clicked();
    void on_btn_stop_clicked();
    void on_btn_next_clicked();
    void on_btn_mode_clicked();
    void on_btn_fullscreen_clicked();
    void on_btn_settings_clicked();

private:
    void setupConnections();
    void restoreSettings();
    void saveSettings();
    void applySettings();
    void applyWindowPreset();
    void highlightCurrentLyric();

    void addLocalFiles(MediaType type, const QStringList &files);
    void playSearchItem(const MediaItem &item);
    void downloadItem(const MediaItem &item);
    void showSearchContextMenu(const QPoint &pos);

    /// 搜索结果分页：整批结果留在内存，分页只替换模型里的那一段切片
    void showSearchResults(const QVector<MediaItem> &items);
    void applySearchPage();
    int searchPageCount() const;
    void setSearchAreaVisible(bool visible);
    QHash<QString, QString> sourceDisplayNames() const;

    void restorePlaylists();
    bool savePlaylists();
    void setupPlaylistInteractions();    void removePlaylistRow(MediaType type, int row);
    void clearPlaylist(MediaType type);
    void showPlaylistContextMenu(MediaType type, QListView *view, PlaylistModel *model,
                                 const QPoint &pos);

    void toggleFullScreen();
    void enterFullScreen();
    void exitFullScreen();
    void updateEdgePanels();
    void positionControlOverlay();
    void positionSidebarOverlay();
    void applyPanelVisibility();
    void setSidebarFloating(bool floating);
    void setControlsFloating(bool floating);
    void updateLyrics(qint64 positionMs);
    void refreshModeButton();
    void ensurePlaylist();

    PlaylistModel *activeModel() const;
    QListView *activeView() const;

    Ui::MainWindow *ui;
    HttpClient *m_http;
    SourceRegistry *m_registry;
    SearchCoordinator *m_coordinator;
    LyricsService *m_lyrics;
    PlaybackController *m_player;
    PlaylistModel *m_musicModel;
    PlaylistModel *m_videoModel;
    PlaylistModel *m_searchModel;

    /// 搜索结果的完整列表与当前页：模型里只有当前页的切片
    QVector<MediaItem> m_searchResults;
    int m_searchPage = 0;
    int m_searchPageSize = 20;
    bool m_searchAreaVisible = false;

    QVector<LrcLine> m_lrcLines;
    int m_lrcIndex = -1;
    bool m_userSeeking = false;
    QString m_lastDir;
    QPointer<DownloadTask> m_download;
    QWidget *m_fullScreenWindow = nullptr;

    /// 侧栏 / 控制条两种形态：收起时做成浮层（贴边弹出、不占布局），常显时回布局占位
    QTimer *m_edgeTimer = nullptr;
    bool m_sidebarPeek = false;
    bool m_sidebarAutoHide = true;
    bool m_controlsAutoHide = true;
    bool m_sidebarFloating = false;
    bool m_controlsFloating = false;
};

#endif // MAINWINDOW_H
