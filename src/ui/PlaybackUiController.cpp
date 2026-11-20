#include "ui/PlaybackUiController.h"

#include "core/PlaylistModel.h"
#include "ui/LyricsController.h"
#include "ui/PlaylistController.h"
#include "ui/VideoSurface.h"
#include "util/TimeFormat.h"

#include <QLabel>
#include <QListView>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

PlaybackUiController::PlaybackUiController(PlaybackController *player,
                                           PlaylistController *playlists,
                                           VideoSurface *surface, LyricsController *lyrics,
                                           const PlaybackUi &ui, QObject *parent)
    : QObject(parent)
    , m_ui(ui)
    , m_player(player)
    , m_playlists(playlists)
    , m_surface(surface)
    , m_lyrics(lyrics)
{
    Q_ASSERT(m_player != nullptr);
    Q_ASSERT(m_playlists != nullptr);
    Q_ASSERT(m_surface != nullptr);
    Q_ASSERT(m_lyrics != nullptr);
    Q_ASSERT(m_ui.progress != nullptr);
    Q_ASSERT(m_ui.volume != nullptr);
    Q_ASSERT(m_ui.time != nullptr);
    Q_ASSERT(m_ui.play != nullptr);
    Q_ASSERT(m_ui.stop != nullptr);
    Q_ASSERT(m_ui.previous != nullptr);
    Q_ASSERT(m_ui.next != nullptr);
    Q_ASSERT(m_ui.mode != nullptr);

    connect(m_ui.previous, &QPushButton::clicked, this, [this] {
        m_playlists->ensurePlaylist();
        m_player->previous();
    });
    connect(m_ui.play, &QPushButton::clicked, this, &PlaybackUiController::playPressed);
    connect(m_ui.stop, &QPushButton::clicked, this, &PlaybackUiController::stopPressed);
    connect(m_ui.next, &QPushButton::clicked, this, [this] {
        m_playlists->ensurePlaylist();
        m_player->next();
    });
    connect(m_ui.mode, &QPushButton::clicked, this, &PlaybackUiController::modePressed);

    // 音量：单向 slider -> controller，避免双向回环
    connect(m_ui.volume, &QSlider::valueChanged,
            m_player, &PlaybackController::setVolumePercent);

    // 进度条：拖动期间不回写，松手才 seek
    connect(m_ui.progress, &QSlider::sliderPressed, this,
            [this] { m_userSeeking = true; });
    connect(m_ui.progress, &QSlider::sliderReleased, this, [this] {
        m_userSeeking = false;
        m_player->seek(m_ui.progress->value());
    });
    connect(m_ui.progress, &QSlider::sliderMoved, this, [this](int value) {
        m_ui.time->setText(TimeFormat::formatMs(value) + QStringLiteral(" / ")
                           + TimeFormat::formatMs(m_player->duration()));
    });
    // 点击凹槽直接跳转（该路径不一定触发 sliderPressed）
    connect(m_ui.progress, &QSlider::valueChanged, this, [this](int value) {
        if (m_userSeeking)
            return;
        if (qAbs(value - m_player->position()) > 1500)
            m_player->seek(value);
    });

    connect(m_player, &PlaybackController::positionChanged, this, [this](qint64 position) {
        if (!m_userSeeking) {
            QSignalBlocker blocker(m_ui.progress);
            m_ui.progress->setValue(static_cast<int>(position));
        }
        m_ui.time->setText(TimeFormat::formatMs(position) + QStringLiteral(" / ")
                           + TimeFormat::formatMs(m_player->duration()));
        m_lyrics->updatePosition(position);
    });
    connect(m_player, &PlaybackController::durationChanged, this, [this](qint64 duration) {
        QSignalBlocker blocker(m_ui.progress);
        m_ui.progress->setRange(0, static_cast<int>(qMax<qint64>(0, duration)));
    });
    connect(m_player, &PlaybackController::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                m_ui.play->setText(state == QMediaPlayer::PlayingState ? tr("暂停")
                                                                       : tr("播放"));
                if (state == QMediaPlayer::StoppedState) {
                    // 停止后播放器不会再送新帧；不清掉的话上一部视频的最后一帧会一直留在面板上
                    m_surface->clearFrame();
                    m_lyrics->hidePanel();
                } else {
                    // 新一次播放开始，允许视频面重新接收帧
                    m_surface->resumeFrames();
                    if (m_player->hasCurrentItem()
                        && m_player->currentItem().type == MediaType::Music)
                        m_lyrics->showPanel();
                }
            });
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                // 视频切歌/切回音乐时先清掉上一帧，等新视频首帧到达再显示
                m_surface->clearFrame();
                emit statusMessage(tr("正在播放：%1").arg(item.displayName()));
                m_lyrics->setCurrentItem(item);
                emit currentItemChanged(item, row);
            });
    connect(m_player, &PlaybackController::modeChanged, this,
            &PlaybackUiController::refreshModeButton);
    connect(m_player, &PlaybackController::errorOccurred, this,
            [this](const QString &message) {
                emit statusMessage(tr("播放失败：%1").arg(message));
            });

    refreshModeButton();
}

void PlaybackUiController::playPressed()
{
    if (!m_player->hasCurrentItem()) {
        PlaylistModel *model = m_playlists->activeModel();
        if (model == nullptr || model->isEmpty())
            return;
        m_player->setPlaylist(model);
        const QModelIndex current = m_playlists->activeView()->currentIndex();
        m_player->playRow(current.isValid() ? current.row() : 0);
        return;
    }
    m_player->togglePlayPause();
}

void PlaybackUiController::stopPressed()
{
    m_player->stop();
    QSignalBlocker blocker(m_ui.progress);
    m_ui.progress->setValue(0);
    m_ui.time->setText(QStringLiteral("00:00 / 00:00"));
}

void PlaybackUiController::modePressed()
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

void PlaybackUiController::refreshModeButton()
{
    m_ui.mode->setText(m_player->modeText());
}
