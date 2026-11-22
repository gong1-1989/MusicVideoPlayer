#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "core/PlaybackController.h"
#include "media/MediaItem.h"

#include <QMainWindow>
#include <QStringList>
#include <QVector>

class PlaylistModel;
class PlaylistController;
class SearchCoordinator;
class SearchPanelController;
class SearchWindow;
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
class QFrame;
class QLabel;
class QTimer;
class MusicVisualWidget;
class HlsCacheBuilder;
class QShortcut;

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
    void on_btn_downloads_clicked();

private:
    void setupConnections();
    void showSearchWindow();
    /// 依据 AppSettings/AppShortcuts 重建全部动态快捷键；设置变化后立即生效
    void rebuildShortcuts();
    void executeShortcutAction(const QString &actionId);
    void updateShortcutTooltips();
    void playSearchItem(const MediaItem &item);
    void addSearchItemToPlaylist(const MediaItem &item, MediaType type);
    void clearPlaybackUi();
    /// 音量按钮改成右侧竖排悬浮条：滑块仍复用 ui->slider_volume，设置持久化不断链
    void setupVolumePopup();
    void showVolumePopup();
    /// 让控制条左右两列等宽，保证中间播放键组落在面板真正的中线
    void updateControlBarCentering();
    /// 左侧栏宽度、标签页高度和底部控制条高度随字体缩放动态调整
    void updateAdaptiveMetrics();
    /// 显示后把窗口夹回当前屏幕可用区；尺寸由窗口预设/用户拖动决定，不再按 150% 特判
    void clampToAvailableScreen();
    /// Qt 的 frameGeometry 不含 Windows 不可见缩放边框，最后用原生外框兜底
    void clampNativeFrameToWorkArea();

    Ui::MainWindow *ui;
    HttpClient *m_http;
    SourceRegistry *m_registry;
    SearchCoordinator *m_coordinator;
    SearchPanelController *m_searchPanel = nullptr;
    SearchWindow *m_searchWindow = nullptr;
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
    QFrame *m_volumePopup = nullptr;
    QLabel *m_volumeLabel = nullptr;
    MusicVisualWidget *m_musicVisual = nullptr;
    HlsCacheBuilder *m_hlsCache = nullptr;
    QVector<QShortcut *> m_shortcuts;
    /// 刚刚从搜索窗口点播的条目身份；启动失败时用它弹一次明确的错误，
    /// 而不是搜索窗关掉后用户只看到“点了没反应”。
    QString m_pendingSearchPlayIdentity;
};

#endif // MAINWINDOW_H
