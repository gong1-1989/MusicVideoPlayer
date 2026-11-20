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
#include <QTimer>
#include <QUrl>

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
        beginSeek(m_ui.progress->value());
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
            beginSeek(value);
    });

    connect(m_player, &PlaybackController::positionChanged, this, [this](qint64 position) {
        if (m_seekTarget >= 0) {
            // 只能等位置真正向前走，不能拿 seek 瞬间回写的那次目标值当成“已就位”。
            if (position >= m_seekTarget + 400) {
                m_seekTarget = -1;
                emit statusMessage(tr("已定位到 %1").arg(TimeFormat::formatMs(position)));
            } else {
                m_ui.time->setText(tr("定位中 %1 / %2")
                                       .arg(TimeFormat::formatMs(m_seekTarget),
                                            TimeFormat::formatMs(m_player->duration())));
                m_lyrics->updatePosition(m_seekTarget);
                return;
            }
        }
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
                    m_seekTarget = -1;
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
                m_seekTarget = -1;
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
                m_seekTarget = -1;
                emit statusMessage(tr("播放失败：%1").arg(message));
            });

    refreshModeButton();
}

void PlaybackUiController::clear()
{
    m_seekTarget = -1;
    // 播放项被删除后播放器已无位置可报，进度条、时间和歌词必须一起归零，
    // 否则界面会停留在已删除的那一行。
    QSignalBlocker blocker(m_ui.progress);
    m_ui.progress->setValue(0);
    m_ui.time->setText(QStringLiteral("00:00 / 00:00"));
    m_lyrics->clear();
}

void PlaybackUiController::beginSeek(qint64 target)
{
    if (!m_player->hasCurrentItem() || target < 0)
        return;

    // 本地文件拖动几乎瞬间完成，只有 HLS 才需要把分片顺序读取的等待说清楚。
    if (!isHlsItem()) {
        m_player->seek(target);
        return;
    }

    m_seekTarget = target;
    const quint64 serial = ++m_seekSerial;
    const qint64 duration = m_player->duration();
    m_ui.time->setText(tr("定位中 %1 / %2")
                           .arg(TimeFormat::formatMs(target),
                                TimeFormat::formatMs(duration)));
    emit statusMessage(tr("正在定位到 %1，源站分片较慢时请稍候…")
                           .arg(TimeFormat::formatMs(target)));

    // 播放器卡在目标时间不动时不会再有 positionChanged，必须自己收尾，
    // 否则“定位中”会一直挂在界面上。
    QTimer::singleShot(45000, this, [this, target, serial] {
        if (m_seekSerial != serial || m_seekTarget != target)
            return;
        m_seekTarget = -1;
        const qint64 position = m_player->position();
        m_ui.time->setText(TimeFormat::formatMs(position) + QStringLiteral(" / ")
                           + TimeFormat::formatMs(m_player->duration()));
        emit statusMessage(tr("定位较慢：源站分片仍未就绪，画面恢复后会自动继续"));
    });

    m_player->seek(target);
}

bool PlaybackUiController::isHlsItem() const
{
    if (!m_player->hasCurrentItem())
        return false;
    const QUrl url = m_player->currentItem().url;
    return url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
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
    m_seekTarget = -1;
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
