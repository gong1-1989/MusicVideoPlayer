#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "core/PlaylistStore.h"
#include "download/DownloadTask.h"
#include "lyrics/LyricsService.h"
#include "net/HttpClient.h"
#include "net/MusicSearchService.h"
#include "settings/AppSettings.h"
#include "settings/WindowPreset.h"
#include "ui/SettingsDialog.h"
#include "ui/ThemeManager.h"
#include "ui/VideoStage.h"
#include "util/TimeFormat.h"

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QMenu>
#include <QModelIndex>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {

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

QByteArray playlistToJson(const PlaylistModel *model)
{
    QJsonArray array;
    const QVector<MediaItem> items = model->items();
    for (const MediaItem &item : items)
        array.append(mediaItemToJson(item));
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_http(new HttpClient(this))
    , m_search(new MusicSearchService(m_http, this))
    , m_lyrics(new LyricsService(m_http, this))
    , m_player(new PlaybackController(this))
    , m_musicModel(new PlaylistModel(this))
    , m_videoModel(new PlaylistModel(this))
    , m_searchModel(new PlaylistModel(this))
{
    ui->setupUi(this);

    ui->list_search->setModel(m_searchModel);    ui->list_music->setModel(m_musicModel);
    ui->list_video->setModel(m_videoModel);
    ui->list_search->setContextMenuPolicy(Qt::CustomContextMenu);

    m_player->mediaPlayer()->setVideoSink(ui->videoWidget->sink());

    // 自绘控件的几何参数：圆角半径与歌词渐隐高度不再散落在各处
    ui->videoStage->setCornerRadius(16.0);
    ui->list_lrc->setFadeExtent(28);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this] { highlightCurrentLyric(); });

    auto *settingsShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    connect(settingsShortcut, &QShortcut::activated, this, &MainWindow::on_btn_settings_clicked);

    setupConnections();
    restoreSettings();
    restorePlaylists();
    setupPlaylistInteractions();
    refreshModeButton();
}

MainWindow::~MainWindow()
{
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
            });
    connect(m_player, &PlaybackController::modeChanged, this, [this] { refreshModeButton(); });
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                ui->label_status->setText(tr("正在播放：%1").arg(item.displayName()));

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
        qWarning() << "lyrics failed:" << message;
    });

    connect(m_search, &MusicSearchService::searchStarted, this, [this] {
        ui->btn_search->setEnabled(false);
        ui->label_status->setText(tr("搜索中…"));
    });
    connect(m_search, &MusicSearchService::searchSucceeded, this,
            [this](const QVector<MediaItem> &items) {
                m_searchModel->clear();
                for (const MediaItem &item : items)
                    m_searchModel->addItem(item);
                const int filtered = m_search->lastFilteredCount();
                if (items.isEmpty()) {
                    ui->label_status->setText(filtered > 0
                                                  ? tr("未搜索到可播放结果（已过滤 %1 首 VIP/无音源）")
                                                        .arg(filtered)
                                                  : tr("未搜索到结果"));
                } else if (filtered > 0) {
                    ui->label_status->setText(tr("搜索完成，共 %1 首（已过滤 %2 首 VIP/无音源）")
                                                  .arg(items.size())
                                                  .arg(filtered));
                } else {
                    ui->label_status->setText(tr("搜索完成，共 %1 首").arg(items.size()));
                }
            });
    connect(m_search, &MusicSearchService::searchFailed, this, [this](const QString &message) {
        ui->label_status->setText(tr("搜索失败：%1").arg(message));
    });
    connect(m_search, &MusicSearchService::searchFinished, this, [this] {
        ui->btn_search->setEnabled(true);
    });

    connect(ui->edit_search, &QLineEdit::returnPressed,
            this, &MainWindow::on_btn_search_clicked);

    connect(ui->list_search, &QWidget::customContextMenuRequested,
            this, &MainWindow::showSearchContextMenu);
    connect(ui->list_search, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (index.isValid())
                    playSearchItem(m_searchModel->itemAt(index.row()));
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

    m_search->setResultLimit(AppSettings::searchLimit());

    // 上次会话的音乐/视频列表
    m_musicModel->setItems(playlistFromJson(AppSettings::playlist(MediaType::Music)));
    m_videoModel->setItems(playlistFromJson(AppSettings::playlist(MediaType::Video)));

    applyWindowPreset();
}

void MainWindow::applyWindowPreset()
{
    const WindowPreset preset = AppSettings::windowPreset();
    const QSize presetSize = AppSettings::presetWindowSize(preset);

    if (preset == WindowPreset::Last || !presetSize.isValid()) {
        const QByteArray geometry = AppSettings::savedGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        return;
    }

    // 预设只是目标值：屏幕装不下时按可用工作区收一收
    const QSize target = fitToAvailableScreen(presetSize);
    resize(target);

    // 固定尺寸下没有历史位置可恢复，首次启动居中显示
    if (AppSettings::savedGeometry().isEmpty()) {
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            move(available.center() - QPoint(target.width() / 2, target.height() / 2));
        }
    }
}

void MainWindow::saveSettings()
{
    AppSettings::setLastDir(m_lastDir);
    AppSettings::setDefaultVolume(m_player->volumePercent());
    AppSettings::setPlaybackMode(static_cast<int>(m_player->mode()));
    AppSettings::setPlaylist(MediaType::Music, playlistToJson(m_musicModel));
    AppSettings::setPlaylist(MediaType::Video, playlistToJson(m_videoModel));

    // 只有"记住上次"才写几何，固定尺寸预设下写进去会互相打架
    if (AppSettings::windowPreset() == WindowPreset::Last)
        AppSettings::setSavedGeometry(saveGeometry());
}

void MainWindow::applySettings()
{
    ThemeManager::instance().apply(AppSettings::themeId());

    const QSize presetSize = AppSettings::presetWindowSize(AppSettings::windowPresetId());
    if (presetSize.isValid())
        resize(fitToAvailableScreen(presetSize));

    m_player->setVolumePercent(AppSettings::defaultVolume());
    {
        QSignalBlocker blocker(ui->slider_volume);
        ui->slider_volume->setValue(m_player->volumePercent());
    }

    m_search->setResultLimit(AppSettings::searchLimit());
    highlightCurrentLyric();
}

void MainWindow::on_btn_settings_clicked()
{
    SettingsDialog dialog(this);
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
    savePlaylists();
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::restorePlaylists()
{
    const QVector<MediaItem> music = PlaylistStore::load(MediaType::Music);
    const QVector<MediaItem> video = PlaylistStore::load(MediaType::Video);
    m_musicModel->setItems(music);
    m_videoModel->setItems(video);

    if (!music.isEmpty() || !video.isEmpty()) {
        ui->label_status->setText(tr("已恢复上次的播放列表：音乐 %1 首，视频 %2 个")
                                      .arg(music.size())
                                      .arg(video.size()));
    }
}

void MainWindow::savePlaylists()
{
    PlaylistStore::saveAll(m_musicModel->items(), m_videoModel->items());
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

    if (!model->removeRow(row))
        return;

    if (m_player->playlist() == model)
        m_player->onPlaylistRowRemoved(row);

    savePlaylists();
    ui->label_status->setText(tr("已从列表移除：%1").arg(name));
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
    savePlaylists();
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

void MainWindow::on_btn_search_clicked()
{
    const QString keyword = ui->edit_search->text().trimmed();
    if (keyword.isEmpty()) {
        ui->label_status->setText(tr("请输入搜索关键词"));
        return;
    }
    m_search->search(keyword);
}

void MainWindow::on_btn_openMusic_clicked()
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

void MainWindow::on_btn_openVideo_clicked()
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

    ui->label_status->setText(added > 0 ? tr("已添加 %1 个文件").arg(added)
                                        : tr("列表中已存在相同文件"));
    if (added > 0)
        savePlaylists();
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
    const int row = model->indexOf(item.url);
    if (row < 0)
        return;

    if (added)
        savePlaylists();

    m_player->setPlaylist(model);
    m_player->playRow(row);
    ui->tab_media->setCurrentIndex(item.type == MediaType::Music ? 0 : 1);
}

void MainWindow::showSearchContextMenu(const QPoint &pos)
{
    const QModelIndex index = ui->list_search->indexAt(pos);
    if (!index.isValid())
        return;

    const MediaItem item = m_searchModel->itemAt(index.row());
    if (!item.isValid())
        return;

    QMenu menu(this);
    QAction *playAction = menu.addAction(tr("播放"));
    QAction *addMusicAction = menu.addAction(tr("加入音乐列表"));
    QAction *addVideoAction = menu.addAction(tr("加入视频列表"));
    addVideoAction->setEnabled(item.type == MediaType::Video);
    QAction *downloadAction = menu.addAction(tr("下载"));

    QAction *chosen = menu.exec(ui->list_search->viewport()->mapToGlobal(pos));
    if (chosen == nullptr)
        return;

    if (chosen == playAction) {
        playSearchItem(item);
    } else if (chosen == addMusicAction) {
        MediaItem music = item;
        music.type = MediaType::Music;
        const bool ok = m_musicModel->addItem(music);
        ui->label_status->setText(ok ? tr("已加入音乐列表") : tr("音乐列表中已存在"));
        if (ok)
            savePlaylists();
    } else if (chosen == addVideoAction) {
        MediaItem video = item;
        video.type = MediaType::Video;
        const bool ok = m_videoModel->addItem(video);
        ui->label_status->setText(ok ? tr("已加入视频列表") : tr("视频列表中已存在"));
        if (ok)
            savePlaylists();
    } else if (chosen == downloadAction) {
        downloadItem(item);
    }
}

void MainWindow::downloadItem(const MediaItem &item)
{
    if (!item.online) {
        ui->label_status->setText(tr("本地文件无需下载"));
        return;
    }

    const QString suffix = item.type == MediaType::Video ? QStringLiteral("mp4")
                                                        : QStringLiteral("mp3");
    const QString suggested =
        QDir(m_lastDir).filePath(DownloadTask::sanitizeFileName(item.displayName(), suffix));
    const QString target = QFileDialog::getSaveFileName(
        this, tr("保存文件"), suggested, tr("媒体文件 (*.%1)").arg(suffix));
    if (target.isEmpty())
        return;

    m_lastDir = QFileInfo(target).absolutePath();

    auto *task = DownloadTask::start(m_http, item.url, target, this);
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
