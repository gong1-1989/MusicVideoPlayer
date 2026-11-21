#include "core/PlaybackController.h"

#include "net/HttpClient.h"
#include "stream/StreamProxy.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

Q_LOGGING_CATEGORY(lcPlayer, "mvp.player")

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
    , m_audio(new QAudioOutput(this))
    , m_proxy(new StreamProxy())
    , m_proxyThread(new QThread(this))
{
    // 代理必须独立于 GUI 线程：Qt FFmpeg 后端在停止/切源时会同步 wait
    // Demuxer 线程，而 Demuxer 此时正阻塞在本地代理 socket 上。两者同线程
    // 会互相等待；把代理移到工作线程后，GUI 等 Demuxer 时代理仍能继续供流。
    m_proxyThread->setObjectName(QStringLiteral("StreamProxyThread"));
    m_proxy->moveToThread(m_proxyThread);
    connect(m_proxyThread, &QThread::finished, m_proxy, &QObject::deleteLater);
    m_proxyThread->start();

    bool startResult = false;
    const bool invoked = m_proxyThread->isRunning()
        && QMetaObject::invokeMethod(
               m_proxy, [this, &startResult] { startResult = m_proxy->start(); },
               Qt::BlockingQueuedConnection);
    m_proxyReady = invoked && startResult;
    if (!m_proxyReady)
        qCWarning(lcPlayer) << "流转服务不可用，在线播放可能因 403 失败";
    else
        qCInfo(lcPlayer) << "流转代理已运行在独立线程";

    m_player->setAudioOutput(m_audio);
    setVolumePercent(60);
    updateLoops();

    connect(m_player, &QMediaPlayer::positionChanged,
            this, &PlaybackController::positionChanged);
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!m_proxyReady || m_proxy == nullptr || !hasCurrentItem())
            return;
        const MediaItem item = currentItem();
        const QUrl playbackUrl = preferredPlaybackUrl(item);
        if (!item.online || !isHlsUrl(playbackUrl))
            return;
        // 每 3 秒提交一次就够了；分片时长通常 3-7 秒，太频繁只会反复覆盖队列。
        if (m_lastPrefetchPosition >= 0 && qAbs(position - m_lastPrefetchPosition) < 3000)
            return;
        m_lastPrefetchPosition = position;
        QMetaObject::invokeMethod(
            m_proxy,
            [proxy = m_proxy, url = playbackUrl, headers = item.headers, position] {
                proxy->prefetchForPosition(url, headers, position);
            },
            Qt::QueuedConnection);
    });
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
                qCWarning(lcPlayer) << "音源不可用："
                                   << HttpClient::redactUrl(upstream) << contentType;
                // 只记录原因，不在这里 emit：QMediaPlayer 随后一定会发 errorOccurred，
                // 否则同一次失败会在界面弹两次。
                m_unavailableReason =
                    tr("这个源没有返回可播放的音频/视频（版权、付费或需要登录），换一首试试");
            });
}

PlaybackController::~PlaybackController()
{
    m_proxyReady = false;
    if (m_proxyThread->isRunning()) {
        // shutdown 必须回到代理线程执行，确保 QTcpServer 和所有 reply
        // 在各自线程里关闭；这里不会与播放器的 Demuxer 等待互相阻塞。
        QMetaObject::invokeMethod(m_proxy, [this] { m_proxy->shutdown(); },
                                  Qt::BlockingQueuedConnection);
        m_proxyThread->quit();
        if (!m_proxyThread->wait(3000)) {
            qCWarning(lcPlayer) << "流转代理线程未在 3 秒内退出，强制终止";
            m_proxyThread->terminate();
            m_proxyThread->wait(500);
        }
    }
    m_proxy = nullptr;
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
    m_lastPrefetchPosition = -1;

    // 在线流统一走本地回环代理，由代理补 Referer/Cookie 并透传 Range；
    // 本地文件直接用 file:// 播放。视频优先 mp4/flv，HLS 只做回退。
    const QUrl playbackUrl = preferredPlaybackUrl(item);
    QUrl source = playbackUrl;
    if (item.online && m_proxyReady && m_proxyThread->isRunning()) {
        QUrl proxied;
        const bool invoked = QMetaObject::invokeMethod(
            m_proxy,
            [proxy = m_proxy, url = playbackUrl, headers = item.headers, &proxied] {
                proxied = proxy->proxyUrlFor(url, headers);
            },
            Qt::BlockingQueuedConnection);
        if (invoked && proxied.isValid())
            source = proxied;
    }

    m_player->setSource(source);
    m_player->play();
    qCInfo(lcPlayer) << "开始播放" << row << item.displayName()
                     << (item.online ? "在线代理" : "本地文件");
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
    if (!hasCurrentItem())
        return;

    // 拖动时不再预取：实测 FFmpeg 的 HLS seek 会先回退约 9 片，
    // 预取这些分片会和播放器自己的请求抢同一条慢源连接，单路都可能超时；
    // 深拖动只保留 UI 的“定位中”等待提示，正常播放仍由 positionChanged 预取后续分片。
    // 这里把预取基准同步到目标位置，避免位置跳到目标后立刻又预取目标片，
    // 和 FFmpeg 正在回退读取的分片抢带宽。
    m_lastPrefetchPosition = ms;
    m_player->setPosition(ms);

    // 只预取目标片之后 1-2 片，且由代理单连接、延迟起步；
    // 目标片本身仍交给播放器的多连接 Range 请求，避免重复占带宽。
    const MediaItem item = currentItem();
    const QUrl playbackUrl = preferredPlaybackUrl(item);
    if (m_proxyReady && m_proxy != nullptr && item.online && isHlsUrl(playbackUrl)) {
        QMetaObject::invokeMethod(
            m_proxy,
            [proxy = m_proxy, playbackUrl, headers = item.headers, ms] {
                proxy->prefetchForSeek(playbackUrl, headers, ms);
            },
            Qt::QueuedConnection);
    }
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
