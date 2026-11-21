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
class QShowEvent;

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
    void showEvent(QShowEvent *event) override;

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
    /// 让控制条左右两列等宽，保证中间播放键组落在面板真正的中线
    void updateControlBarCentering();
    /// 显示后把窗口夹回当前屏幕可用区；尺寸由窗口预设/用户拖动决定，不再按 150% 特判
    void clampToAvailableScreen();
    /// Qt 的 frameGeometry 不含 Windows 不可见缩放边框，最后用原生外框兜底
    void clampNativeFrameToWorkArea();

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
};

#endif // MAINWINDOW_H
