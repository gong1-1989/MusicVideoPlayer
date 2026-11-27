/// @file MainWindow.cpp
/// @brief 主窗口装配：控制器创建、界面自适应、快捷键、设置/下载入口与关闭流程。
#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "cache/HlsCacheBuilder.h"
#include "net/HttpClient.h"
#include "net/MediaPageResolver.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "settings/AppShortcuts.h"
#include "settings/AppSettings.h"
#include "ui/DownloadController.h"
#include "ui/FullscreenController.h"
#include "ui/LyricsController.h"
#include "ui/MarqueeDelegate.h"
#include "ui/MusicVisualWidget.h"
#include "ui/PanelOverlayController.h"
#include "ui/PlaybackUiController.h"
#include "ui/PlaylistController.h"
#include "ui/SearchPanelController.h"
#include "ui/SearchWindow.h"
#include "ui/SettingsController.h"
#include "ui/ThemeManager.h"
#include "util/NetworkAddressPolicy.h"
#include "ui/UiIcons.h"
#include "ui/VideoStage.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLabel>
#include <QLayoutItem>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSlider>
#include <QSplitter>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcWindow, "mvp.window")

/// @brief 构造主窗口并装配全部子控制器。
/// @param parent 父窗口。
/// @note 创建顺序有依赖：源注册表先加载，再建搜索协调器、下载控制器和
///       HLS 缓存器；界面控制器在绑定后才恢复播放列表。
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_http(new HttpClient(this))
    , m_pageResolver(new MediaPageResolver(m_http, this))
    , m_registry(new SourceRegistry(this))
    , m_coordinator(nullptr)
    , m_player(new PlaybackController(this))
{
    ui->setupUi(this);
    // 侧栏和播放区之间需要一条用户可拖动的分隔线；必须在其它控制器
    // 取用 layout_center / panelSidebar 之前完成重排。
    setupSidebarSplitter();
    // 播放器只管取流；网页播放页的二次解析交给共享解析器，搜索、播放和下载
    // 用同一份缓存，避免同一集被反复请求播放页。
    m_player->setPageMediaResolver(m_pageResolver);
    // 控制条改成三列：左右等宽、中间播放键固定，播放键组始终落在整条面板中线。
    ui->layout_controls->setColumnStretch(0, 1);
    ui->layout_controls->setColumnStretch(1, 0);
    ui->layout_controls->setColumnStretch(2, 1);
    QTimer::singleShot(0, this, &MainWindow::updateControlBarCentering);

    if (!m_registry->load() || !m_registry->lastError().isEmpty()) {
        const QString message = m_registry->lastError().isEmpty()
                                    ? tr("搜索源配置读取失败，已使用空配置启动。")
                                    : m_registry->lastError();
        QMessageBox::warning(this, tr("搜索源配置"), message);
    }
    m_coordinator = new SearchCoordinator(m_registry, m_http, this);

    // 下载对话框、HLS 后缀处理、任务进度与结果提示交给控制器
    m_downloads = new DownloadController(m_http, this, this);

    // HLS 影视播放时后台把全部分片合并转成 MP4 缓存；它不是用户下载任务，
    // 只跟随当前播放条目，避免和下载队列抢并发/抢同一个目标文件。
    m_hlsCache = new HlsCacheBuilder(m_http, this);
    // 后台缓存要跟随播放器当前实际打开的码率，而不是主列表里最高的码率：
    // 播放器把最近一次视频/音频播放列表 URL 回传给缓存器，由它匹配主列表。
    m_hlsCache->setPlaybackTrackResolvers(
        [this] { return m_player->lastHlsVideoPlaylistUrl(); },
        [this] { return m_player->lastHlsAudioPlaylistUrl(); });
    // 后台整段缓存不再自己直连分片：每个 HLS 分片都登记到播放器同一个流转
    // 代理，已经由播放/预取下载过的片直接复用，未下载的片也由代理统一排队。
    m_hlsCache->setProxyUrlResolver(
        [this](const QUrl &url, const QHash<QString, QString> &headers,
               const QString &cachePath, const QString &cacheIdentity,
               const QUrl &sourceOrigin) {
            return m_player->proxyUrlForHlsCache(url, headers, cachePath, cacheIdentity,
                                                 sourceOrigin);
        });
    // 当前播放仍拿着代理地址时，缓存完成不能立刻删轨道/索引，否则播放会
    // 在 100% 的瞬间退回直连；延迟到切换条目或程序退出再回收。
    m_hlsCache->setRetainCompletedTrackFiles(true);
    connect(m_hlsCache, &HlsCacheBuilder::progressChanged, this,
            [this](const MediaItem &, int percent, const QString &detail) {
                // 只影响状态栏，不碰播放控制；percent 已包含在 detail 文案里。
                Q_UNUSED(percent);
                ui->label_status->setText(detail);
            });
    connect(m_hlsCache, &HlsCacheBuilder::completed, this,
            [this](const MediaItem &item, const QString &) {
                ui->label_status->setText(tr("影视缓存完成：%1").arg(item.displayName()));
            });
    connect(m_hlsCache, &HlsCacheBuilder::failed, this,
            [this](const MediaItem &item, const QString &message) {
                // 加密/直播列表失败是正常能力边界，不弹窗打断播放，只在状态栏说明；
                // 用户仍可继续在线播放。
                ui->label_status->setText(
                    tr("影视缓存未完成：%1（%2）").arg(item.displayName(), message));
            });

    // 播放列表为安全落盘不保存 Cookie；播放/下载前按 sourceId 从源配置
    // 恢复内存态凭据，源配置被更新或删除后也自然跟随。
    const auto credentialResolver = [this](const MediaItem &item) {
        return m_registry->mediaItemWithCredentials(item);
    };
    m_player->setMediaItemResolver(credentialResolver);
    m_downloads->setMediaItemResolver(credentialResolver);
    // 下载入口也跟随当前实际播放线路：主线路 403/分享页、备用 m3u8 已播放时，
    // 不能再用 preferredPlaybackUrl(item) 去下载已经失败的主线路。
    m_downloads->setActiveSourceResolver([this](const MediaItem &item) {
        if (!m_player->hasCurrentItem())
            return QUrl();
        const MediaItem current = m_player->currentItem();
        if (mediaIdentity(current) != mediaIdentity(item))
            return QUrl();
        return m_player->activeUpstreamUrl();
    });

    // 搜索整体从左侧拆成独立窗口：主界面只保留播放列表，
    // 搜索窗口在点搜索结果播放时自动隐藏，由侧栏搜索按钮再次打开。
    m_searchWindow = new SearchWindow(m_coordinator, m_registry, m_http, this);
    m_searchPanel = m_searchWindow->controller();

    // 搜索进度、解析失败和直链识别结果都在搜索窗内显示；
    // 主窗口控制条只保留播放/列表状态，避免两个独立窗口的信息又挤回底部。
    connect(m_searchPanel, &SearchPanelController::statusMessage,
            m_searchWindow, &SearchWindow::setStatusMessage);
    connect(m_searchPanel, &SearchPanelController::mediaActivated, this,
            [this](const MediaItem &item) {
                // 地址无效时不要先把搜索窗藏掉：用户还需要在原结果里重试。
                if (!playSearchItem(item))
                    return;
                if (m_searchWindow != nullptr)
                    m_searchWindow->hide();
                // 搜索窗作为独立顶层窗口关闭后，主动把焦点还给主窗，
                // 避免用户下一次点击侧栏搜索按钮时第一下只起到激活窗口的作用。
                raise();
                activateWindow();
            });
    connect(m_searchPanel, &SearchPanelController::addToPlaylistRequested,
            this, &MainWindow::addSearchItemsToPlaylist);
    connect(m_searchPanel, &SearchPanelController::downloadRequested, this,
            &MainWindow::startDownload);
    connect(m_downloads, &DownloadController::itemDownloaded, this,
            [this](const MediaItem &item, const QString &localPath) {
                m_playlists->updateDownloadedItem(item, localPath);
            });

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
            this, [this] {
                // 先停后台 HLS 缓存再删缓存目录，避免分片下载句柄让删除一直重试。
                if (m_hlsCache != nullptr)
                    m_hlsCache->cancelAndRemove();
                clearPlaybackUi();
            });
    connect(m_playlists, &PlaylistController::downloadRequested, this,
            &MainWindow::startDownload);
    // 视频进度落盘由 PlaylistController 统一防抖处理，这里不再重复连接，
    // 否则一次停止会整份保存两次。
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int) {
                if (!m_pendingSearchPlayIdentity.isEmpty()
                    && mediaIdentity(item) != m_pendingSearchPlayIdentity) {
                    m_pendingSearchPlayIdentity.clear();
                }
            });
    connect(m_player, &PlaybackController::positionChanged, this, [this](qint64 position) {
        // “搜索条目无法播放”只应描述播放从未出声的启动失败。只要当前条目
        // 已经产生非零播放位置，就说明内容确实播起来了，后续网络/解码中断
        // 不能再弹“没有返回可播放内容”的误导提示。
        if (position <= 0 || m_pendingSearchPlayIdentity.isEmpty()
            || !m_player->hasCurrentItem()) {
            return;
        }
        if (mediaIdentity(m_player->currentItem()) == m_pendingSearchPlayIdentity)
            m_pendingSearchPlayIdentity.clear();
    });
    connect(m_player, &PlaybackController::errorOccurred, this,
            [this](const QString &message) {
                if (m_pendingSearchPlayIdentity.isEmpty() || !m_player->hasCurrentItem())
                    return;
                if (mediaIdentity(m_player->currentItem()) != m_pendingSearchPlayIdentity)
                    return;
                m_pendingSearchPlayIdentity.clear();
                // 放到下一轮事件循环：先让播放器完成本次错误处理/自动跳过，
                // 再把原因明确告诉用户，避免模态框卡在信号回调里。
                QTimer::singleShot(0, this, [this, message] {
                    QMessageBox::warning(
                        this, tr("无法播放搜索结果"),
                        tr("这个搜索条目没有返回可播放的内容。\n\n原因：%1\n\n"
                           "可以换一个来源、重新搜索，或稍后再试。").arg(message));
                });
            });

    // 歌词加载与当前行定位只产出数据信号，实际绘制在音乐视觉层完成
    m_lyrics = new LyricsController(m_http, this);

    // 侧栏 / 控制条的常显与浮层形态、贴边检测和几何定位
    PanelOverlayUi panelUi;
    panelUi.centralWidget = ui->centralwidget;
    panelUi.sidebar = ui->panelSidebar;
    panelUi.stage = ui->panelStage;
    panelUi.transport = ui->panelTransport;
    panelUi.rootLayout = ui->layout_root;
    panelUi.sidebarSplitter = m_sidebarSplitter;
    panelUi.centerLayout = m_centerLayout;
    m_panels = new PanelOverlayController(panelUi, this);

    // 全屏窗口：新建独立视频控件并接管播放器视频输出，避免重挂原生视频子窗丢帧
    m_fullscreen = new FullscreenController(ui->videoWidget, m_player->mediaPlayer(),
                                            ui->panelTransport, m_centerLayout,
                                            m_panels, 16.0, this);
    connect(m_fullscreen, &FullscreenController::shortcutTriggered, this,
            &MainWindow::executeShortcutAction);

    // 播放条：进度、音量、播放/停止/上下曲、模式按钮与歌词联动
    PlaybackUi playbackUi;
    playbackUi.progress = ui->slider_progress;
    playbackUi.volume = ui->slider_volume;
    playbackUi.time = ui->label_time;
    playbackUi.play = ui->btn_play;
    playbackUi.stop = ui->btn_stop;
    playbackUi.previous = ui->btn_prev;
    playbackUi.next = ui->btn_next;
    playbackUi.seekBack = ui->btn_seekBack;
    playbackUi.seekForward = ui->btn_seekForward;
    playbackUi.mode = ui->btn_mode;
    m_transport = new PlaybackUiController(m_player, m_playlists, ui->videoWidget,
                                           m_lyrics, playbackUi, this);

    // 列表里长名字默认被省略号截断，改成悬停/当前行横向滚动
    ui->list_music->setItemDelegate(new MarqueeDelegate(ui->list_music));
    ui->list_video->setItemDelegate(new MarqueeDelegate(ui->list_video));

    m_player->mediaPlayer()->setVideoSink(ui->videoWidget->sink());

    // 音乐没有视频帧，原 VideoSurface 会一直黑着。视觉层盖在整个播放舞台上，
    // 播音乐时隐藏视频面、显示动态渐变+频谱；切回视频立刻还原。
    m_musicVisual = new MusicVisualWidget(ui->videoStage);
    m_musicVisual->setGeometry(ui->videoStage->rect());
    m_musicVisual->raise();
    m_musicVisual->hide();
    // 频谱直接取播放器解码后的 PCM，只分析本播放器自身音频。
    m_musicVisual->setMediaPlayer(m_player->mediaPlayer());
    m_musicVisual->setAccentColor(ThemeManager::instance().current().accentColor);
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int) {
                const bool music = item.isValid() && item.type == MediaType::Music;
                if (music) {
                    m_musicVisual->setTrack(item);
                    QString sourceName;
                    if (!item.sourceId.isEmpty() && item.sourceId != QLatin1String("link")) {
                        const SourceConfig config = m_registry->sourceById(item.sourceId);
                        sourceName = config.name;
                    }
                    m_musicVisual->setSourceDisplayName(sourceName);
                    ui->videoWidget->hide();
                    m_musicVisual->setGeometry(ui->videoStage->rect());
                    m_musicVisual->show();
                    m_musicVisual->raise();
                } else {
                    m_musicVisual->hide();
                    ui->videoWidget->show();
                }
                // HLS 影视开始在后台补齐整段缓存。切到音乐/直链视频时不再主动
                // 取消：后台缓存应继续跑完，真正需要让位时由下一次 HLS start()
                // 自己取消上一个条目，避免“切歌即丢缓存”。
                // 必须把播放器实际打开的上游地址交给缓存器：主线路失败换到
                // 备用 m3u8 时，条目原始 URL 仍可能是分享页/失效线路。
                if (m_hlsCache != nullptr && item.type == MediaType::Video && item.online)
                    m_hlsCache->start(item, m_player->activeUpstreamUrl());
            });
    connect(m_player, &PlaybackController::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                if (m_musicVisual != nullptr)
                    m_musicVisual->setPlaying(state == QMediaPlayer::PlayingState);
            });

    // 歌词不再占用右侧独立面板，直接绘制在音乐视觉层的左下区域
    connect(m_lyrics, &LyricsController::linesLoaded, this,
            [this](const QStringList &lines) { m_musicVisual->setLyrics(lines); });
    connect(m_lyrics, &LyricsController::currentIndexChanged,
            m_musicVisual, &MusicVisualWidget::setLyricIndex);
    connect(m_lyrics, &LyricsController::cleared,
            m_musicVisual, &MusicVisualWidget::clearLyrics);

    // 自绘控件的几何参数：圆角半径不再散落在各处
    ui->videoStage->setCornerRadius(16.0);

    // 原生标题栏跟着主题走，不要留一条白条
    ThemeManager::instance().applyWindowChrome(this);
    UiIcons::apply(ui->btn_settings, QStringLiteral("settings"),
                   tr("系统设置 (Ctrl+,)"), 20);
    UiIcons::apply(ui->btn_openSearch, QStringLiteral("search"),
                   tr("搜索发现"), 20);
    UiIcons::apply(ui->btn_fullscreen, QStringLiteral("fullscreen"),
                   tr("全屏 / 退出全屏"), 20);
    UiIcons::apply(ui->btn_downloads, QStringLiteral("downloads"),
                   tr("下载任务"), 20);
    UiIcons::apply(ui->btn_volume, QStringLiteral("volume"), tr("音量"), 20);
    setupVolumePopup();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] {
                ThemeManager::instance().applyWindowChrome(this);
                UiIcons::apply(ui->btn_settings, QStringLiteral("settings"),
                               tr("系统设置 (Ctrl+,)"), 20);
                UiIcons::apply(ui->btn_openSearch, QStringLiteral("search"),
                               tr("搜索发现"), 20);
                UiIcons::apply(ui->btn_fullscreen, QStringLiteral("fullscreen"),
                               tr("全屏 / 退出全屏"), 20);
                UiIcons::apply(ui->btn_downloads, QStringLiteral("downloads"),
                               tr("下载任务"), 20);
                UiIcons::apply(ui->btn_volume, QStringLiteral("volume"), tr("音量"), 20);
                QTimer::singleShot(0, this, &MainWindow::updateControlBarCentering);
                QTimer::singleShot(0, this, &MainWindow::updateAdaptiveMetrics);
                m_lyrics->refreshHighlight();
                if (m_musicVisual != nullptr) {
                    m_musicVisual->setAccentColor(
                        ThemeManager::instance().current().accentColor);
                    m_musicVisual->setUiFontPx(AppSettings::uiFontSizePx());
                }
                updateShortcutTooltips();
            });

    // 设置读写、主题/窗口预设与设置对话框入口交给控制器
    m_settings = new SettingsController(m_player, m_registry, m_http,
                                        SettingsUi{this}, this);
    connect(m_settings, &SettingsController::applied, this, [this] {
        // 侧栏 / 控制条的折叠策略改了要立刻生效；歌词高亮跟随主题刷新
        m_panels->applyVisibility();
        m_lyrics->refreshHighlight();
        rebuildShortcuts();
    });

    setupConnections();
    m_settings->restore();
    m_playlists->restore();
    // 列表恢复后立刻定位上次正在播放的条目为默认选中项；只选中、不自动播放。
    m_playlists->restoreLastSelection();
    // 快捷键也属于设置：启动时按用户绑定重建，不再写死 Ctrl+, / Ctrl+S。
    rebuildShortcuts();
    // 主题/字体缩放已经在 restore 中应用，等样式 polish 完再按实际字体尺寸排版。
    QTimer::singleShot(0, this, &MainWindow::updateAdaptiveMetrics);
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
    // 退出时中止 HLS 后台分片，但保留已下载轨道和续传点；下次播放继续缓存。
    if (m_hlsCache != nullptr)
        m_hlsCache->stopForExit();
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    delete ui;
}

/// @brief 连接各控制器与子窗口信号。
void MainWindow::setupConnections()
{
    connect(ui->btn_openSearch, &QPushButton::clicked,
            this, &MainWindow::showSearchWindow);

    // 播放列表目前是自动保存；显式保存入口（Ctrl+S）由动态快捷键统一创建，
    // 这样用户可以把它改成别的组合键。
    connect(m_transport, &PlaybackUiController::statusMessage, this,
            [this](const QString &text) { ui->label_status->setText(text); });
    connect(m_transport, &PlaybackUiController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                // 视频列表是折叠树：按源行映射到分组子项，并自动展开所属剧集。
                ui->tab_media->setCurrentIndex(item.type == MediaType::Music ? 0 : 1);
                m_playlists->selectSourceRow(item.type, row);
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
                // 剧头只负责展开/收起（单击处理），双击不再触发播放第 1 集。
                if (m_playlists->isGroupIndex(MediaType::Video, index))
                    return;
                const int row = m_playlists->sourceRowForIndex(MediaType::Video, index);
                if (row < 0)
                    return;
                m_player->setPlaylist(m_videoModel);
                m_player->playRow(row);
            });
}

/// @brief 按用户绑定重建全部窗口级快捷键。
/// @note 先清空旧 QShortcut，避免设置页保存后重复触发同一动作。
void MainWindow::rebuildShortcuts()
{
    for (QShortcut *shortcut : m_shortcuts) {
        // rebuild 可能由设置页信号在快捷键派发期间触发；同步 delete 正在
        // 派发的 QShortcut 会悬空。先断开再延迟删除，立即停用旧绑定。
        shortcut->setEnabled(false);
        shortcut->disconnect();
        shortcut->deleteLater();
    }
    m_shortcuts.clear();

    const QHash<QString, QString> bindings = AppShortcuts::currentBindings();
    QSet<QString> used;
    for (const AppShortcuts::Action &action : AppShortcuts::actions()) {
        const QString sequence = AppShortcuts::normalizeSequence(bindings.value(action.id));
        if (sequence.isEmpty())
            continue;
        // 设置对话框已经拦截重复；这里再兜底一次，防止手工改配置文件后
        // 同一个键同时触发多个动作。
        if (used.contains(sequence)) {
            qCWarning(lcWindow) << "快捷键重复，已跳过：" << sequence << action.label;
            continue;
        }
        used.insert(sequence);

        auto *shortcut = new QShortcut(
            QKeySequence::fromString(sequence, QKeySequence::PortableText), this);
        shortcut->setContext(Qt::WindowShortcut);
        const QString actionId = action.id;
        connect(shortcut, &QShortcut::activated, this,
                [this, actionId] { executeShortcutAction(actionId); });
        m_shortcuts.append(shortcut);
    }
    // 全屏窗有独立的 QShortcut 集合；设置页保存后同步重建，避免全屏仍用旧键。
    if (m_fullscreen != nullptr)
        m_fullscreen->refreshShortcuts();
    updateShortcutTooltips();
}

/// @brief 执行一个快捷键动作。
/// @param actionId AppShortcuts 中的稳定动作 id；未知 id 直接忽略。
void MainWindow::executeShortcutAction(const QString &actionId)
{
    if (m_player == nullptr)
        return;

    if (actionId == QLatin1String("playPause")) {
        m_player->togglePlayPause();
    } else if (actionId == QLatin1String("stop")) {
        m_player->stop();
    } else if (actionId == QLatin1String("previous")) {
        m_player->previous();
    } else if (actionId == QLatin1String("next")) {
        m_player->next();
    } else if (actionId == QLatin1String("seekBack")) {
        if (m_transport != nullptr)
            m_transport->seekBySeconds(-10);
    } else if (actionId == QLatin1String("seekForward")) {
        if (m_transport != nullptr)
            m_transport->seekBySeconds(10);
    } else if (actionId == QLatin1String("fullscreen")) {
        if (m_fullscreen != nullptr)
            m_fullscreen->toggle();
    } else if (actionId == QLatin1String("settings")) {
        if (m_settings != nullptr)
            m_settings->openDialog();
    } else if (actionId == QLatin1String("savePlaylist")) {
        if (m_playlists != nullptr)
            m_playlists->saveNow();
    } else if (actionId == QLatin1String("search")) {
        showSearchWindow();
    } else if (actionId == QLatin1String("downloads")) {
        if (m_downloads != nullptr)
            m_downloads->openManager();
    } else if (actionId == QLatin1String("volumeUp")
               || actionId == QLatin1String("volumeDown")) {
        const int step = actionId == QLatin1String("volumeUp") ? 5 : -5;
        const int volume = qBound(0, m_player->volumePercent() + step, 100);
        m_player->setVolumePercent(volume);
        ui->label_status->setText(tr("音量 %1%").arg(volume));
    }
}

/// @brief 按当前绑定刷新按钮悬停提示，让用户看到实际快捷键。
void MainWindow::updateShortcutTooltips()
{
    const QHash<QString, QString> bindings = AppShortcuts::currentBindings();
    auto withKey = [&bindings](const QString &label, const QString &actionId) {
        // 用户保存时可能带空格/大小写差异；提示必须显示实际注册的规范化序列。
        const QString key = AppShortcuts::normalizeSequence(bindings.value(actionId));
        return key.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, key);
    };

    if (ui->btn_settings != nullptr)
        ui->btn_settings->setToolTip(withKey(tr("系统设置"), QStringLiteral("settings")));
    if (ui->btn_openSearch != nullptr)
        ui->btn_openSearch->setToolTip(withKey(tr("搜索发现"), QStringLiteral("search")));
    if (ui->btn_fullscreen != nullptr)
        ui->btn_fullscreen->setToolTip(
            withKey(tr("全屏 / 退出全屏"), QStringLiteral("fullscreen")));
    if (ui->btn_downloads != nullptr)
        ui->btn_downloads->setToolTip(
            withKey(tr("下载任务"), QStringLiteral("downloads")));
}

void MainWindow::showSearchWindow(){
    if (m_searchWindow == nullptr)
        return;

    // 第一次打开时把独立搜索窗放在主窗中心；之后记住用户拖动的位置。
    if (!m_searchWindow->property("placed").toBool()) {
        const QRect frame = frameGeometry();
        const QSize size = m_searchWindow->size();
        QRect available = screen() != nullptr ? screen()->availableGeometry()
                                              : QRect(frame.center(), QSize(1, 1));
        QPoint topLeft = frame.center() - QPoint(size.width() / 2, size.height() / 2);
        // 搜索窗比可用区域还大时，right()-width 会小于 left()，qBound 会触发
        // 上界小于下界的断言；先把上界抬到至少 left/top。
        const int maxX = qMax(available.left(), available.right() - size.width() + 1);
        const int maxY = qMax(available.top(), available.bottom() - size.height() + 1);
        topLeft.setX(qBound(available.left(), topLeft.x(), maxX));
        topLeft.setY(qBound(available.top(), topLeft.y(), maxY));
        m_searchWindow->move(topLeft);
        m_searchWindow->setProperty("placed", true);
    }
    m_searchWindow->focusSearch();
}

void MainWindow::on_btn_settings_clicked()
{
    m_settings->openDialog();
}

void MainWindow::on_btn_downloads_clicked()
{
    if (m_downloads != nullptr)
        m_downloads->openManager();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_downloads != nullptr && m_downloads->hasActiveDownloads()) {
        const int count = m_downloads->activeDownloadCount();
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("下载任务尚未完成"),
            tr("还有 %1 个下载任务正在进行。\n\n"
               "退出后会暂停并保留已下载的部分，下次启动程序会自动续传。\n"
               "是否退出？").arg(count),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        // 真正暂停下载放到播放列表保存成功之后，避免保存失败取消退出时下载已被停掉。
    }

    // 常显模式下把用户拖出的侧栏宽度写回设置；贴边浮层时不覆盖。
    if (m_sidebarSplitter != nullptr
        && m_sidebarSplitter->indexOf(ui->panelSidebar) >= 0
        && !ui->panelSidebar->property("floating").toBool()) {
        const int width = m_sidebarSplitter->sizes().value(0, ui->panelSidebar->width());
        if (width > 0)
            AppSettings::setSidebarWidth(width);
    }

    // 记录退出时正在播放的条目，下次启动恢复为默认选中项。
    m_playlists->rememberPlayingItem();

    // 保存前先把播放器的最后位置写回模型（不停止播放）。
    // 旧顺序是“先 save 再 stop”，stop 里才写位置，最后几秒进度会丢。
    if (m_player != nullptr)
        m_player->persistCurrentPosition();

    // 先确认播放列表真的落盘，再停止播放/退全屏/暂停下载。
    // 否则用户在保存失败对话框里点“取消退出”后，窗口还在但播放已停、下载已暂停。
    if (m_playlists->storageBlocked()) {
        // 读取失败或文件被占用：保存会覆盖原文件，只能明确告知本次不保存后退出。
        QMessageBox::warning(
            this, tr("播放列表未安全读取"),
            tr("播放列表原文件读取失败、被占用或超过大小上限，未能安全加载。\n\n"
               "为避免用空列表或旧快照覆盖原文件，本次退出不会保存当前列表修改；"
               "原文件未被覆盖（已判为损坏的文件会保留备份）。\n"
               "请检查 Data/playlists.json 的权限、占用情况后重启程序。"));
    } else {
        while (!m_playlists->save()) {
            const QMessageBox::StandardButton answer = QMessageBox::warning(
                this, tr("播放列表保存失败"),
                tr("播放列表无法写入数据目录。\n\n"
                   "可能原因：磁盘已满、目录无写权限，或数据文件被其他程序占用。\n"
                   "可以释放空间/关闭占用后点“重试”，或取消退出保留当前修改。"),
                QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
            if (answer != QMessageBox::Retry) {
                event->ignore();
                return;
            }
        }
    }

    if (m_downloads != nullptr && m_downloads->hasActiveDownloads())
        m_downloads->prepareForExit();

    // 全屏是独立顶层窗；不先退出，主窗关闭后进程可能被它拉住不退出。
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    m_player->stop();

    m_settings->save();
    QMainWindow::closeEvent(event);
    // 关闭主窗后显式结束事件循环：避免独立全屏窗或异步播放对象让进程残留在后台。
    QCoreApplication::quit();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (!event->spontaneous()) {
        QTimer::singleShot(0, this, &MainWindow::clampToAvailableScreen);
        QTimer::singleShot(0, this, &MainWindow::applySavedSidebarWidth);
    }
}

void MainWindow::clampToAvailableScreen()
{
    if (isFullScreen() || isMaximized() || isMinimized())
        return;

    QScreen *targetScreen = screen();
    if (targetScreen == nullptr)
        targetScreen = QGuiApplication::primaryScreen();
    if (targetScreen == nullptr)
        return;

    const QRect available = targetScreen->availableGeometry();
    QRect frame = frameGeometry();
    if (available.contains(frame)) {
        clampNativeFrameToWorkArea();
        return;
    }

    // 外框比可用区还大时，先按标题栏/边框差值收缩客户区，再整体挪回屏内
    const int frameExtraW = qMax(0, frame.width() - width());
    const int frameExtraH = qMax(0, frame.height() - height());
    const int targetW = qMin(width(), qMax(240, available.width() - frameExtraW));
    const int targetH = qMin(height(), qMax(180, available.height() - frameExtraH));
    const int oldW = width();
    const int oldH = height();
    if (targetW != oldW || targetH != oldH) {
        resize(targetW, targetH);
        // 窗口管理器可能延迟更新外框，尺寸真的变了就下一拍再夹一次
        if (width() != oldW || height() != oldH) {
            QTimer::singleShot(0, this, &MainWindow::clampToAvailableScreen);
            return;
        }
    }

    frame = frameGeometry();
    int x = frame.x();
    int y = frame.y();
    if (frame.right() > available.right())
        x -= frame.right() - available.right();
    if (frame.bottom() > available.bottom())
        y -= frame.bottom() - available.bottom();
    if (frame.left() < available.left())
        x += available.left() - frame.left();
    if (frame.top() < available.top())
        y += available.top() - frame.top();
    if (x != frame.x() || y != frame.y())
        move(x, y);

    clampNativeFrameToWorkArea();
}

void MainWindow::clampNativeFrameToWorkArea()
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    RECT frame{};
    if (!GetWindowRect(hwnd, &frame))
        return;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfo(monitor, &info))
        return;

    const RECT &work = info.rcWork;
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int maxWidth = work.right - work.left;
    const int maxHeight = work.bottom - work.top;
    const int targetW = qMin(width, maxWidth);
    const int targetH = qMin(height, maxHeight);
    int x = frame.left;
    int y = frame.top;
    if (x + targetW > work.right)
        x = work.right - targetW;
    if (y + targetH > work.bottom)
        y = work.bottom - targetH;
    if (x < work.left)
        x = work.left;
    if (y < work.top)
        y = work.top;

    if (x != frame.left || y != frame.top || targetW != width || targetH != height) {
        SetWindowPos(hwnd, nullptr, x, y, targetW, targetH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 侧栏、控制条都是浮层、不跟布局走，窗口尺寸一变要自己贴回原位
    if (m_panels != nullptr)
        m_panels->reposition();
    QTimer::singleShot(0, this, &MainWindow::updateControlBarCentering);
}

void MainWindow::updateControlBarCentering()
{
    // 左右两组辅助按钮宽度不同；两列都取较宽的一侧做最小宽度，
    // 中间播放键组才会留在面板正中央，不会因为左侧新增按钮而偏移。
    const int leftWidth = ui->layout_controlLeftButtons->minimumSize().width();
    const int rightWidth = ui->layout_auxButtons->minimumSize().width();
    const int columnWidth = qMax(leftWidth, rightWidth);
    ui->layout_controls->setColumnMinimumWidth(0, columnWidth);
    ui->layout_controls->setColumnMinimumWidth(2, columnWidth);
    ui->layout_controls->invalidate();
}

void MainWindow::setupSidebarSplitter()
{
    // .ui 的根布局是“侧栏控件 + 中心裸布局”。裸布局不能直接放进 QSplitter，
    // 这里把中心布局里的控件搬到承载 QWidget，再整体放进 splitter。
    // 新中心布局保留原来的 spacing/margins，视觉间距不变。
    m_sidebarSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
    m_sidebarSplitter->setObjectName(QStringLiteral("mainSplitter"));
    m_sidebarSplitter->setChildrenCollapsible(false);
    m_sidebarSplitter->setHandleWidth(8);

    QLayoutItem *centerItem = nullptr;
    for (int i = 0; i < ui->layout_root->count(); ++i) {
        QLayoutItem *item = ui->layout_root->itemAt(i);
        if (item != nullptr && item->layout() == ui->layout_center) {
            centerItem = ui->layout_root->takeAt(i);
            break;
        }
    }
    Q_ASSERT(centerItem != nullptr);
    if (centerItem == nullptr)
        return;

    QWidget *centerHost = new QWidget(m_sidebarSplitter);
    centerHost->setObjectName(QStringLiteral("panelCenterHost"));
    m_centerLayout = new QVBoxLayout(centerHost);
    m_centerLayout->setSpacing(ui->layout_center->spacing());
    m_centerLayout->setContentsMargins(ui->layout_center->contentsMargins());
    while (ui->layout_center->count() > 0) {
        QLayoutItem *child = ui->layout_center->takeAt(0);
        // 必须通过 addWidget 重新挂载，而不是直接搬 QLayoutItem：直接搬不会把
        // 控件 reparent 到 centerHost，控件仍挂在 centralwidget 上，
        // 之后 mapTo()/浮层定位会拿到错误坐标。
        if (QWidget *widget = child->widget()) {
            m_centerLayout->addWidget(widget);
            delete child;
        } else {
            m_centerLayout->addItem(child);
        }
    }
    // 中心布局已整体迁移完；takeAt() 返回的 QLayoutItem 就是 layout_center
    // 本身（QLayout 继承 QLayoutItem），所以下面 delete 同时释放 centerItem。
    // 不要再单独 delete centerItem，否则会二次释放。
    delete ui->layout_center;
    ui->layout_center = nullptr;

    ui->layout_root->removeWidget(ui->panelSidebar);
    m_sidebarSplitter->addWidget(ui->panelSidebar);
    m_sidebarSplitter->addWidget(centerHost);
    // 侧栏保持用户拖出的绝对宽度，窗口变宽时把新增空间全部给播放区。
    m_sidebarSplitter->setStretchFactor(0, 0);
    m_sidebarSplitter->setStretchFactor(1, 1);
    // 控制条/侧栏处于贴边浮层时，侧栏拖宽会改变舞台几何，浮层必须跟着重算。
    connect(m_sidebarSplitter, &QSplitter::splitterMoved, this, [this]() {
        if (m_panels != nullptr)
            m_panels->reposition();
    });
    ui->layout_root->addWidget(m_sidebarSplitter, 1);

    // 兼容旧 .ui 的 260/340 宽度上下限：最小宽度仍由字体自适应决定，
    // 但取消 340 上限，否则“可拖动”实际只能拖几十像素。
    ui->panelSidebar->setMaximumWidth(QWIDGETSIZE_MAX);
    m_pendingSidebarWidth = AppSettings::sidebarWidth();
}

/// @brief 首次显示布局完成后恢复上次拖出的侧栏宽度。
/// @note 延迟到 show 后执行：构造阶段 splitter 还没有实际像素宽度，
///       直接 setSizes 会被随后布局覆盖。
void MainWindow::applySavedSidebarWidth()
{
    if (m_sidebarSplitter == nullptr || m_pendingSidebarWidth <= 0)
        return;
    if (m_sidebarSplitter->indexOf(ui->panelSidebar) < 0) {
        // 侧栏处于贴边浮层模式，宽度由 PanelOverlayController 自己保存/恢复。
        m_pendingSidebarWidth = 0;
        return;
    }

    const int total = m_sidebarSplitter->width();
    if (total <= 0)
        return;
    const int minWidth = ui->panelSidebar->minimumWidth();
    // 给播放区至少留 320px，避免用户把侧栏拖到覆盖整个窗口后误以为界面坏了。
    const int maxWidth = qMax(minWidth, total - 320);
    const int width = qBound(minWidth, m_pendingSidebarWidth, maxWidth);
    m_sidebarSplitter->setSizes(QList<int>{width, total - width});
    m_pendingSidebarWidth = 0;
    // setSizes 引发的布局要等下一轮事件循环才生效；浮层控制条/侧栏要到那时
    // 才能按最终舞台尺寸重新贴边，避免启动后浮层停在旧几何上。
    QTimer::singleShot(0, this, [this]() {
        if (m_panels != nullptr)
            m_panels->reposition();
    });
}

void MainWindow::updateAdaptiveMetrics()
{
    if (ui == nullptr)
        return;

    // 左侧栏最小宽度由标签栏实际字体宽度决定：不同字体缩放下，
    // “音乐列表 / 视频列表”必须完整显示。最大宽度不再封顶，
    // 由 QSplitter 把实际宽度交给用户拖动，避免只能在小范围内调整。
    const QFontMetrics tabMetrics(ui->tab_media->tabBar()->font());
    const QFontMetrics titleMetrics(ui->label_playlistTitle->font());
    const QMargins margins = ui->layout_sidebar->contentsMargins();
    const int tabBarWidth = ui->tab_media->tabBar()->sizeHint().width();
    const int titleWidth = titleMetrics.horizontalAdvance(ui->label_playlistTitle->text());
    const int contentWidth = qMax(tabBarWidth, titleWidth) + margins.left() + margins.right()
                             + tabMetrics.height() * 2;
    const int widthCap = qMax(240, width() / 3);
    const int sidebarMinWidth = qMin(qMax(180, contentWidth), widthCap);
    ui->panelSidebar->setMinimumWidth(sidebarMinWidth);
    ui->panelSidebar->setMaximumWidth(QWIDGETSIZE_MAX);

    // 标签页最小高度跟着标签栏字体走，避免大字号下标签被压扁。
    ui->tab_media->setMinimumHeight(
        qMax(96, ui->tab_media->tabBar()->sizeHint().height()
                     + tabMetrics.height() * 2 + 16));

    // 底部区域按自身 sizeHint（状态文字、进度条、56px 播放键、间距和边距）
    // 决定最小高度，字体放大时不会被固定高度截断。
    ui->panelTransport->setMinimumHeight(
        qMax(112, ui->panelTransport->sizeHint().height()));

    if (m_panels != nullptr)
        m_panels->reposition();
}

void MainWindow::on_btn_fullscreen_clicked()
{
    m_fullscreen->toggle();
}

bool MainWindow::playSearchItem(const MediaItem &item)
{
    MediaItem playable = item;
    // 搜索源理论上都填了 online，但外部解析器/手工直链偶尔会漏；
    // 只要地址是 http(s) 就按网络条目走代理和请求头，别让签名直链裸奔。
    const bool httpUrl = NetworkAddressPolicy::isHttpMediaUrl(playable.url);
    if (httpUrl)
        playable.online = true;

    // 搜索结果只能播放网络地址：file/data/ftp 等协议既不该由远程源返回，
    // 也会绕过代理的请求头与缓存边界。
    if (!playable.isValid() || playable.url.isRelative() || !httpUrl) {
        ui->label_status->setText(tr("搜索结果缺少可播放地址，换一个来源或重新搜索试试"));
        qCWarning(lcWindow) << "搜索结果地址无效" << playable.title
                            << playable.url.toString();
        return false;
    }

    m_pendingSearchPlayIdentity = mediaIdentity(playable);
    qCInfo(lcWindow) << "播放搜索结果" << playable.displayName()
                     << (playable.online ? "网络" : "本地")
                     << HttpClient::redactUrl(playable.url);
    m_playlists->playItem(playable);
    return true;
}

void MainWindow::addSearchItemsToPlaylist(const QVector<MediaItem> &items, MediaType type)
{
    m_playlists->addSearchItems(items, type);
}

/// @brief 下载前解析网页播放页。
/// 不是网页地址时 resolver 会同步回调，不改变原有下载流程。
void MainWindow::startDownload(const MediaItem &item)
{
    if (m_pageResolver == nullptr || m_downloads == nullptr) {
        return;
    }
    m_pageResolver->resolve(item, [this](const MediaItem &resolved, const QString &error) {
        if (!error.isEmpty()) {
            qCWarning(lcWindow) << "下载前解析播放页失败" << resolved.displayName()
                                << error;
            ui->label_status->setText(tr("无法解析下载地址：%1").arg(error));
            return;
        }
        m_downloads->download(resolved);
    });
}

void MainWindow::clearPlaybackUi()
{
    // 播放条自己负责把进度、时间和歌词清零
    m_transport->clear();
    // 删除的是当前播放条目时，播放器不会再上报新曲目；动感视图必须整轨复位，
    // 否则封面、曲名、频谱和“正在播放”状态会留在已删除的条目上。
    if (m_musicVisual != nullptr) {
        m_musicVisual->reset();
        m_musicVisual->hide();
    }
    if (ui->videoWidget != nullptr) {
        ui->videoWidget->clearFrame();
        ui->videoWidget->show();
    }
}

void MainWindow::setupVolumePopup()
{
    // 控制条只保留一个音量图标；点开后在按钮上方弹出竖排滑块，避免横向
    // 滑块长期占用控制条宽度。slider_volume 是全局唯一音量入口，
    // PlaybackUiController 负责滑块与播放器双向同步。
    ui->label_volume->hide();
    ui->layout_auxButtons->removeWidget(ui->label_volume);
    ui->label_volume->deleteLater();

    ui->layout_auxButtons->removeWidget(ui->slider_volume);
    ui->slider_volume->setParent(this);

    m_volumePopup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    m_volumePopup->setObjectName(QStringLiteral("volumePopup"));
    m_volumePopup->setAttribute(Qt::WA_StyledBackground, true);
    m_volumePopup->setAttribute(Qt::WA_TranslucentBackground, true);
    m_volumePopup->setFixedSize(72, 206);

    auto *layout = new QVBoxLayout(m_volumePopup);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_volumeLabel = new QLabel(m_volumePopup);
    m_volumeLabel->setObjectName(QStringLiteral("label_volumeValue"));
    m_volumeLabel->setAlignment(Qt::AlignCenter);
    m_volumeLabel->setText(QStringLiteral("%1%").arg(ui->slider_volume->value()));
    layout->addWidget(m_volumeLabel);

    ui->slider_volume->setParent(m_volumePopup);
    ui->slider_volume->setOrientation(Qt::Vertical);
    ui->slider_volume->setInvertedAppearance(false);
    ui->slider_volume->setFixedSize(38, 146);
    ui->slider_volume->setVisible(true);
    layout->addWidget(ui->slider_volume, 0, Qt::AlignHCenter);

    connect(ui->slider_volume, &QSlider::valueChanged, this, [this](int value) {
        if (m_volumeLabel != nullptr)
            m_volumeLabel->setText(QStringLiteral("%1%").arg(value));
        if (ui->btn_volume != nullptr)
            ui->btn_volume->setToolTip(tr("音量 %1%").arg(value));
    });
    // 快捷键或播放器内部改动音量时，弹窗百分比和按钮提示也要跟随更新。
    connect(m_player, &PlaybackController::volumePercentChanged, this, [this](int value) {
        if (m_volumeLabel != nullptr)
            m_volumeLabel->setText(QStringLiteral("%1%").arg(value));
        if (ui->btn_volume != nullptr)
            ui->btn_volume->setToolTip(tr("音量 %1%").arg(value));
    });
    ui->btn_volume->setToolTip(tr("音量 %1%").arg(ui->slider_volume->value()));

    // 点击按钮在按钮上方弹出；点击外部由 Qt::Popup 自动关闭。
    connect(ui->btn_volume, &QPushButton::clicked, this, &MainWindow::showVolumePopup);
    if (m_volumePopup != nullptr)
        m_volumePopup->installEventFilter(this);
    m_volumePopup->hide();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_volumePopup && event->type() == QEvent::Hide)
        m_volumePopupHideMs = QDateTime::currentMSecsSinceEpoch();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::showVolumePopup()
{
    if (m_volumePopup == nullptr || ui->btn_volume == nullptr)
        return;
    if (m_volumePopup->isVisible()) {
        m_volumePopup->hide();
        return;
    }
    // 点击按钮本身时 Qt::Popup 会先因“点外部”自动隐藏，随后 clicked 又到这里。
    // 250ms 内视为同一次点击，不重新弹出。
    if (QDateTime::currentMSecsSinceEpoch() - m_volumePopupHideMs < 250)
        return;

    m_volumePopup->adjustSize();
    const QPoint buttonTop = ui->btn_volume->mapToGlobal(QPoint(0, 0));
    // 弹层比按钮宽，右边缘对齐会让弹层整体偏左；改为按按钮中心对齐，
    // 窄窗口下再用屏幕边界夹住，避免弹层被裁掉。
    int x = buttonTop.x() + (ui->btn_volume->width() - m_volumePopup->width()) / 2;
    int y = buttonTop.y() - m_volumePopup->height() - 6;

    // 控制条贴近屏幕顶端时向下弹，避免跑到屏幕外。
    const QRect available = ui->btn_volume->screen() != nullptr
                                ? ui->btn_volume->screen()->availableGeometry()
                                : QRect(buttonTop, QSize(0, 0));
    if (y < available.top())
        y = buttonTop.y() + ui->btn_volume->height() + 6;
    const int maxX = qMax(available.left(), available.right() - m_volumePopup->width() + 1);
    x = qBound(available.left(), x, maxX);

    m_volumePopup->move(x, y);
    m_volumePopup->show();
    m_volumePopup->raise();
}


