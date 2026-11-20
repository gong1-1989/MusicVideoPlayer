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

    void playSearchItem(const MediaItem &item);
    void addSearchItemToPlaylist(const MediaItem &item, MediaType type);
    void clearPlaybackUi();

    void refreshModeButton();

    Ui::MainWindow *ui;
    HttpClient *m_http;
    SourceRegistry *m_registry;
    SearchCoordinator *m_coordinator;
    SearchPanelController *m_searchPanel = nullptr;
    FullscreenController *m_fullscreen = nullptr;
    PanelOverlayController *m_panels = nullptr;
    LyricsController *m_lyrics = nullptr;
    DownloadController *m_downloads = nullptr;
    PlaybackController *m_player;
    PlaylistController *m_playlists = nullptr;
    PlaylistModel *m_musicModel = nullptr;
    PlaylistModel *m_videoModel = nullptr;

    bool m_userSeeking = false;
    QString m_lastDir;
};

#endif // MAINWINDOW_H
