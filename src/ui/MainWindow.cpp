#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "ui/DownloadController.h"
#include "ui/FullscreenController.h"
#include "ui/LyricsController.h"
#include "ui/MarqueeDelegate.h"
#include "ui/PanelOverlayController.h"
#include "ui/PlaybackUiController.h"
#include "ui/PlaylistController.h"
#include "ui/SearchPanelController.h"
#include "ui/SettingsController.h"
#include "ui/ThemeManager.h"
#include "ui/VideoStage.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QModelIndex>
#include <QResizeEvent>
#include <QShortcut>
#include <QWidget>

Q_LOGGING_CATEGORY(lcWindow, "mvp.window")

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_http(new HttpClient(this))
    , m_registry(new SourceRegistry(this))
    , m_coordinator(nullptr)
    , m_player(new PlaybackController(this))
{
    ui->setupUi(this);

    if (!m_registry->load() || !m_registry->lastError().isEmpty()) {
        const QString message = m_registry->lastError().isEmpty()
                                    ? tr("搜索源配置读取失败，已使用空配置启动。")
                                    : m_registry->lastError();
        QMessageBox::warning(this, tr("搜索源配置"), message);
    }
    m_coordinator = new SearchCoordinator(m_registry, m_http, this);

    // 下载对话框、HLS 后缀处理、任务进度与结果提示交给控制器
    m_downloads = new DownloadController(m_http, this, this);
    connect(m_downloads, &DownloadController::statusMessage, this,
            [this](const QString &text) { ui->label_status->setText(text); });

    // 搜索面板的模型、委托、分页和右键菜单全部由控制器持有
    SearchPanelUi searchUi;
    searchUi.list = ui->list_search;
    searchUi.edit = ui->edit_search;
    searchUi.searchButton = ui->btn_search;
    searchUi.pager = ui->frame_searchPager;
    searchUi.pageSizeCombo = ui->combo_searchPageSize;
    searchUi.prevButton = ui->btn_searchPrev;
    searchUi.nextButton = ui->btn_searchNext;
    searchUi.titleLabel = ui->label_searchTitle;
    searchUi.pageLabel = ui->label_searchPage;
    m_searchPanel = new SearchPanelController(m_coordinator, m_registry, searchUi, this);

    connect(m_searchPanel, &SearchPanelController::statusMessage, this,
            [this](const QString &text) { ui->label_status->setText(text); });
    connect(m_searchPanel, &SearchPanelController::mediaActivated,
            this, &MainWindow::playSearchItem);
    connect(m_searchPanel, &SearchPanelController::addToPlaylistRequested,
            this, &MainWindow::addSearchItemToPlaylist);
    connect(m_searchPanel, &SearchPanelController::downloadRequested, this,
            [this](const MediaItem &item) { m_downloads->download(item); });

    // 播放列表模型、持久化、右键菜单和本地导入统一交给控制器
    PlaylistUi playlistUi;
    playlistUi.musicView = ui->list_music;
    playlistUi.videoView = ui->list_video;
    playlistUi.tabs = ui->tab_media;
    m_playlists = new PlaylistController(m_player, playlistUi, this);
    m_musicModel = m_playlists->musicModel();
    m_videoModel = m_playlists->videoModel();

    connect(m_playlists, &PlaylistController::statusMessage, this,
            [this](const QString &text) { ui->label_status->setText(text); });
    connect(m_playlists, &PlaylistController::playbackInvalidated,
            this, &MainWindow::clearPlaybackUi);

    // 歌词加载、当前行高亮和歌词栏显隐由控制器统一处理
    LyricsUi lyricsUi;
    lyricsUi.panel = ui->panelLyrics;
    lyricsUi.list = ui->list_lrc;
    m_lyrics = new LyricsController(m_http, lyricsUi, this);

    // 全屏窗口与视频面搬运
    m_fullscreen = new FullscreenController(ui->videoWidget, ui->layout_videoStage, 16.0, this);

    // 侧栏 / 控制条的常显与浮层形态、贴边检测和几何定位
    PanelOverlayUi panelUi;
    panelUi.centralWidget = ui->centralwidget;
    panelUi.sidebar = ui->panelSidebar;
    panelUi.stage = ui->panelStage;
    panelUi.transport = ui->panelTransport;
    panelUi.rootLayout = ui->layout_root;
    panelUi.centerLayout = ui->layout_center;
    m_panels = new PanelOverlayController(panelUi, this);

    // 播放条：进度、音量、播放/停止/上下曲、模式按钮与歌词联动
    PlaybackUi playbackUi;
    playbackUi.progress = ui->slider_progress;
    playbackUi.volume = ui->slider_volume;
    playbackUi.time = ui->label_time;
    playbackUi.play = ui->btn_play;
    playbackUi.stop = ui->btn_stop;
    playbackUi.previous = ui->btn_prev;
    playbackUi.next = ui->btn_next;
    playbackUi.mode = ui->btn_mode;
    m_transport = new PlaybackUiController(m_player, m_playlists, ui->videoWidget,
                                           m_lyrics, playbackUi, this);

    // 列表里长名字默认被省略号截断，改成悬停/当前行横向滚动
    ui->list_music->setItemDelegate(new MarqueeDelegate(ui->list_music));
    ui->list_video->setItemDelegate(new MarqueeDelegate(ui->list_video));

    m_player->mediaPlayer()->setVideoSink(ui->videoWidget->sink());

    // 歌词栏默认隐藏，只有播音乐时才让位出来
    m_lyrics->hidePanel();

    // 自绘控件的几何参数：圆角半径与歌词渐隐高度不再散落在各处
    ui->videoStage->setCornerRadius(16.0);
    ui->list_lrc->setFadeExtent(28);

    // 原生标题栏跟着主题走，不要留一条白条
    ThemeManager::instance().applyWindowChrome(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] {
                ThemeManager::instance().applyWindowChrome(this);
                m_lyrics->refreshHighlight();
            });

    auto *settingsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    connect(settingsShortcut, &QShortcut::activated, this, &MainWindow::on_btn_settings_clicked);

    // 设置读写、主题/窗口预设与设置对话框入口交给控制器
    m_settings = new SettingsController(m_player, m_registry, m_http,
                                        SettingsUi{this, ui->slider_volume}, this);
    connect(m_settings, &SettingsController::applied, this, [this] {
        // 侧栏 / 控制条的折叠策略改了要立刻生效；歌词高亮跟随主题刷新
        m_panels->applyVisibility();
        m_lyrics->refreshHighlight();
    });

    setupConnections();
    m_settings->restore();
    m_playlists->restore();
    m_panels->applyVisibility();
}

MainWindow::~MainWindow()
{
    // QVideoSink 属于 ui->videoWidget；先摘掉 sink 再删 ui，
    // 否则播放器析构时仍可能回调已释放的 surface。
    if (m_player != nullptr) {
        m_player->stop();
        if (m_player->mediaPlayer() != nullptr)
            m_player->mediaPlayer()->setVideoSink(nullptr);
    }
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    delete ui;
}

void MainWindow::setupConnections()
{
    connect(m_transport, &PlaybackUiController::statusMessage, this,
            [this](const QString &text) { ui->label_status->setText(text); });
    connect(m_transport, &PlaybackUiController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                if (item.type == MediaType::Music) {
                    ui->list_music->setCurrentIndex(m_musicModel->index(row));
                    ui->tab_media->setCurrentIndex(0);
                } else {
                    ui->list_video->setCurrentIndex(m_videoModel->index(row));
                    ui->tab_media->setCurrentIndex(1);
                }
            });

    connect(ui->list_music, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (!index.isValid())
                    return;
                m_player->setPlaylist(m_musicModel);
                m_player->playRow(index.row());
            });
    connect(ui->list_video, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (!index.isValid())
                    return;
                m_player->setPlaylist(m_videoModel);
                m_player->playRow(index.row());
            });
}

void MainWindow::on_btn_settings_clicked()
{
    m_settings->openDialog();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 全屏是独立顶层窗；不先退出，主窗关闭后进程可能被它拉住不退出。
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    m_player->stop();
    m_playlists->save();
    m_settings->save();
    QMainWindow::closeEvent(event);
    // 关闭主窗后显式结束事件循环：避免独立全屏窗或异步播放对象让进程残留在后台。
    QCoreApplication::quit();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 侧栏、控制条都是浮层、不跟布局走，窗口尺寸一变要自己贴回原位
    m_panels->reposition();
}

void MainWindow::on_btn_fullscreen_clicked()
{
    m_fullscreen->toggle();
}

void MainWindow::playSearchItem(const MediaItem &item)
{
    m_playlists->playItem(item);
}

void MainWindow::addSearchItemToPlaylist(const MediaItem &item, MediaType type)
{
    m_playlists->addSearchItem(item, type);
}

void MainWindow::clearPlaybackUi()
{
    // 播放条自己负责把进度、时间和歌词清零，主窗不再逐个控件复位
    m_transport->clear();
}


