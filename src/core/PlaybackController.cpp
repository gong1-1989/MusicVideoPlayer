#include "core/PlaybackController.h"

#include "stream/StreamProxy.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QRandomGenerator>

Q_LOGGING_CATEGORY(lcPlayer, "mvp.player")

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_audio(new QAudioOutput(this))
    , m_proxy(new StreamProxy(this))
{
    m_player->setAudioOutput(m_audio);
    setVolumePercent(60);
    updateLoops();

    if (!m_proxy->start())
        qCWarning(lcPlayer) << "stream proxy unavailable, online playback may fail on 403";

    connect(m_player, &QMediaPlayer::positionChanged,
            this, &PlaybackController::positionChanged);
    connect(m_player, &QMediaPlayer::durationChanged,
            this, &PlaybackController::durationChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &PlaybackController::playbackStateChanged);
    connect(m_player, &QMediaPlayer::mediaStatusChanged,
            this, &PlaybackController::onMediaStatusChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                // 上游没有音源时，播放器只会报 “Could not open file”，
                // 用代理给出的真实原因替换掉，别让用户以为是自己操作错了
                const QString text = m_unavailableReason.isEmpty() ? message : m_unavailableReason;
                m_unavailableReason.clear();
                qCWarning(lcPlayer) << "playback error:" << text;
                emit errorOccurred(text);
            });

    connect(m_proxy, &StreamProxy::sourceUnavailable, this,
            [this](const QUrl &upstream, const QString &contentType) {
                qCWarning(lcPlayer) << "source unavailable:" << upstream.toString() << contentType;
                m_unavailableReason =
                    tr("这个源没有返回可播放的音频/视频（版权、付费或需要登录），换一首试试");
                emit errorOccurred(m_unavailableReason);
            });
}

void PlaybackController::setPlaylist(PlaylistModel *playlist)
{
    if (m_playlist.data() == playlist)
        return;
    m_playlist = playlist;
    m_row = -1;
}

bool PlaybackController::hasCurrentItem() const
{
    return m_playlist && m_row >= 0 && m_row < m_playlist->size();
}

MediaItem PlaybackController::currentItem() const
{
    if (!hasCurrentItem())
        return MediaItem();
    return m_playlist->itemAt(m_row);
}

int PlaybackController::rowCount() const
{
    return m_playlist ? m_playlist->size() : 0;
}

void PlaybackController::playRow(int row)
{
    if (!m_playlist || row < 0 || row >= m_playlist->size())
        return;

    const MediaItem item = m_playlist->itemAt(row);
    if (!item.isValid()) {
        emit errorOccurred(tr("媒体地址无效"));
        return;
    }

    m_row = row;
    m_unavailableReason.clear();

    // 在线流统一走本地回环代理，由代理补 Referer/Cookie 并透传 Range；
    // 本地文件直接用 file:// 播放。
    QUrl source = item.url;
    if (item.online && m_proxy->isListening()) {
        const QUrl proxied = m_proxy->proxyUrlFor(item.url, item.headers);
        if (proxied.isValid())
            source = proxied;
    }

    m_player->setSource(source);
    m_player->play();
    qCInfo(lcPlayer) << "play" << row << item.displayName() << source.toString();
    emit currentItemChanged(item, row);
}

void PlaybackController::togglePlayPause()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
        return;
    }
    if (hasCurrentItem()) {
        m_player->play();
        return;
    }
    if (rowCount() > 0)
        playRow(0);
}

void PlaybackController::stop()
{
    m_player->stop();
}

void PlaybackController::next()
{
    const int count = rowCount();
    if (count <= 0)
        return;

    int row = 0;
    if (m_row < 0) {
        row = 0;
    } else if (m_mode == Mode::Random && count > 1) {
        do {
            row = int(QRandomGenerator::global()->bounded(count));
        } while (row == m_row);
    } else {
        row = (m_row + 1) % count;
    }
    playRow(row);
}

void PlaybackController::previous()
{
    const int count = rowCount();
    if (count <= 0)
        return;
    if (m_row < 0) {
        playRow(0);
        return;
    }
    playRow(m_row == 0 ? count - 1 : m_row - 1);
}

void PlaybackController::seek(qint64 ms)
{
    if (hasCurrentItem())
        m_player->setPosition(ms);
}

void PlaybackController::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    updateLoops();
    emit modeChanged(m_mode);
}

void PlaybackController::updateLoops()
{
    // 单曲循环交给播放器自己处理，避免在 mediaStatusChanged 里手工重播
    m_player->setLoops(m_mode == Mode::SingleLoop ? QMediaPlayer::Infinite : 1);
}

QString PlaybackController::modeText() const
{
    switch (m_mode) {
    case Mode::ListLoop:
        return tr("列表循环");
    case Mode::SingleLoop:
        return tr("单曲循环");
    case Mode::Random:
        return tr("随机播放");
    }
    return tr("列表循环");
}

void PlaybackController::setVolumePercent(int percent)
{
    m_volume = qBound(0, percent, 100);
    // 听觉曲线：平方映射，避免滑块中段过响
    const float linear = static_cast<float>(m_volume) / 100.0f;
    m_audio->setVolume(linear * linear);
    emit volumePercentChanged(m_volume);
}

void PlaybackController::onPlaylistRowRemoved(int row)
{
    if (row < 0)
        return;

    if (m_row == row) {
        // 正在播的就是被删掉的那首，直接停，避免继续指向已失效的下标
        m_player->stop();
        m_player->setSource(QUrl());
        m_row = -1;
        return;
    }
    if (m_row > row)
        --m_row;
}

void PlaybackController::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status != QMediaPlayer::EndOfMedia)
        return;

    if (m_mode == Mode::SingleLoop) {
        // 单曲循环由 QMediaPlayer::setLoops(Infinite) 负责续播，这里不再手工重播，
        // 否则会和播放器自身的循环逻辑打架、产生重复 seek。
        return;
    }
    next();
}

qint64 PlaybackController::position() const
{
    return m_player->position();
}

qint64 PlaybackController::duration() const
{
    return m_player->duration();
}
