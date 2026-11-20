#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/DownloadController.h"
#include "ui/FullscreenController.h"
#include "ui/LyricsController.h"
#include "ui/PanelOverlayController.h"
#include "ui/PlaybackUiController.h"
#include "ui/PlaylistController.h"
#include "ui/SearchPanelController.h"
#include "ui/SettingsDialog.h"
#include "ui/ThemeManager.h"
#include "ui/VideoStage.h"
#include "util/TimeFormat.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTimer>
#include <QWidget>

Q_LOGGING_CATEGORY(lcWindow, "mvp.window")

namespace {

/// 列表行文字滚动：默认风格放不下就省略号，这里让"悬停/当前"那一行横向滚动，
/// 其余行仍用省略号，避免整屏一起动、看着发花。
class MarqueeDelegate : public QStyledItemDelegate
{
public:
    explicit MarqueeDelegate(QListView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
        m_timer.setInterval(40);
        QObject::connect(&m_timer, &QTimer::timeout, view->viewport(), [this] { advance(); });
        m_timer.start();

        m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_view->setMouseTracking(true);
        m_view->viewport()->setMouseTracking(true);
        m_view->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == m_view->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
                const QModelIndex index = m_view->indexAt(pos);
                m_hoveredRow = index.isValid() ? index.row() : -1;
            } else if (event->type() == QEvent::Leave) {
                m_hoveredRow = -1;
            } else if (event->type() == QEvent::Hide || event->type() == QEvent::HideToParent) {
                // 切到另一个列表页后不再 40ms 空转，重新显示时再启动
                m_timer.stop();
            } else if (event->type() == QEvent::Show || event->type() == QEvent::ShowToParent) {
                if (!m_timer.isActive())
                    m_timer.start();
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QString text = opt.text;
        const QWidget *widget = opt.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();

        const QFontMetrics metrics(opt.font);
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const int textWidth = metrics.horizontalAdvance(text);
        const bool scrollRow =
            index.row() == m_hoveredRow || (m_hoveredRow < 0 && index.row() == m_view->currentIndex().row());

        if (textRect.isEmpty() || text.isEmpty() || !scrollRow || textWidth <= textRect.width()) {
            // 放得下就整行走主题默认绘制，选中态颜色完全跟 QSS 一致
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // 背景、悬停/选中态交给主题去画，文字留空后自己按横向偏移画
        opt.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const QPalette::ColorGroup group =
            (option.state & QStyle::State_Enabled) != 0 ? QPalette::Normal : QPalette::Disabled;

        painter->save();
        painter->setClipRect(textRect);
        painter->setPen(opt.palette.color(group,
                                         selected ? QPalette::HighlightedText : QPalette::Text));

        // 两段首尾相接，滚到末尾时无缝接回开头
        const int gap = 36;
        const int period = textWidth + gap;
        const int offset = m_phase % period;
        painter->drawText(QRect(textRect.x() - offset, textRect.y(), textWidth, textRect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, text);
        painter->drawText(
            QRect(textRect.x() - offset + period, textRect.y(), textWidth, textRect.height()),
            Qt::AlignVCenter | Qt::AlignLeft, text);
        painter->restore();
    }

private:
    void advance()
    {
        const int row = m_hoveredRow >= 0 ? m_hoveredRow : m_view->currentIndex().row();
        if (row < 0)
            return;
        const QModelIndex index = m_view->model() ? m_view->model()->index(row, 0, QModelIndex())
                                                  : QModelIndex();
        if (!index.isValid())
            return;
        const QRect rect = m_view->visualRect(index);
        if (!rect.isValid())
            return;
        // 放得下就不用动，省掉无意义的重绘
        const QFontMetrics metrics(m_view->font());
        if (metrics.horizontalAdvance(index.data(Qt::DisplayRole).toString()) <= rect.width() - 24)
            return;

        m_phase += 2;
        if (m_phase > 100000)
            m_phase = 0;
        m_view->viewport()->update(rect);
    }

    QListView *m_view;
    QTimer m_timer;
    int m_hoveredRow = -1;
    int m_phase = 0;
};

/// 预设尺寸只是目标值：屏幕装不下时按可用工作区收一收，否则布局会被挤到裁切
QSize fitToAvailableScreen(const QSize &size)
{
    QSize target = size;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        target.setWidth(qMin(target.width(), available.width() - 24));
        target.setHeight(qMin(target.height(), available.height() - 32));
    }
    return target;
}

/// 当前屏幕的可用工作区（扣掉任务栏）
QSize availableWindowSize()
{
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        return QSize(available.width(), available.height());
    }
    return QSize(1280, 720);
}

} // namespace

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
    connect(m_downloads, &DownloadController::lastDirChanged, this,
            [this](const QString &dir) { m_lastDir = dir; });

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
            [this](const MediaItem &item) { m_downloads->download(item, m_lastDir); });

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
    connect(m_playlists, &PlaylistController::localFilesRequested, this,
            [this](MediaType type) {
                if (type == MediaType::Music)
                    openLocalMusic();
                else
                    openLocalVideo();
            });

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

    setupConnections();
    restoreSettings();
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

void MainWindow::restoreSettings()
{
    m_lastDir = AppSettings::lastDir();

    // 音量、播放模式、搜索条数全部来自设置，不再有散落的默认值
    m_player->setVolumePercent(AppSettings::defaultVolume());
    ui->slider_volume->setValue(m_player->volumePercent());

    const int modeValue = AppSettings::playbackMode();
    m_player->setMode(static_cast<PlaybackController::Mode>(qBound(0, modeValue, 2)));

    applyWindowPreset();
}

void MainWindow::applyWindowPreset()
{
    const WindowPreset preset = AppSettings::windowPreset();

    // 记住上次：直接用上次的几何，不再套预设
    if (preset == WindowPreset::Last) {
        const QByteArray geometry = AppSettings::savedGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        return;
    }

    // 自适应 = 铺满可用工作区；固定预设 = 标准分辨率
    const QSize presetSize = preset == WindowPreset::Auto
                                 ? availableWindowSize()
                                 : AppSettings::presetWindowSize(preset);
    if (!presetSize.isValid())
        return;

    // 预设只是目标值：屏幕装不下时按可用工作区收一收
    const QSize target = fitToAvailableScreen(presetSize);
    resize(target);

    // 自适应/固定尺寸都不沿用历史位置，启动时居中
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        move(available.center() - QPoint(target.width() / 2, target.height() / 2));
    }
}

void MainWindow::saveSettings()
{
    AppSettings::setLastDir(m_lastDir);
    AppSettings::setDefaultVolume(m_player->volumePercent());
    AppSettings::setPlaybackMode(static_cast<int>(m_player->mode()));
    // 播放列表现只写 Data/playlists.json；QSettings 双写会让删除后的列表“复活”。

    // 只有"记住上次"才写几何，固定尺寸预设下写进去会互相打架
    if (AppSettings::windowPreset() == WindowPreset::Last)
        AppSettings::setSavedGeometry(saveGeometry());
}

void MainWindow::applySettings()
{
    ThemeManager::instance().apply(AppSettings::themeId());

    // 尺寸实时跟着设置走：自适应铺开，固定预设按标准分辨率（超屏自动收）
    const WindowPreset preset = AppSettings::windowPreset();
    if (preset != WindowPreset::Last) {
        const QSize target = preset == WindowPreset::Auto
                                 ? fitToAvailableScreen(availableWindowSize())
                                 : fitToAvailableScreen(AppSettings::presetWindowSize(preset));
        if (target.isValid())
            resize(target);
    }

    m_player->setVolumePercent(AppSettings::defaultVolume());
    {
        QSignalBlocker blocker(ui->slider_volume);
        ui->slider_volume->setValue(m_player->volumePercent());
    }

    // 侧栏 / 控制条的启动折叠策略改了要立刻生效
    m_panels->applyVisibility();

    m_lyrics->refreshHighlight();
}

void MainWindow::on_btn_settings_clicked()
{
    SettingsDialog dialog(m_registry, m_http, this);
    if (dialog.exec() == QDialog::Accepted)
        applySettings();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 全屏是独立顶层窗；不先退出，主窗关闭后进程可能被它拉住不退出。
    if (m_fullscreen != nullptr)
        m_fullscreen->exitFullScreen();
    m_player->stop();
    m_playlists->save();
    saveSettings();
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

void MainWindow::openLocalMusic()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("选择音频文件"),
        m_lastDir,
        tr("音频文件 (*.mp3 *.wav *.m4a *.flac *.ogg);;所有文件 (*)"));
    if (files.isEmpty())
        return;

    m_lastDir = QFileInfo(files.first()).absolutePath();
    m_playlists->addLocalFiles(MediaType::Music, files);
}

void MainWindow::openLocalVideo()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("选择视频文件"),
        m_lastDir,
        tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.wmv);;所有文件 (*)"));
    if (files.isEmpty())
        return;

    m_lastDir = QFileInfo(files.first()).absolutePath();
    m_playlists->addLocalFiles(MediaType::Video, files);
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
    // 停播信号只负责清画面；进度条、时间和歌词仍会停在已删除的那一行
    QSignalBlocker blocker(ui->slider_progress);
    ui->slider_progress->setValue(0);
    ui->label_time->setText(QStringLiteral("00:00 / 00:00"));
    m_lyrics->clear();
}


