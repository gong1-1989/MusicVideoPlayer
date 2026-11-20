#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "core/PlaybackController.h"
#include "media/MediaItem.h"

#include <QMainWindow>
#include <QStringList>

class PlaylistModel;
class PlaylistController;
class SearchCoordinator;
class SearchPanelController;
class FullscreenController;
class PanelOverlayController;
class LyricsController;
class DownloadController;
class PlaybackUiController;
class SettingsController;
class SourceRegistry;
class HttpClient;
class QCloseEvent;
class QResizeEvent;

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

private slots:
    void on_btn_fullscreen_clicked();
    void on_btn_settings_clicked();

private:
    void setupConnections();
    /// 搜索/下载类提示单独放一个标签，避免反复覆盖播放与恢复状态
    void setNetworkStatus(const QString &text);
    void playSearchItem(const MediaItem &item);
    void addSearchItemToPlaylist(const MediaItem &item, MediaType type);
    void clearPlaybackUi();
    /// 窄窗口下压缩控制条：收窄间距与按钮，避免“列表循环 / 全屏 / 音量”互相压字
    void updateControlBarLayout();

    Ui::MainWindow *ui;
    HttpClient *m_http;
    SourceRegistry *m_registry;
    SearchCoordinator *m_coordinator;
    SearchPanelController *m_searchPanel = nullptr;
    FullscreenController *m_fullscreen = nullptr;
    PanelOverlayController *m_panels = nullptr;
    LyricsController *m_lyrics = nullptr;
    DownloadController *m_downloads = nullptr;
    PlaybackUiController *m_transport = nullptr;
    SettingsController *m_settings = nullptr;
    PlaybackController *m_player;
    PlaylistController *m_playlists = nullptr;
    PlaylistModel *m_musicModel = nullptr;
    PlaylistModel *m_videoModel = nullptr;
    bool m_compactControls = false;
};

#endif // MAINWINDOW_H
