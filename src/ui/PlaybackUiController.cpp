#include "ui/PlaybackUiController.h"

#include "core/PlaylistModel.h"
#include "ui/LyricsController.h"
#include "ui/PlaylistController.h"
#include "ui/ThemeManager.h"
#include "ui/UiIcons.h"
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

    // 连续点击进度条时先把目标攒一下：每次点击都立刻 seek 会反复取消
    // StreamProxy 的预取，FFmpeg 正在读的连接被中断后容易整条播放链崩溃。
    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    m_seekTimer->setInterval(180);
    connect(m_seekTimer, &QTimer::timeout, this, &PlaybackUiController::flushSeek);

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
                refreshPlayButton(state);
                if (state == QMediaPlayer::StoppedState) {
                    m_seekTarget = -1;
                    // 拖动中停止后鼠标可能不会再发 sliderReleased，必须复位，
                    // 否则下一次播放的 positionChanged 永远不会再更新进度条。
                    m_userSeeking = false;
                    // 停止后播放器不会再送新帧；不清掉的话上一部视频的最后一帧会一直留在面板上
                    m_surface->clearFrame();
                } else {
                    // 新一次播放开始，允许视频面重新接收帧
                    m_surface->resumeFrames();
                }
            });
    connect(m_player, &PlaybackController::currentItemChanged, this,
            [this](const MediaItem &item, int row) {
                m_seekTarget = -1;
                m_seekPendingTarget = -1;
                m_seekTimer->stop();
                // 切歌时同样要清掉“用户正在拖动”的标记，避免旧拖动状态
                // 把新歌的进度条更新一直挡住。
                m_userSeeking = false;
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

    // 主题切换时内置图标需要按新强调色重画；外部自定义图标也重新解析。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const ThemeInfo &) {
                refreshIcons();
                refreshPlayButton(m_player->mediaPlayer()->playbackState());
            });

    refreshIcons();
    refreshPlayButton(m_player->mediaPlayer()->playbackState());
}

void PlaybackUiController::clear()
{
    m_seekTarget = -1;
    m_seekPendingTarget = -1;
    if (m_seekTimer != nullptr)
        m_seekTimer->stop();
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

    m_seekPendingTarget = target;

    // HLS 需要把“正在定位”立刻告诉用户；真正下发统一交给防抖计时器。
    if (isHlsItem()) {
        m_ui.time->setText(tr("定位中 %1 / %2")
                               .arg(TimeFormat::formatMs(target),
                                    TimeFormat::formatMs(m_player->duration())));
        emit statusMessage(tr("正在定位到 %1，源站分片较慢时请稍候…")
                               .arg(TimeFormat::formatMs(target)));
    }

    // 180ms 内的后续点击只覆盖目标，不发出第一次 seek；
    // 这是用户“没加载完又点一次就崩”的直接保护。
    m_seekTimer->start();
}

void PlaybackUiController::flushSeek()
{
    const qint64 target = m_seekPendingTarget;
    m_seekPendingTarget = -1;
    if (target < 0 || !m_player->hasCurrentItem())
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
    // 判断实际播放地址：条目原始 URL 可能是 HLS，但备选有 MP4 时会优先播 MP4，
    // 这时不能进入 HLS 的“定位中”等待。
    return playbackIsHls(m_player->currentItem());
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
    m_seekPendingTarget = -1;
    m_seekTimer->stop();
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
    QString iconName = QStringLiteral("mode-list");
    QString tooltip = tr("列表循环（播完自动下一首，点击切换）");
    switch (m_player->mode()) {
    case PlaybackController::Mode::SingleLoop:
        iconName = QStringLiteral("mode-single");
        tooltip = tr("单曲循环（当前视频/歌曲重复播放，点击切换）");
        break;
    case PlaybackController::Mode::Random:
        iconName = QStringLiteral("mode-random");
        tooltip = tr("随机播放（下一首随机选择，点击切换）");
        break;
    case PlaybackController::Mode::ListLoop:
    default:
        break;
    }
    UiIcons::apply(m_ui.mode, iconName, tooltip, 20);
}

void PlaybackUiController::refreshPlayButton(QMediaPlayer::PlaybackState state)
{
    const bool playing = state == QMediaPlayer::PlayingState;
    UiIcons::apply(m_ui.play, playing ? QStringLiteral("pause") : QStringLiteral("play"),
                   playing ? tr("暂停") : tr("播放"), 26, QColor(255, 255, 255));
}

void PlaybackUiController::refreshIcons()
{
    UiIcons::apply(m_ui.previous, QStringLiteral("prev"), tr("上一首 / 上一个视频"), 20);
    UiIcons::apply(m_ui.stop, QStringLiteral("stop"), tr("停止播放"), 20);
    UiIcons::apply(m_ui.next, QStringLiteral("next"), tr("下一首 / 下一个视频"), 20);
    refreshModeButton();
}
