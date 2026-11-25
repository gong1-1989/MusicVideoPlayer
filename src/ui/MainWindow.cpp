/// @file MainWindow.cpp
/// @brief 主窗口装配：控制器创建、界面自适应、快捷键、设置/下载入口与关闭流程。
#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "cache/HlsCacheBuilder.h"
#include "cache/MediaCache.h"
#include "net/HttpClient.h"
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
#include "util/MediaExtensions.h"
#include "util/NetworkAddressPolicy.h"
#include "ui/UiIcons.h"
#include "ui/VideoStage.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLabel>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

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
    , m_registry(new SourceRegistry(this))
    , m_coordinator(nullptr)
    , m_player(new PlaybackController(this))
{
    ui->setupUi(this);
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
            this, &MainWindow::addSearchItemToPlaylist);
    connect(m_searchPanel, &SearchPanelController::downloadRequested, this,
            [this](const MediaItem &item) { m_downloads->download(item); });
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
            [this](const MediaItem &item) { m_downloads->download(item); });
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
    panelUi.centerLayout = ui->layout_center;
    m_panels = new PanelOverlayController(panelUi, this);

    // 全屏窗口：新建独立视频控件并接管播放器视频输出，避免重挂原生视频子窗丢帧
    m_fullscreen = new FullscreenController(ui->videoWidget, m_player->mediaPlayer(),
                                            ui->panelTransport, ui->layout_center,
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
                    // 控制区的画质/音质选择栏不再隐藏；音乐没有可选码率，
                    // 由 refreshBitrateBar() 改成“音质：…”并禁用下拉。
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
                    refreshBitrateBar();
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
    setupBitrateBar();
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
    if (!event->spontaneous())
        QTimer::singleShot(0, this, &MainWindow::clampToAvailableScreen);
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

void MainWindow::updateAdaptiveMetrics()
{
    if (ui == nullptr)
        return;

    // 左侧栏宽度由标签栏实际字体宽度决定：不同字体缩放下，
    // “音乐列表 / 视频列表”必须完整显示，不能靠固定 260 去硬撑。
    const QFontMetrics tabMetrics(ui->tab_media->tabBar()->font());
    const QFontMetrics titleMetrics(ui->label_playlistTitle->font());
    const QMargins margins = ui->layout_sidebar->contentsMargins();
    const int tabBarWidth = ui->tab_media->tabBar()->sizeHint().width();
    const int titleWidth = titleMetrics.horizontalAdvance(ui->label_playlistTitle->text());
    const int contentWidth = qMax(tabBarWidth, titleWidth) + margins.left() + margins.right()
                             + tabMetrics.height() * 2;
    const int widthCap = qMax(240, width() / 3);
    const int sidebarWidth = qMin(qMax(220, contentWidth), widthCap);
    ui->panelSidebar->setMinimumWidth(sidebarWidth);
    ui->panelSidebar->setMaximumWidth(sidebarWidth + tabMetrics.height() * 2);

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

void MainWindow::addSearchItemToPlaylist(const MediaItem &item, MediaType type)
{
    m_playlists->addSearchItem(item, type);
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
    // 当前条目被删后代理里还留着上一部的 master 变体；清空选项指纹并立即
    // 刷新为“画质/音质：—”，但选择栏本身保持可见，不再隐藏。
    m_bitrateOptionsSignature.clear();
    refreshBitrateBar();
}

void MainWindow::setupBitrateBar()
{
    // 选择栏常驻控制条，不再按播放状态隐藏；初始内容由 refresh 统一填充。
    ui->bitrateBar->show();
    connect(ui->combo_bitrate, QOverload<int>::of(&QComboBox::activated), this,
            [this](int comboIndex) {
                const int target = ui->combo_bitrate->itemData(comboIndex).toInt();
                if (m_player == nullptr)
                    return;
                m_player->selectBitrate(target);
                // 切换是同步重载，下一次快照能立即反映锁定项；延时一拍是为了
                // 让下拉框先收起，再按新状态刷新选项文本。
                QTimer::singleShot(0, this, &MainWindow::refreshBitrateBar);
            });

    m_bitrateTimer = new QTimer(this);
    m_bitrateTimer->setInterval(1000);
    connect(m_bitrateTimer, &QTimer::timeout, this, &MainWindow::refreshBitrateBar);
    m_bitrateTimer->start();
    refreshBitrateBar();
}

void MainWindow::refreshBitrateBar()
{
    if (ui == nullptr || ui->bitrateBar == nullptr || ui->combo_bitrate == nullptr
        || m_player == nullptr) {
        return;
    }

    // 没有独立文字标签后，所有说明都必须落在下拉框自身：
    // - HLS 视频列出可选码率，自动项带上当前实际码率；
    // - 音乐显示音质；
    // - 直链/已缓存视频显示固定画质；
    // - 没有当前条目时显示占位内容。
    // 选择栏始终可见，只按状态切换是否可展开。
    const auto showSingle = [this](const QString &text, const QString &tip) {
        QSignalBlocker blocker(ui->combo_bitrate);
        if (ui->combo_bitrate->count() != 1 || ui->combo_bitrate->itemData(0).isValid()) {
            ui->combo_bitrate->clear();
            ui->combo_bitrate->addItem(text);
        } else if (ui->combo_bitrate->itemText(0) != text) {
            ui->combo_bitrate->setItemText(0, text);
        }
        ui->combo_bitrate->setEnabled(false);
        ui->combo_bitrate->setToolTip(tip);
        m_bitrateOptionsSignature.clear();
        ui->bitrateBar->show();
    };

    const MediaItem item = m_player->currentItem();
    if (!item.isValid()) {
        showSingle(tr("画质/音质：—"), tr("播放音乐或视频后显示音质/可选码率"));
        return;
    }

    if (item.type == MediaType::Music) {
        // 在线音乐一般带 quality；本地文件此前没有品质字段，直接显示“标准”
        // 会把 FLAC/APE 这类无损文件标错。按扩展名至少区分无损与有损格式。
        QString quality = item.quality;
        if (quality.isEmpty() && !item.online && item.url.isLocalFile()) {
            const QString suffix = QFileInfo(item.url.toLocalFile()).suffix().toLower();
            if (MediaExtensions::isLosslessAudioSuffix(suffix))
                quality = tr("无损");
            else if (!suffix.isEmpty())
                quality = suffix.toUpper();
        }
        if (quality.isEmpty())
            quality = tr("标准");
        showSingle(tr("音质：%1").arg(quality), tr("当前音乐的播放音质"));
        return;
    }

    const auto snapshot = m_player->bitrateSnapshot();
    if (!snapshot.isVideo || snapshot.options.isEmpty()) {
        QString text;
        if (item.quality.isEmpty())
            text = tr("画质：固定");
        else
            text = tr("画质：%1").arg(item.quality);
        if (!MediaCache::completeFileForItem(item).isEmpty())
            text += tr("（已缓存）");
        showSingle(text, tr("当前视频没有可切换的 HLS 码率"));
        return;
    }

    const auto labelFor = [this](const PlaybackController::BitrateOption &option) {
        const QString resolution = option.resolution.isEmpty()
                                       ? tr("未知分辨率")
                                       : option.resolution;
        QString bandwidth;
        if (option.bandwidth <= 0) {
            bandwidth = tr("未知码率");
        } else if (option.bandwidth >= 1000000) {
            // HLS 的 BANDWIDTH 单位是 bit/s；不能按 kbit/s 换算，否则
            // 1300000 会显示成 1300.00 Mbps。
            bandwidth = QStringLiteral("%1 Mbps").arg(option.bandwidth / 1000000.0, 0, 'f', 2);
        } else if (option.bandwidth >= 1000) {
            bandwidth = QStringLiteral("%1 kbps").arg(option.bandwidth / 1000.0, 0, 'f', 0);
        } else {
            bandwidth = QStringLiteral("%1 bps").arg(option.bandwidth);
        }
        return QStringLiteral("%1 · %2").arg(resolution, bandwidth);
    };

    // 高码率放前面，符合“清晰度优先”的直觉；序号仍来自原始 master，
    // 排序不会影响锁定项和当前项的匹配。
    QList<PlaybackController::BitrateOption> ordered = snapshot.options;
    std::sort(ordered.begin(), ordered.end(),
              [](const PlaybackController::BitrateOption &a,
                 const PlaybackController::BitrateOption &b) {
                  if (a.bandwidth != b.bandwidth)
                      return a.bandwidth > b.bandwidth;
                  return a.index < b.index;
              });

    QString currentLabel;
    for (const PlaybackController::BitrateOption &option : std::as_const(ordered)) {
        if (option.index == snapshot.currentIndex) {
            currentLabel = labelFor(option);
            break;
        }
    }

    // 自动项直接显示“自动 · 当前码率”，让用户不点开下拉也能看到实际播放码率。
    QString autoText = tr("自动");
    if (!currentLabel.isEmpty())
        autoText = tr("自动 · %1").arg(currentLabel);
    else if (snapshot.selectedIndex >= 0)
        autoText = tr("自动（切换中）");

    QString signature = QStringLiteral("item:%1;").arg(item.url.toString());
    for (const PlaybackController::BitrateOption &option : std::as_const(ordered)) {
        signature += QStringLiteral("%1:%2:%3;").arg(option.index)
                         .arg(option.bandwidth)
                         .arg(option.resolution);
    }

    if (signature != m_bitrateOptionsSignature) {
        QSignalBlocker blocker(ui->combo_bitrate);
        ui->combo_bitrate->clear();
        ui->combo_bitrate->addItem(autoText, -1);
        for (const PlaybackController::BitrateOption &option : std::as_const(ordered))
            ui->combo_bitrate->addItem(labelFor(option), option.index);
        m_bitrateOptionsSignature = signature;
    } else {
        QSignalBlocker blocker(ui->combo_bitrate);
        if (ui->combo_bitrate->count() > 0 && ui->combo_bitrate->itemText(0) != autoText)
            ui->combo_bitrate->setItemText(0, autoText);
    }

    {
        QSignalBlocker blocker(ui->combo_bitrate);
        const int comboIndex = ui->combo_bitrate->findData(snapshot.selectedIndex);
        ui->combo_bitrate->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
    }
    ui->combo_bitrate->setEnabled(true);
    ui->combo_bitrate->setToolTip(tr("选择码率后按所选码率重新播放，并尽量保留进度"));
    ui->bitrateBar->show();
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


