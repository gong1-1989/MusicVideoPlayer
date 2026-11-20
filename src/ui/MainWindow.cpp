#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "download/DownloadTask.h"
#include "lyrics/LyricsService.h"
#include "net/HttpClient.h"
#include "net/SearchCoordinator.h"
#include "net/SourceRegistry.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
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
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeyEvent>
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
#include <QVBoxLayout>
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

QVector<MediaItem> playlistFromJson(const QByteArray &payload)
{
    QVector<MediaItem> items;
    if (payload.isEmpty())
        return items;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray())
        return items;

    const QJsonArray array = doc.array();
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const MediaItem item = mediaItemFromJson(value.toObject());
        if (item.isValid())
            items.append(item);
    }
    return items;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_http(new HttpClient(this))
    , m_registry(new SourceRegistry(this))
    , m_coordinator(nullptr)
    , m_lyrics(new LyricsService(m_http, this))
    , m_player(new PlaybackController(this))
    , m_musicModel(new PlaylistModel(this))
    , m_videoModel(new PlaylistModel(this))
{
    ui->setupUi(this);

    if (!m_registry->load() || !m_registry->lastError().isEmpty()) {
        const QString message = m_registry->lastError().isEmpty()
                                    ? tr("搜索源配置读取失败，已使用空配置启动。")
                                    : m_registry->lastError();
        QMessageBox::warning(this, tr("搜索源配置"), message);
    }
    m_coordinator = new SearchCoordinator(m_registry, m_http, this);

    ui->list_music->setModel(m_musicModel);
    ui->list_video->setModel(m_videoModel);

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
    connect(m_searchPanel, &SearchPanelController::downloadRequested,
            this, &MainWindow::downloadItem);

    // 列表里长名字默认被省略号截断，改成悬停/当前行横向滚动
    ui->list_music->setItemDelegate(new MarqueeDelegate(ui->list_music));
    ui->list_video->setItemDelegate(new MarqueeDelegate(ui->list_video));

    m_player->mediaPlayer()->setVideoSink(ui->videoWidget->sink());

    // 歌词栏默认隐藏，只有播音乐时才让位出来
    ui->panelLyrics->hide();

    // 左侧栏 / 控制条"收起=浮层、常显=占布局"的切换由 applyPanelVisibility() 决定，
    // 这里只管鼠标贴边检测的定时器
    m_edgeTimer = new QTimer(this);
    m_edgeTimer->setInterval(120);
    connect(m_edgeTimer, &QTimer::timeout, this, &MainWindow::updateEdgePanels);
    m_edgeTimer->start();

    // 自绘控件的几何参数：圆角半径与歌词渐隐高度不再散落在各处
    ui->videoStage->setCornerRadius(16.0);
    ui->list_lrc->setFadeExtent(28);

    // 原生标题栏跟着主题走，不要留一条白条
    ThemeManager::instance().applyWindowChrome(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] {
                ThemeManager::instance().applyWindowChrome(this);
                highlightCurrentLyric();
            });

    auto *settingsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    connect(settingsShortcut, &QShortcut::activated, this, &MainWindow::on_btn_settings_clicked);

    setupConnections();
    restoreSettings();
    restorePlaylists();
    setupPlaylistInteractions();
    refreshModeButton();
    applyPanelVisibility();
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
    if (m_fullScreenWindow != nullptr)
        exitFullScreen();
    delete ui;
}

void MainWindow::setupConnections()
{
    // 音量：单向 slider -> controller，避免双向回环
    connect(ui->slider_volume, &QSlider::valueChanged,
            m_player, &PlaybackController::setVolumePercent);

    // 进度条：拖动期间不回写，松手才 seek
    connect(ui->slider_progress, &QSlider::sliderPressed, this, [this] {
        m_userSeeking = true;
    });
    connect(ui->slider_progress, &QSlider::sliderReleased, this, [this] {
        m_userSeeking = false;
        m_player->seek(ui->slider_progress->value());
    });
    connect(ui->slider_progress, &QSlider::sliderMoved, this, [this](int value) {
        ui->label_time->setText(TimeFormat::formatMs(value) + QStringLiteral(" / ")
                                + TimeFormat::formatMs(m_player->duration()));
    });
    // 点击凹槽直接跳转（该路径不一定触发 sliderPressed）
    connect(ui->slider_progress, &QSlider::valueChanged, this, [this](int value) {
        if (m_userSeeking)
            return;
        if (qAbs(value - m_player->position()) > 1500)
            m_player->seek(value);
    });

    connect(m_player, &PlaybackController::positionChanged, this, [this](qint64 position) {
        if (!m_userSeeking) {
            QSignalBlocker blocker(ui->slider_progress);
            ui->slider_progress->setValue(static_cast<int>(position));
        }
        ui->label_time->setText(TimeFormat::formatMs(position) + QStringLiteral(" / ")
                                + TimeFormat::formatMs(m_player->duration()));
        updateLyrics(position);
    });
    connect(m_player, &PlaybackController::durationChanged, this, [this](qint64 duration) {
        QSignalBlocker blocker(ui->slider_progress);
        ui->slider_progress->setRange(0, static_cast<int>(qMax<qint64>(0, duration)));
    });
    connect(m_player, &PlaybackController::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                ui->btn_play->setText(state == QMediaPlayer::PlayingState ? tr("暂停") : tr("播放"));
                // 停止后不再占着歌词栏；继续播音乐时再放出来
                if (state == QMediaPlayer::StoppedState) {
                    // 停止后播放器不会再送新帧；不清掉的话上一部视频的最后一帧会一直留在面板上
                    ui->videoWidget->clearFrame();
                    ui->panelLyrics->hide();
                } else {
                    // 新一次播放开始，允许视频面重新接收帧
                    ui->videoWidget->resumeFrames();
                    if (m_player->hasCurrentItem()
                        && m_player->currentItem().type == MediaType::Music)
                        ui->panelLyrics->show();
                }
            });
    connect(m_player, &PlaybackController::modeChanged, this, [this] { refreshModeButton(); });
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                // 视频切歌/切回音乐时先清掉上一帧，等新视频首帧到达再显示
                ui->videoWidget->clearFrame();
                ui->label_status->setText(tr("正在播放：%1").arg(item.displayName()));

                // 歌词栏只在音乐播放时出现，视频不需要
                ui->panelLyrics->setVisible(item.type == MediaType::Music);

                if (item.type == MediaType::Music) {
                    ui->list_music->setCurrentIndex(m_musicModel->index(row));
                    ui->tab_media->setCurrentIndex(0);
                } else {
                    ui->list_video->setCurrentIndex(m_videoModel->index(row));
                    ui->tab_media->setCurrentIndex(1);
                }

                m_lrcLines.clear();
                m_lrcIndex = -1;
                ui->list_lrc->clear();

                if (item.lyricUrl.isLocalFile())
                    m_lyrics->loadLocalFile(item.lyricUrl.toLocalFile());
                else if (!item.lyricUrl.isEmpty())
                    m_lyrics->load(item.lyricUrl);
            });
    connect(m_player, &PlaybackController::errorOccurred, this, [this](const QString &message) {
        ui->label_status->setText(tr("播放失败：%1").arg(message));
    });

    connect(m_lyrics, &LyricsService::lyricsReady, this, [this](const QVector<LrcLine> &lines) {
        m_lrcLines = lines;
        m_lrcIndex = -1;
        ui->list_lrc->clear();
        for (const LrcLine &line : lines) {
            auto *item = new QListWidgetItem(line.text);
            item->setTextAlignment(Qt::AlignCenter);
            ui->list_lrc->addItem(item);
        }
        if (m_player->hasCurrentItem())
            updateLyrics(m_player->position());
    });
    connect(m_lyrics, &LyricsService::lyricsFailed, this, [this](const QString &message) {
        qCWarning(lcWindow) << "歌词加载失败：" << message;
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
    applyPanelVisibility();

    highlightCurrentLyric();
}

void MainWindow::on_btn_settings_clicked()
{
    SettingsDialog dialog(m_registry, m_http, this);
    if (dialog.exec() == QDialog::Accepted)
        applySettings();
}

void MainWindow::highlightCurrentLyric()
{
    if (m_lrcIndex < 0 || m_lrcIndex >= ui->list_lrc->count())
        return;
    if (auto *item = ui->list_lrc->item(m_lrcIndex))
        item->setForeground(QBrush(ThemeManager::instance().current().accentColor));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 全屏是独立顶层窗；不先退出，主窗关闭后进程可能被它拉住不退出。
    if (m_fullScreenWindow != nullptr)
        exitFullScreen();
    m_player->stop();
    savePlaylists();
    saveSettings();
    QMainWindow::closeEvent(event);
    // 关闭主窗后显式结束事件循环：避免独立全屏窗或异步播放对象让进程残留在后台。
    QCoreApplication::quit();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 侧栏、控制条都是浮层、不跟布局走，窗口尺寸一变要自己贴回原位
    positionSidebarOverlay();
    positionControlOverlay();
}

void MainWindow::restorePlaylists()
{
    QVector<MediaItem> music = PlaylistStore::load(MediaType::Music);
    QVector<MediaItem> video = PlaylistStore::load(MediaType::Video);

    // 只迁移一次：旧版 QSettings 里的列表写入 Data/playlists.json 后清键。
    // 迁移标记防止“用户清空列表”后下次启动又被旧键复活。
    if (!AppSettings::playlistsMigrated()) {
        bool canMarkMigrated = true;
        bool imported = false;
        if (music.isEmpty()) {
            const QVector<MediaItem> legacyMusic =
                playlistFromJson(AppSettings::playlist(MediaType::Music));
            if (!legacyMusic.isEmpty()) {
                music = legacyMusic;
                imported = true;
            }
        }
        if (video.isEmpty()) {
            const QVector<MediaItem> legacyVideo =
                playlistFromJson(AppSettings::playlist(MediaType::Video));
            if (!legacyVideo.isEmpty()) {
                video = legacyVideo;
                imported = true;
            }
        }
        if (imported) {
            canMarkMigrated = PlaylistStore::saveAll(music, video);
            if (canMarkMigrated)
                qCInfo(lcWindow) << "已把旧 QSettings 播放列表迁移到 Data/playlists.json";
            else
                qCWarning(lcWindow) << "播放列表迁移失败，保留旧 QSettings 数据待下次重试";
        }
        if (canMarkMigrated) {
            AppSettings::setPlaylistsMigrated(true);
            AppSettings::clearLegacyPlaylists();
        }
    }

    m_musicModel->setItems(music);
    m_videoModel->setItems(video);

    if (!music.isEmpty() || !video.isEmpty()) {
        ui->label_status->setText(tr("已恢复上次的播放列表：音乐 %1 首，视频 %2 个")
                                      .arg(music.size())
                                      .arg(video.size()));
    }
}

bool MainWindow::savePlaylists()
{
    if (PlaylistStore::saveAll(m_musicModel->items(), m_videoModel->items()))
        return true;

    qCWarning(lcWindow) << "播放列表保存失败";
    if (ui != nullptr && ui->label_status != nullptr)
        ui->label_status->setText(tr("播放列表保存失败：Data 目录不可写"));
    return false;
}

void MainWindow::setupPlaylistInteractions()
{
    auto bindView = [this](QListView *view, PlaylistModel *model, MediaType type) {
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, view, model, type](const QPoint &pos) {
                    showPlaylistContextMenu(type, view, model, pos);
                });

        // 选中一行后按 Delete 直接移除
        auto *shortcut = new QShortcut(QKeySequence::Delete, view);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, view, type] {
            const QModelIndex index = view->currentIndex();
            if (index.isValid())
                removePlaylistRow(type, index.row());
        });
    };

    bindView(ui->list_music, m_musicModel, MediaType::Music);
    bindView(ui->list_video, m_videoModel, MediaType::Video);
}

void MainWindow::removePlaylistRow(MediaType type, int row)
{
    PlaylistModel *model = type == MediaType::Music ? m_musicModel : m_videoModel;
    const QString name = model->itemAt(row).displayName();
    const bool removingCurrent = m_player->playlist() == model && m_player->currentRow() == row;

    if (!model->removeRow(row))
        return;

    if (m_player->playlist() == model)
        m_player->onPlaylistRowRemoved(row);

    if (removingCurrent) {
        // 停播信号只负责清画面；进度条、时间和歌词仍会停在已删除的那一行
        QSignalBlocker blocker(ui->slider_progress);
        ui->slider_progress->setValue(0);
        ui->label_time->setText(QStringLiteral("00:00 / 00:00"));
        ui->list_lrc->clear();
        m_lrcLines.clear();
        m_lrcIndex = -1;
    }

    if (savePlaylists()) {
        ui->label_status->setText(
            removingCurrent ? tr("已移除当前播放项：%1").arg(name)
                            : tr("已从列表移除：%1").arg(name));
    }
}

void MainWindow::clearPlaylist(MediaType type)
{
    PlaylistModel *model = type == MediaType::Music ? m_musicModel : m_videoModel;
    if (model->isEmpty())
        return;

    if (m_player->playlist() == model) {
        m_player->stop();
        m_player->setPlaylist(nullptr);
        ui->list_lrc->clear();
        m_lrcLines.clear();
        m_lrcIndex = -1;
    }

    model->clear();
    if (savePlaylists())
        ui->label_status->setText(tr("已清空列表"));
}

void MainWindow::showPlaylistContextMenu(MediaType type, QListView *view, PlaylistModel *model,
                                         const QPoint &pos)
{
    const QModelIndex index = view->indexAt(pos);

    QMenu menu(this);
    QAction *playAction = nullptr;
    QAction *removeAction = nullptr;
    if (index.isValid()) {
        playAction = menu.addAction(tr("播放"));
        removeAction = menu.addAction(tr("从列表移除"));
        menu.addSeparator();
    }
    QAction *clearAction = menu.addAction(tr("清空列表"));
    clearAction->setEnabled(!model->isEmpty());
    menu.addSeparator();

    // 菜单跟着当前列表走：音乐列表只给音乐入口，视频列表只给视频入口
    QAction *addLocalAction = menu.addAction(type == MediaType::Music ? tr("加载本地音乐…")
                                                                     : tr("加载本地视频…"));

    QAction *chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (chosen == nullptr)
        return;

    if (chosen == playAction) {
        m_player->setPlaylist(model);
        m_player->playRow(index.row());
    } else if (chosen == removeAction) {
        removePlaylistRow(type, index.row());
    } else if (chosen == clearAction) {
        clearPlaylist(type);
    } else if (chosen == addLocalAction) {
        if (type == MediaType::Music)
            openLocalMusic();
        else
            openLocalVideo();
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_fullScreenWindow != nullptr && watched == m_fullScreenWindow) {
        if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                exitFullScreen();
                return true;
            }
        } else if (event->type() == QEvent::Close) {
            // Alt+F4 关掉全屏窗口时，也要把视频面放回舞台
            exitFullScreen();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

PlaylistModel *MainWindow::activeModel() const
{
    return ui->tab_media->currentIndex() == 1 ? m_videoModel : m_musicModel;
}

QListView *MainWindow::activeView() const
{
    return ui->tab_media->currentIndex() == 1 ? ui->list_video : ui->list_music;
}

void MainWindow::ensurePlaylist()
{
    if (m_player->playlist() == nullptr) {
        m_player->setPlaylist(activeModel());
        return;
    }
    if (!m_player->hasCurrentItem() && m_player->playlist() != activeModel())
        m_player->setPlaylist(activeModel());
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
    addLocalFiles(MediaType::Music, files);
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
    addLocalFiles(MediaType::Video, files);
}

void MainWindow::addLocalFiles(MediaType type, const QStringList &files)
{
    PlaylistModel *model = type == MediaType::Music ? m_musicModel : m_videoModel;
    int added = 0;

    for (const QString &path : files) {
        const QFileInfo info(path);
        MediaItem item;
        item.type = type;
        item.title = info.fileName();
        item.url = QUrl::fromLocalFile(info.absoluteFilePath());
        item.online = false;

        // 同名 .lrc 作为本地歌词
        const QString lyricPath = info.absolutePath() + QLatin1Char('/')
                                + info.completeBaseName() + QStringLiteral(".lrc");
        if (QFileInfo::exists(lyricPath))
            item.lyricUrl = QUrl::fromLocalFile(lyricPath);

        if (model->addItem(item))
            ++added;
    }

    if (added > 0) {
        if (savePlaylists())
            ui->label_status->setText(tr("已添加 %1 个文件").arg(added));
    } else {
        ui->label_status->setText(tr("列表中已存在相同文件"));
    }
    qCInfo(lcWindow) << "加载本地文件" << (type == MediaType::Music ? "音乐" : "视频") << "共"
                     << files.size() << "个，新增" << added << "个";
}

void MainWindow::on_btn_prev_clicked()
{
    ensurePlaylist();
    m_player->previous();
}

void MainWindow::on_btn_play_clicked()
{
    if (!m_player->hasCurrentItem()) {
        PlaylistModel *model = activeModel();
        if (model == nullptr || model->isEmpty())
            return;
        m_player->setPlaylist(model);
        const QModelIndex current = activeView()->currentIndex();
        m_player->playRow(current.isValid() ? current.row() : 0);
        return;
    }
    m_player->togglePlayPause();
}

void MainWindow::on_btn_stop_clicked()
{
    m_player->stop();
    {
        QSignalBlocker blocker(ui->slider_progress);
        ui->slider_progress->setValue(0);
    }
    ui->label_time->setText(QStringLiteral("00:00 / 00:00"));
}

void MainWindow::on_btn_next_clicked()
{
    ensurePlaylist();
    m_player->next();
}

void MainWindow::on_btn_mode_clicked()
{
    using Mode = PlaybackController::Mode;
    Mode nextMode = Mode::ListLoop;
    switch (m_player->mode()) {
    case Mode::ListLoop:
        nextMode = Mode::SingleLoop;
        break;
    case Mode::SingleLoop:
        nextMode = Mode::Random;
        break;
    case Mode::Random:
        nextMode = Mode::ListLoop;
        break;
    }
    m_player->setMode(nextMode);
}

void MainWindow::on_btn_fullscreen_clicked()
{
    toggleFullScreen();
}

void MainWindow::updateEdgePanels()
{
    if (!m_sidebarAutoHide && !m_controlsAutoHide)
        return;

    const QPoint local = ui->centralwidget->mapFromGlobal(QCursor::pos());
    const bool inside = ui->centralwidget->rect().contains(local);

    // 左侧栏：鼠标贴到窗口左边缘弹出，移出侧栏范围就收回去
    if (m_sidebarAutoHide) {
        if (!m_sidebarPeek && inside && local.x() <= 10) {
            positionSidebarOverlay();
            ui->panelSidebar->show();
            ui->panelSidebar->raise();
            m_sidebarPeek = true;
        } else if (m_sidebarPeek
                   && (!inside || local.x() > ui->panelSidebar->width() + 8)) {
            ui->panelSidebar->hide();
            m_sidebarPeek = false;
        }
    }

    // 底部控制条：贴到底边弹出，鼠标离开控制条范围就收回
    if (m_controlsAutoHide) {
        const bool controlsVisible = ui->panelTransport->isVisible();
        if (!controlsVisible && inside && local.y() >= ui->centralwidget->height() - 20) {
            positionControlOverlay();
            ui->panelTransport->show();
            ui->panelTransport->raise();
        } else if (controlsVisible
                   && (!inside
                       || !ui->panelTransport->geometry().adjusted(-8, -8, 8, 8).contains(local))) {
            ui->panelTransport->hide();
        }
    }
}

void MainWindow::applyPanelVisibility()
{
    m_sidebarAutoHide = AppSettings::sidebarAutoHide();
    m_controlsAutoHide = AppSettings::controlsAutoHide();

    // 收起 = 浮层（贴边弹出、不参与布局）；常显 = 回布局（正常占位）
    setSidebarFloating(m_sidebarAutoHide);
    setControlsFloating(m_controlsAutoHide);

    m_sidebarPeek = false;
    ui->panelSidebar->setVisible(!m_sidebarAutoHide);
    ui->panelTransport->setVisible(!m_controlsAutoHide);

    if (m_sidebarAutoHide)
        positionSidebarOverlay();
    if (m_controlsAutoHide)
        positionControlOverlay();

    // 两侧都常显时没有任何贴边检测需求，停掉 120ms 定时器
    if (m_edgeTimer != nullptr) {
        if (m_sidebarAutoHide || m_controlsAutoHide) {
            if (!m_edgeTimer->isActive())
                m_edgeTimer->start();
        } else {
            m_edgeTimer->stop();
        }
    }

    if (ui->panelSidebar->isVisible())
        ui->panelSidebar->raise();
    if (ui->panelTransport->isVisible())
        ui->panelTransport->raise();
}

void MainWindow::setSidebarFloating(bool floating)
{
    if (floating == m_sidebarFloating)
        return;

    m_sidebarFloating = floating;
    m_sidebarPeek = false;

    if (floating) {
        // 浮层：从布局里摘出来，挂到 centralwidget 上，由 positionSidebarOverlay() 定位
        ui->layout_root->removeWidget(ui->panelSidebar);
        ui->panelSidebar->setParent(ui->centralwidget);
    } else {
        // 占布局：插回原来的位置（布局最左）
        ui->layout_root->insertWidget(0, ui->panelSidebar);
    }

    // QSS 用 [floating="true"] 区分两种底色：浮层压画面要更实，占布局保持淡玻璃
    ui->panelSidebar->setProperty("floating", floating);
    if (QStyle *style = ui->panelSidebar->style()) {
        style->unpolish(ui->panelSidebar);
        style->polish(ui->panelSidebar);
    }
    ui->panelSidebar->update();

    positionSidebarOverlay();
}

void MainWindow::setControlsFloating(bool floating)
{
    if (floating == m_controlsFloating)
        return;

    m_controlsFloating = floating;

    if (floating) {
        ui->layout_center->removeWidget(ui->panelTransport);
        ui->panelTransport->setParent(ui->centralwidget);
        positionControlOverlay();
    } else {
        // 回布局时放在舞台下方，恢复原来的上下排布
        ui->layout_center->addWidget(ui->panelTransport);
    }
}

void MainWindow::positionSidebarOverlay()
{
    if (ui->panelSidebar == nullptr || !m_sidebarFloating)
        return;

    const int margin = 16;
    const int width = 280;
    ui->panelSidebar->setGeometry(margin, margin, width,
                                  qMax(240, ui->centralwidget->height() - margin * 2));
}

void MainWindow::positionControlOverlay()
{
    if (ui->panelStage == nullptr || ui->panelTransport == nullptr || !m_controlsFloating)
        return;

    // 舞台是 centralwidget 的直接子控件，控制条挂在同一层，坐标可以直接对齐
    const QRect stage = ui->panelStage->geometry();
    const int margin = 8;
    const int height = qBound(112, ui->panelTransport->sizeHint().height(), 150);
    const QRect target(stage.left() + margin, stage.bottom() - height - margin,
                       qMax(240, stage.width() - margin * 2), height);
    ui->panelTransport->setGeometry(target);
    ui->panelTransport->raise();
}

void MainWindow::toggleFullScreen()
{
    if (m_fullScreenWindow != nullptr)
        exitFullScreen();
    else
        enterFullScreen();
}

void MainWindow::enterFullScreen()
{
    // 视频面是自绘控件，不再走 QVideoWidget 的原生全屏；
    // 把它临时搬进一个全屏窗口，退出时再放回舞台布局。
    auto *window = new QWidget(nullptr, Qt::Window);
    window->setObjectName(QStringLiteral("fullScreenWindow"));
    window->setWindowTitle(tr("全屏播放"));
    window->setStyleSheet(QStringLiteral("QWidget#fullScreenWindow { background: black; }"));

    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    ui->videoWidget->setParent(window);
    ui->videoWidget->setCornerRadius(0.0);
    layout->addWidget(ui->videoWidget);
    ui->videoWidget->show();

    window->installEventFilter(this);
    window->showFullScreen();
    window->setFocus();

    m_fullScreenWindow = window;
}

void MainWindow::exitFullScreen()
{
    if (m_fullScreenWindow == nullptr)
        return;

    QWidget *window = m_fullScreenWindow;
    m_fullScreenWindow = nullptr;

    window->removeEventFilter(this);
    ui->layout_videoStage->addWidget(ui->videoWidget);
    ui->videoWidget->setCornerRadius(16.0);
    ui->videoWidget->show();

    window->close();
    window->deleteLater();
}

void MainWindow::refreshModeButton()
{
    ui->btn_mode->setText(m_player->modeText());
}

void MainWindow::playSearchItem(const MediaItem &item)
{
    if (!item.isValid())
        return;

    PlaylistModel *model = item.type == MediaType::Music ? m_musicModel : m_videoModel;
    const bool added = model->addItem(item); // 已存在时返回 false，不影响下面的定位
    const int row = model->indexOf(item);
    if (row < 0)
        return;

    if (added)
        savePlaylists();

    m_player->setPlaylist(model);
    m_player->playRow(row);
    ui->tab_media->setCurrentIndex(item.type == MediaType::Music ? 0 : 1);
}

void MainWindow::addSearchItemToPlaylist(const MediaItem &item, MediaType type)
{
    MediaItem copy = item;
    copy.type = type;
    PlaylistModel *model = type == MediaType::Music ? m_musicModel : m_videoModel;
    if (!model->addItem(copy)) {
        ui->label_status->setText(type == MediaType::Music ? tr("音乐列表中已存在")
                                                         : tr("视频列表中已存在"));
        return;
    }
    if (savePlaylists())
        ui->label_status->setText(type == MediaType::Music ? tr("已加入音乐列表")
                                                         : tr("已加入视频列表"));
}

void MainWindow::downloadItem(const MediaItem &item)
{
    if (!item.online) {
        ui->label_status->setText(tr("本地文件无需下载"));
        return;
    }

    const bool hls = DownloadTask::looksLikeHls(item.url);
    const QString suffix = hls ? QStringLiteral("ts")
                               : (item.type == MediaType::Video ? QStringLiteral("mp4")
                                                                : QStringLiteral("mp3"));
    const QString filter = hls ? tr("视频文件 (*.ts *.mp4)")
                               : tr("媒体文件 (*.%1)").arg(suffix);
    const QString suggested =
        QDir(m_lastDir).filePath(DownloadTask::sanitizeFileName(item.displayName(), suffix));
    const QString target = QFileDialog::getSaveFileName(this, tr("保存文件"), suggested, filter);
    if (target.isEmpty())
        return;

    m_lastDir = QFileInfo(target).absolutePath();
    QString finalTarget = target;
    if (hls) {
        if (!finalTarget.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive)
            && !finalTarget.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) {
            finalTarget += QLatin1String(".ts");
        }
    } else if (!finalTarget.endsWith(QStringLiteral(".") + suffix, Qt::CaseInsensitive)) {
        finalTarget += QStringLiteral(".") + suffix;
    }

    qCInfo(lcWindow) << "开始下载" << item.displayName() << "到" << finalTarget
                     << (hls ? "（HLS 分片合并）" : "");
    auto *task = DownloadTask::start(m_http, item.url, finalTarget, item.headers, this);
    m_download = task;

    connect(task, &DownloadTask::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0)
            ui->label_status->setText(tr("下载中 %1%").arg(static_cast<int>(received * 100 / total)));
        else
            ui->label_status->setText(tr("下载中 %1 KB").arg(received / 1024));
    });
    connect(task, &DownloadTask::succeeded, this, [this](const QString &path) {
        ui->label_status->setText(tr("下载完成：%1").arg(QDir::toNativeSeparators(path)));
    });
    connect(task, &DownloadTask::failed, this, [this](const QString &message) {
        ui->label_status->setText(tr("下载失败：%1").arg(message));
    });
    connect(task, &DownloadTask::finished, this, [this, task] {
        if (m_download == task)
            m_download.clear();
    });
}

void MainWindow::updateLyrics(qint64 positionMs)
{
    if (m_lrcLines.isEmpty())
        return;

    const int index = LrcParser::indexAt(m_lrcLines, positionMs);
    if (index == m_lrcIndex || index < 0)
        return;

    if (m_lrcIndex >= 0 && m_lrcIndex < ui->list_lrc->count()) {
        if (auto *previous = ui->list_lrc->item(m_lrcIndex))
            previous->setForeground(QBrush());
    }

    m_lrcIndex = index;
    if (auto *current = ui->list_lrc->item(index)) {
        current->setForeground(QBrush(ThemeManager::instance().current().accentColor));
        ui->list_lrc->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    }
}
