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
#include "ui/MusicVisualWidget.h"
#include "ui/PanelOverlayController.h"
#include "ui/PlaybackUiController.h"
#include "ui/PlaylistController.h"
#include "ui/SearchPanelController.h"
#include "ui/SearchWindow.h"
#include "ui/SettingsController.h"
#include "ui/ThemeManager.h"
#include "ui/UiIcons.h"
#include "ui/VideoStage.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QSlider>
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

    // 播放列表为安全落盘不保存 Cookie；播放/下载前按 sourceId 从源配置
    // 恢复内存态凭据，源配置被更新或删除后也自然跟随。
    const auto credentialResolver = [this](const MediaItem &item) {
        return m_registry->mediaItemWithCredentials(item);
    };
    m_player->setMediaItemResolver(credentialResolver);
    m_downloads->setMediaItemResolver(credentialResolver);

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
                if (m_searchWindow != nullptr)
                    m_searchWindow->hide();
                // 搜索窗作为独立顶层窗口关闭后，主动把焦点还给主窗，
                // 避免用户下一次点击侧栏搜索按钮时第一下只起到激活窗口的作用。
                raise();
                activateWindow();
                playSearchItem(item);
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
            this, &MainWindow::clearPlaybackUi);
    connect(m_playlists, &PlaylistController::downloadRequested, this,
            [this](const MediaItem &item) { m_downloads->download(item); });
    // 视频进度一旦写回模型就立刻落盘：停止、切音乐、切视频、退出都会经过
    // PlaybackController::saveCurrentVideoPosition()，不能只在退出时保存。
    connect(m_player, &PlaybackController::currentItemPositionUpdated, m_playlists,
            [this](int) {
                if (!m_playlists->save())
                    qCWarning(lcWindow) << "视频进度写入播放列表失败，等待下次保存重试";
            });
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int) {
                if (!m_pendingSearchPlayIdentity.isEmpty()
                    && mediaIdentity(item) != m_pendingSearchPlayIdentity) {
                    m_pendingSearchPlayIdentity.clear();
                }
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

    // 音乐没有视频帧，原 VideoSurface 会一直黑着。视觉层盖在整个播放舞台上，
    // 播音乐时隐藏视频面、显示动态渐变+频谱；切回视频立刻还原。
    m_musicVisual = new MusicVisualWidget(ui->videoStage);
    m_musicVisual->setGeometry(ui->videoStage->rect());
    m_musicVisual->raise();
    m_musicVisual->hide();
    // 频谱直接取播放器解码后的 PCM，只分析本播放器自身音频。
    m_musicVisual->setMediaPlayer(m_player->mediaPlayer());
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int) {
                const bool music = item.isValid() && item.type == MediaType::Music;
                if (music) {
                    m_musicVisual->setTrack(item);
                    ui->videoWidget->hide();
                    m_musicVisual->setGeometry(ui->videoStage->rect());
                    m_musicVisual->show();
                    m_musicVisual->raise();
                } else {
                    m_musicVisual->hide();
                    ui->videoWidget->show();
                }
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
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    delete ui;
}

void MainWindow::setupConnections()
{
    connect(ui->btn_openSearch, &QPushButton::clicked,
            this, &MainWindow::showSearchWindow);

    // 播放列表目前是自动保存；再给一个显式保存入口，便于确认修改真的落盘。
    auto *savePlaylistShortcut = new QShortcut(QKeySequence::Save, this);
    connect(savePlaylistShortcut, &QShortcut::activated, this, [this] {
        if (m_playlists != nullptr)
            m_playlists->saveNow();
    });
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

void MainWindow::showSearchWindow()
{
    if (m_searchWindow == nullptr)
        return;

    // 第一次打开时把独立搜索窗放在主窗中心；之后记住用户拖动的位置。
    if (!m_searchWindow->property("placed").toBool()) {
        const QRect frame = frameGeometry();
        const QSize size = m_searchWindow->size();
        QRect available = screen() != nullptr ? screen()->availableGeometry()
                                              : QRect(frame.center(), QSize(1, 1));
        QPoint topLeft = frame.center() - QPoint(size.width() / 2, size.height() / 2);
        topLeft.setX(qBound(available.left(), topLeft.x(),
                            available.right() - size.width()));
        topLeft.setY(qBound(available.top(), topLeft.y(),
                            available.bottom() - size.height()));
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

    // 先确认播放列表真的落盘，再停止播放/退全屏/暂停下载。
    // 否则用户在保存失败对话框里点“取消退出”后，窗口还在但播放已停、下载已暂停。
    if (m_playlists->storageBlocked()) {
        // 读取失败且连备份都做不了：保存会覆盖原文件，只能明确告知本次不保存后退出。
        QMessageBox::warning(
            this, tr("播放列表未安全读取"),
            tr("播放列表原文件读取失败，且无法备份到其他路径。\n\n"
               "为避免用空列表覆盖原文件，本次退出不会保存当前列表修改；原文件保持不变。\n"
               "请检查 Data/playlists.json 的权限或占用情况后重启程序。"));
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

void MainWindow::playSearchItem(const MediaItem &item)
{
    MediaItem playable = item;
    // 搜索源理论上都填了 online，但外部解析器/手工直链偶尔会漏；
    // 只要地址是 http(s) 就按网络条目走代理和请求头，别让签名直链裸奔。
    const QString scheme = playable.url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        playable.online = true;

    if (!playable.isValid() || playable.url.isRelative()) {
        ui->label_status->setText(tr("搜索结果缺少可播放地址，换一个来源或重新搜索试试"));
        qCWarning(lcWindow) << "搜索结果地址无效" << playable.title
                            << playable.url.toString();
        return;
    }

    m_pendingSearchPlayIdentity = mediaIdentity(playable);
    qCInfo(lcWindow) << "播放搜索结果" << playable.displayName()
                     << (playable.online ? "网络" : "本地")
                     << HttpClient::redactUrl(playable.url);
    m_playlists->playItem(playable);
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

void MainWindow::setupVolumePopup()
{
    // 要求是“右侧竖排悬浮音量条”：原来横向滑块长期占着控制条宽度，
    // 现在只保留一个音量图标，点开后在按钮上方弹出竖排滑块。
    // slider_volume 对象本身继续复用，PlaybackUiController 的音量联动和
    // SettingsController 的持久化都不需要改接口。
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
    ui->btn_volume->setToolTip(tr("音量 %1%").arg(ui->slider_volume->value()));

    // 点击按钮在按钮上方弹出；点击外部由 Qt::Popup 自动关闭。
    connect(ui->btn_volume, &QPushButton::clicked, this, &MainWindow::showVolumePopup);
    m_volumePopup->hide();
}

void MainWindow::showVolumePopup()
{
    if (m_volumePopup == nullptr || ui->btn_volume == nullptr)
        return;
    if (m_volumePopup->isVisible()) {
        m_volumePopup->hide();
        return;
    }

    m_volumePopup->adjustSize();
    const QPoint buttonTop = ui->btn_volume->mapToGlobal(QPoint(0, 0));
    int x = buttonTop.x() + ui->btn_volume->width() - m_volumePopup->width();
    int y = buttonTop.y() - m_volumePopup->height() - 6;

    // 控制条贴近屏幕顶端时向下弹，避免跑到屏幕外。
    const QRect available = ui->btn_volume->screen() != nullptr
                                ? ui->btn_volume->screen()->availableGeometry()
                                : QRect(buttonTop, QSize(0, 0));
    if (y < available.top())
        y = buttonTop.y() + ui->btn_volume->height() + 6;
    x = qBound(available.left(), x, available.right() - m_volumePopup->width());

    m_volumePopup->move(x, y);
    m_volumePopup->show();
    m_volumePopup->raise();
}


