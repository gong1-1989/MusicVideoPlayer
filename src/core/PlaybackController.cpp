#include "core/PlaybackController.h"

#include "stream/StreamProxy.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QTimer>

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
        qCWarning(lcPlayer) << "流转服务不可用，在线播放可能因 403 失败";

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
                qCWarning(lcPlayer) << "播放出错：" << text;
                emit errorOccurred(text);

                // 单曲失败不应把整个列表卡死：跳过这一首继续下一首；
                // 连续失败达到列表长度就停下，避免所有源都坏时无限循环。
                if (++m_consecutiveErrors < rowCount()) {
                    QTimer::singleShot(0, this, &PlaybackController::next);
                } else {
                    qCWarning(lcPlayer) << "连续播放失败" << m_consecutiveErrors << "次，停止自动跳过";
                    m_player->stop();
                }
            });

    connect(m_proxy, &StreamProxy::sourceUnavailable, this,
            [this](const QUrl &upstream, const QString &contentType) {
                qCWarning(lcPlayer) << "音源不可用：" << upstream.toString() << contentType;
                // 只记录原因，不在这里 emit：QMediaPlayer 随后一定会发 errorOccurred，
                // 否则同一次失败会在界面弹两次。
                m_unavailableReason =
                    tr("这个源没有返回可播放的音频/视频（版权、付费或需要登录），换一首试试");
            });
}

void PlaybackController::setPlaylist(PlaylistModel *playlist)
{
    if (m_playlist.data() == playlist)
        return;
    m_playlist = playlist;
    m_row = -1;
    m_consecutiveErrors = 0;
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
    playRowInternal(row, false);
}

void PlaybackController::playRowInternal(int row, bool autoAdvance)
{
    if (!m_playlist || row < 0 || row >= m_playlist->size())
        return;

    // 用户主动点歌视为重新开始一轮；自动跳过则保留连续失败计数，
    // 否则每个坏源都“加载一下再报错”会无限循环。
    if (!autoAdvance)
        m_consecutiveErrors = 0;

    const MediaItem item = m_playlist->itemAt(row);
    if (!item.isValid()) {
        emit errorOccurred(tr("媒体地址无效"));
        m_row = row;
        if (++m_consecutiveErrors < rowCount())
            QTimer::singleShot(0, this, &PlaybackController::next);
        else
            m_player->stop();
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
    qCInfo(lcPlayer) << "开始播放" << row << item.displayName() << source.toString();
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
    m_consecutiveErrors = 0;
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
    playRowInternal(row, true);
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
    // 随机模式下“上一首”也随机，和 next() 保持一致，不再退化成顺序上一首
    if (m_mode == Mode::Random && count > 1) {
        int row = m_row;
        while (row == m_row)
            row = int(QRandomGenerator::global()->bounded(count));
        playRow(row);
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

    // 完整播完一首才算“这一轮源是好的”，清空连续失败计数。
    m_consecutiveErrors = 0;

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
