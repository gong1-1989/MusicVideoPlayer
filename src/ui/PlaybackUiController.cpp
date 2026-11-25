/// @file PlaybackUiController.cpp
/// @brief 播放控制条（进度、时间、播放/停止、模式、音量）与播放器的信号桥接。
#include "ui/PlaybackUiController.h"

#include "core/PlaylistModel.h"
#include "ui/LyricsController.h"
#include "ui/PlaylistController.h"
#include "ui/ThemeManager.h"
#include "ui/UiIcons.h"
#include "ui/VideoSurface.h"
#include "util/TimeFormat.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QListView>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QUrl>

/// @brief 构造控制器：创建 180ms seek 防抖定时器并连接全部播放相关信号。
/// @param player 播放控制核心。
/// @param playlists 播放列表控制器，用于空列表时选中行。
/// @param surface 视频画面，用于停止/切歌时清帧。
/// @param lyrics 歌词控制器，跟随播放位置高亮。
/// @param ui 控制条控件集合。
/// @param parent 父对象。
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
    Q_ASSERT(m_ui.seekBack != nullptr);
    Q_ASSERT(m_ui.seekForward != nullptr);
    Q_ASSERT(m_ui.mode != nullptr);

    // 连续点击进度条时先把目标攒一下：每次点击都立刻 seek 会反复取消
    // StreamProxy 的预取，FFmpeg 正在读的连接被中断后容易整条播放链崩溃。
    m_seekTimer = new QTimer(this);
    m_seekTimer->setSingleShot(true);
    m_seekTimer->setInterval(180);
    connect(m_seekTimer, &QTimer::timeout, this, &PlaybackUiController::flushSeek);

    // 按住超过 350ms 才算长按：短按仍保持原来的 10 秒单步跳转。
    m_forwardHoldTimer = new QTimer(this);
    m_forwardHoldTimer->setSingleShot(true);
    m_forwardHoldTimer->setInterval(350);
    connect(m_forwardHoldTimer, &QTimer::timeout, this, &PlaybackUiController::beginForwardHold);

    m_rewindHoldTimer = new QTimer(this);
    m_rewindHoldTimer->setSingleShot(true);
    m_rewindHoldTimer->setInterval(350);
    connect(m_rewindHoldTimer, &QTimer::timeout, this, &PlaybackUiController::beginRewindHold);

    // 快退长按按连续点击处理；500ms 间隔给 StreamProxy 留出预取切换时间，
    // 比 180ms 防抖更保守，避免 HLS 每秒多次打断分片连接。
    m_rewindRepeatTimer = new QTimer(this);
    m_rewindRepeatTimer->setInterval(500);
    connect(m_rewindRepeatTimer, &QTimer::timeout, this,
            &PlaybackUiController::advanceRewindHold);

    // 应用或主窗口失焦时鼠标释放不一定回到按钮，必须兜底恢复速率。
    if (qApp != nullptr)
        qApp->installEventFilter(this);

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

    // 快退：短按 -10 秒；长按先退 10 秒，之后每 500ms 再退 10 秒。
    connect(m_ui.seekBack, &QPushButton::pressed, this, [this] {
        m_rewindPressConsumed = false;
        m_rewindHoldTimer->start();
    });
    connect(m_ui.seekBack, &QPushButton::released, this, [this] {
        if (m_rewindHoldTimer->isActive()) {
            m_rewindHoldTimer->stop();
            seekBySeconds(-10);
        } else if (m_rewindHoldActive) {
            endRewindHold();
        }
        // 其他情况说明长按已被停止/切歌/失焦提前收尾，松手不再补跳。
    });

    // 快进：短按 +10 秒；播放中长按切 2×，松手恢复原速率；暂停/停止时
    // 长按无倍速意义，松手仍按一次普通快进处理。
    connect(m_ui.seekForward, &QPushButton::pressed, this, [this] {
        m_forwardPressConsumed = false;
        m_forwardHoldTimer->start();
    });
    connect(m_ui.seekForward, &QPushButton::released, this, [this] {
        if (m_forwardHoldActive) {
            endForwardHold();
        } else if (m_forwardHoldTimer->isActive()) {
            m_forwardHoldTimer->stop();
            seekBySeconds(10);
        } else if (!m_forwardPressConsumed) {
            // 定时器已触发但当时不是播放态，没进入 2×；松手补一次单步快进。
            seekBySeconds(10);
        }
    });

    connect(m_ui.mode, &QPushButton::clicked, this, &PlaybackUiController::modePressed);

    // 音量：滑块是唯一入口；播放器音量变化时反向同步滑块，保证快捷键、
    // 启动恢复和设置写回都不会让滑块停在旧数值。
    connect(m_ui.volume, &QSlider::valueChanged,
            m_player, &PlaybackController::setVolumePercent);
    connect(m_player, &PlaybackController::volumePercentChanged, this, [this](int percent) {
        QSignalBlocker blocker(m_ui.volume);
        m_ui.volume->setValue(percent);
    });

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
        refreshSeekButtons();
    });
    connect(m_player, &PlaybackController::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                // 暂停/停止/切歌时先结束长按，避免 2× 速率在状态切换后残留。
                if (state != QMediaPlayer::PlayingState)
                    cancelHolds(false);
                refreshPlayButton(state);
                refreshSeekButtons();
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
                // 长按快进播到结尾自动切歌时必须先恢复 1×，否则下一首会继续 2×。
                cancelHolds(false);
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
                refreshSeekButtons();
                emit currentItemChanged(item, row);
            });
    connect(m_player, &PlaybackController::modeChanged, this,
            &PlaybackUiController::refreshModeButton);
    connect(m_player, &PlaybackController::errorOccurred, this,
            [this](const QString &message) {
                cancelHolds(false);
                m_seekTarget = -1;
                emit statusMessage(tr("播放失败：%1").arg(message));
                refreshSeekButtons();
            });

    // 主题切换时内置图标需要按新强调色重画；外部自定义图标也重新解析。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](const ThemeInfo &) {
                refreshIcons();
                refreshPlayButton(m_player->mediaPlayer()->playbackState());
            });

    refreshIcons();
    refreshPlayButton(m_player->mediaPlayer()->playbackState());
    refreshSeekButtons();
}

PlaybackUiController::~PlaybackUiController()
{
    // 控制器销毁时不能把 2× 留在播放器里；窗口关闭路径也必须恢复。
    cancelHolds(false);
    if (qApp != nullptr)
        qApp->removeEventFilter(this);
}

/// @brief 播放项被移除后的界面归零：停 seek、进度归零、歌词清空。
void PlaybackUiController::clear()
{
    cancelHolds(false);
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
    refreshSeekButtons();
}

/// @brief 相对当前位置快进/快退指定秒数。
/// @param seconds 正数为快进、负数为快退；单位为秒。
/// @param announce 是否向状态栏播报本次跳转；长按连续快退只在第一步播报。
/// @return 目标确实发生变化时返回 true；无条目、时长未知或已到边界返回 false。
bool PlaybackUiController::seekBySeconds(int seconds, bool announce)
{
    if (seconds == 0 || !m_player->hasCurrentItem())
        return false;

    const qint64 duration = m_player->duration();
    if (duration <= 0)
        return false;

    // 连续快速点击时位置信号还没追上，必须从待执行/最近一次目标继续累加，
    // 否则三次 +10 秒会都从同一个旧位置算起，实际只前进 10 秒。
    const qint64 base = seekBasePosition();
    const qint64 target = PlaybackController::steppedSeekTarget(
        base, duration, static_cast<qint64>(seconds) * 1000);
    if (target == base)
        return false;

    // HLS 由 beginSeek() 统一提示“定位中”；本地文件没有那段等待，这里补一句。
    if (announce && !isHlsItem()) {
        emit statusMessage(seconds > 0
                               ? tr("快进 %1 秒：%2")
                                     .arg(seconds)
                                     .arg(TimeFormat::formatMs(target))
                               : tr("快退 %1 秒：%2")
                                     .arg(-seconds)
                                     .arg(TimeFormat::formatMs(target)));
    }
    beginSeek(target);
    return true;
}

/// @brief 长按快进开始：仅播放态切到 2×，暂停/停止保持原速率。
void PlaybackUiController::beginForwardHold()
{
    if (m_forwardHoldActive || !m_player->hasCurrentItem())
        return;
    if (m_player->mediaPlayer()->playbackState() != QMediaPlayer::PlayingState)
        return;

    m_rateBeforeHold = m_player->playbackRate();
    m_player->setPlaybackRate(2.0);
    m_forwardHoldActive = true;
    // 长按已经接管本次按下，松手时不能再补一次 +10 秒。
    m_forwardPressConsumed = true;
    emit statusMessage(tr("快进 2×（松开恢复）"));
}

/// @brief 结束快进长按并恢复进入前的速率。
/// @param announce 是否提示“已恢复”；切歌/失焦等被动收尾可静默。
void PlaybackUiController::endForwardHold(bool announce)
{
    if (!m_forwardHoldActive)
        return;

    m_forwardHoldActive = false;
    m_player->setPlaybackRate(m_rateBeforeHold);
    if (announce) {
        emit statusMessage(tr("已恢复 %1×")
                               .arg(m_rateBeforeHold, 0, 'f', 1));
    }
}

/// @brief 长按快退开始：先退 10 秒，然后按 500ms 间隔连续后退。
void PlaybackUiController::beginRewindHold()
{
    if (m_rewindHoldActive)
        return;

    m_rewindHoldActive = true;
    m_rewindPressConsumed = true;
    if (!seekBySeconds(-10)) {
        m_rewindHoldActive = false;
        m_rewindRepeatTimer->stop();
        return;
    }
    m_rewindRepeatTimer->start();
}

/// @brief 长按快退的后续步进；到达开头后自动停止重复。
void PlaybackUiController::advanceRewindHold()
{
    if (!m_rewindHoldActive) {
        m_rewindRepeatTimer->stop();
        return;
    }
    if (!seekBySeconds(-10, false)) {
        m_rewindHoldActive = false;
        m_rewindRepeatTimer->stop();
    }
}

/// @brief 松开/取消长按快退；不改变播放速率，只停止后续步进。
void PlaybackUiController::endRewindHold()
{
    m_rewindHoldActive = false;
    m_rewindPressConsumed = true;
    if (m_rewindRepeatTimer != nullptr)
        m_rewindRepeatTimer->stop();
    if (m_rewindHoldTimer != nullptr)
        m_rewindHoldTimer->stop();
}

/// @brief 统一取消所有长按状态并恢复原播放速率。
void PlaybackUiController::cancelHolds(bool announce)
{
    if (m_forwardHoldTimer != nullptr)
        m_forwardHoldTimer->stop();
    if (m_rewindHoldTimer != nullptr)
        m_rewindHoldTimer->stop();
    if (m_rewindRepeatTimer != nullptr)
        m_rewindRepeatTimer->stop();

    // 两个按钮的本次按下都视为已被长按消费，避免取消后松手又补一次跳转。
    m_forwardPressConsumed = true;
    m_rewindPressConsumed = true;
    m_rewindHoldActive = false;
    if (m_forwardHoldActive)
        endForwardHold(announce);
}

/// @brief 无当前条目、时长未知或已停止时禁用快进/快退。
void PlaybackUiController::refreshSeekButtons()
{
    const bool mediaLoaded = m_player->hasCurrentItem() && m_player->duration() > 0;
    const bool playingOrPaused =
        m_player->mediaPlayer()->playbackState() != QMediaPlayer::StoppedState;
    const bool enabled = mediaLoaded && playingOrPaused;
    m_ui.seekBack->setEnabled(enabled);
    m_ui.seekForward->setEnabled(enabled);
}

/// @brief 连点累加的基准位置：优先待执行目标，其次已确认目标，最后播放器位置。
qint64 PlaybackUiController::seekBasePosition() const
{
    if (m_seekPendingTarget >= 0)
        return m_seekPendingTarget;
    if (m_seekTarget >= 0)
        return m_seekTarget;
    return m_player->position();
}

/// @brief 应用/窗口失焦时鼠标释放事件可能丢失，必须在这里恢复 1×。
bool PlaybackUiController::eventFilter(QObject *watched, QEvent *event)
{
    if (event != nullptr
        && (event->type() == QEvent::ApplicationDeactivate
            || event->type() == QEvent::WindowDeactivate)) {
        cancelHolds(false);
    }
    return QObject::eventFilter(watched, event);
}


/// @brief 记录 seek 目标并启动 180ms 防抖，期间重复点击只覆盖目标。
/// @param target 目标播放位置（毫秒）。
/// @note HLS 条目立即显示“定位中”提示；真正 seek 在防抖到期后由 flushSeek() 下发。
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

/// @brief 防抖到期后真正下发 seek。
/// @note 本地文件直接 seek；HLS 记录目标、序号并起 45 秒兜底计时器，
///       防止播放器卡住不再发 positionChanged 时“定位中”永久停留。
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

/// @brief 判断实际播放地址是否为 HLS；有 MP4 备选时不算 HLS。
bool PlaybackUiController::isHlsItem() const
{
    if (!m_player->hasCurrentItem())
        return false;
    // 判断实际播放地址：条目原始 URL 可能是 HLS，但备选有 MP4 时会优先播 MP4，
    // 这时不能进入 HLS 的“定位中”等待。
    return playbackIsHls(m_player->currentItem());
}

/// @brief 播放按钮：无当前项时从活动列表的选中行起播，否则切换播放/暂停。
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

/// @brief 停止播放并清空 seek 状态、进度条和时间显示。
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

/// @brief 在“列表循环 → 单曲循环 → 随机”之间轮换播放模式。
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

/// @brief 按当前播放模式刷新模式按钮图标与悬停文案。
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

/// @brief 播放状态变化时刷新主播放按钮的图标与提示。
/// @param state 当前 QMediaPlayer 播放状态。
void PlaybackUiController::refreshPlayButton(QMediaPlayer::PlaybackState state)
{
    const bool playing = state == QMediaPlayer::PlayingState;
    UiIcons::apply(m_ui.play, playing ? QStringLiteral("pause") : QStringLiteral("play"),
                   playing ? tr("暂停") : tr("播放"), 26, QColor(255, 255, 255));
}

/// @brief 重新解析上一首/停止/下一首/模式按钮图标。
/// @note 主题切换时调用，内置图标需按新强调色重绘。
void PlaybackUiController::refreshIcons()
{
    UiIcons::apply(m_ui.seekBack, QStringLiteral("seek-back"), tr("快退 10 秒"), 20);
    UiIcons::apply(m_ui.previous, QStringLiteral("prev"), tr("上一首 / 上一个视频"), 20);
    UiIcons::apply(m_ui.stop, QStringLiteral("stop"), tr("停止播放"), 20);
    UiIcons::apply(m_ui.next, QStringLiteral("next"), tr("下一首 / 下一个视频"), 20);
    UiIcons::apply(m_ui.seekForward, QStringLiteral("seek-forward"), tr("快进 10 秒"), 20);
    refreshModeButton();
}
