/// @file MainWindow.h
/// @brief 主窗口声明：装配各控制器、承载播放列表/视觉区/控制条，并管理搜索窗与全屏。
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
class MediaPageResolver;
class QCloseEvent;
class QResizeEvent;
class QShowEvent;
class QFrame;
class QLabel;
class MusicVisualWidget;
class HlsCacheBuilder;
class QShortcut;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

/// @brief 应用主窗口。
/// @note 本类只做装配与转发，播放、列表、搜索、下载、全屏等具体行为分别委托给
///       对应控制器；窗口自身负责几何约束、退出确认与快捷键分发。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    /// @brief 关闭前处理下载未完成、保存播放列表与设置。
    void closeEvent(QCloseEvent *event) override;
    /// @brief 窗口尺寸变化时重算自适应度量与面板定位。
    void resizeEvent(QResizeEvent *event) override;
    /// @brief 拦截控制条/音量弹层等控件的事件，用于浮动与隐藏逻辑。
    bool eventFilter(QObject *watched, QEvent *event) override;
    /// @brief 首次显示后夹回屏幕可用区，并按需恢复上次几何。
    void showEvent(QShowEvent *event) override;

private slots:
    void on_btn_fullscreen_clicked();
    void on_btn_settings_clicked();
    void on_btn_downloads_clicked();

private:
    /// @brief 连接播放器、列表、界面控件之间的信号槽。
    void setupConnections();
    /// @brief 打开（或前置）独立搜索窗口。
    void showSearchWindow();
    /// 依据 AppSettings/AppShortcuts 重建全部动态快捷键；设置变化后立即生效
    void rebuildShortcuts();
    /// @brief 按动作 ID 执行一次快捷键动作。
    void executeShortcutAction(const QString &actionId);
    /// @brief 把当前快捷键映射写进按钮/菜单的提示文本。
    void updateShortcutTooltips();
    /// @brief 播放搜索结果条目。
    /// @return 成功发起播放返回 true；失败时界面已给出提示。
    bool playSearchItem(const MediaItem &item);
    /// @brief 把搜索结果加入对应类型播放列表；多集剧整组传入一次写盘。
    void addSearchItemsToPlaylist(const QVector<MediaItem> &items, MediaType type);
    /// @brief 下载前先解析网页播放页，避免把 HTML 播放页当媒体文件下载。
    void startDownload(const MediaItem &item);
    /// @brief 清空播放相关界面（标题、进度、视觉区）。
    void clearPlaybackUi();
    /// 音量按钮改成右侧竖排悬浮条：slider_volume 是全局唯一音量入口
    void setupVolumePopup();
    /// @brief 在音量按钮上方弹出竖排滑块，并保持当前音量值。
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
    MediaPageResolver *m_pageResolver;
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
    /// Qt::Popup 被按钮点击自动关闭后，clicked 会紧接着触发；用它避免刚关又重开。
    qint64 m_volumePopupHideMs = 0;
    QLabel *m_volumeLabel = nullptr;
    MusicVisualWidget *m_musicVisual = nullptr;
    HlsCacheBuilder *m_hlsCache = nullptr;
    QVector<QShortcut *> m_shortcuts;
    /// 刚刚从搜索窗口点播的条目身份；启动失败时用它弹一次明确的错误，
    /// 而不是搜索窗关掉后用户只看到“点了没反应”。
    QString m_pendingSearchPlayIdentity;
};

#endif // MAINWINDOW_H
