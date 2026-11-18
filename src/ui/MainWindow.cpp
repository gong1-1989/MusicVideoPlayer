#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include "core/PlaylistModel.h"
#include "download/DownloadTask.h"
#include "lyrics/LyricsService.h"
#include "net/HttpClient.h"
#include "net/MusicSearchService.h"
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
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QModelIndex>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>

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

    ui->list_search->setModel(m_searchModel);
    ui->list_music->setModel(m_musicModel);
    ui->list_video->setModel(m_videoModel);
    ui->list_search->setContextMenuPolicy(Qt::CustomContextMenu);

    m_player->mediaPlayer()->setVideoOutput(ui->videoWidget);
    ui->videoWidget->installEventFilter(this);

    setupConnections();
    restoreSettings();
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
                ui->label_status->setText(items.isEmpty()
                                              ? tr("未搜索到结果")
                                              : tr("搜索完成，共 %1 首").arg(items.size()));
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
    QSettings settings;
    m_lastDir = settings.value(QStringLiteral("lastDir"),
                               QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
                    .toString();

    const QByteArray geometry = settings.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    m_player->setVolumePercent(settings.value(QStringLiteral("volume"), 60).toInt());
    ui->slider_volume->setValue(m_player->volumePercent());

    const int modeValue = settings.value(QStringLiteral("mode"),
                                         static_cast<int>(PlaybackController::Mode::ListLoop))
                              .toInt();
    m_player->setMode(static_cast<PlaybackController::Mode>(qBound(0, modeValue, 2)));
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("lastDir"), m_lastDir);
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("volume"), m_player->volumePercent());
    settings.setValue(QStringLiteral("mode"), static_cast<int>(m_player->mode()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->videoWidget && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape && ui->videoWidget->isFullScreen()) {
            ui->videoWidget->setFullScreen(false);
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
    // 不手工 setParent(nullptr)：那会把 videoWidget 从布局里摘掉，
    // 退出全屏后布局无法自愈。QVideoWidget 自己会处理顶层窗口切换。
    ui->videoWidget->setFullScreen(!ui->videoWidget->isFullScreen());
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
    model->addItem(item); // 已存在时返回 false，不影响下面的定位
    const int row = model->indexOf(item.url);
    if (row < 0)
        return;

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
        ui->label_status->setText(m_musicModel->addItem(music) ? tr("已加入音乐列表")
                                                               : tr("音乐列表中已存在"));
    } else if (chosen == addVideoAction) {
        MediaItem video = item;
        video.type = MediaType::Video;
        ui->label_status->setText(m_videoModel->addItem(video) ? tr("已加入视频列表")
                                                               : tr("视频列表中已存在"));
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
        current->setForeground(QBrush(QColor(0, 200, 255)));
        ui->list_lrc->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    }
}
