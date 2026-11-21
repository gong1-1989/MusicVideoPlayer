#include "core/PlaybackController.h"

#include "core/VideoPositionStore.h"
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
        // 加载完成时立刻 setPosition 可能被随后的 play() 重置回 0；
        // 这里以播放器真实上报的位置为准，确认到达目标前保留待恢复任务并限次重试。
        if (m_pendingResumeMs > 0) {
            const qint64 target = m_pendingResumeMs;
            if (qAbs(position - target) <= 1500) {
                qCInfo(lcPlayer) << "视频进度已恢复" << currentItem().displayName()
                                 << position;
                m_pendingResumeMs = -1;
                m_pendingResumeIdentity.clear();
                m_resumeRetryCount = 0;
            } else if (position + 500 < target && m_resumeRetryCount < 4) {
                QTimer::singleShot(200, this, &PlaybackController::applyPendingResume);
            }
        }

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
    connect(m_player, &QMediaPlayer::seekableChanged, this, [this](bool seekable) {
        // HLS 的 seekable 往往晚于 LoadedMedia 才变成 true，等它真正可定位再恢复。
        if (seekable)
            applyPendingResume();
    });
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

    QThread *proxyThread = m_proxyThread;
    StreamProxy *proxy = m_proxy;
    if (proxyThread == nullptr || !proxyThread->isRunning()) {
        m_proxy = nullptr;
        return;
    }

    // 旧实现用 BlockingQueuedConnection 从 GUI 线程等代理线程；代理一旦卡在
    // 网络回调或析构里，退出流程会无限等待。这里改为把 shutdown 和 quit
    // 投递到代理线程自己的事件循环，最多等 3 秒。
    //
    // 超时后不再调用 QThread::terminate()：强制终止可能发生在互斥锁、网络
    // 回调或 deleteLater 中间，轻则泄漏，重则崩溃。线程还在跑时也不能让
    // PlaybackController 的析构连带 delete 运行中的 QThread，所以把线程对象
    // 从父子关系里摘掉，留给进程退出回收。
    const bool queued = proxy != nullptr
        && QMetaObject::invokeMethod(
               proxy,
               [proxy, proxyThread] {
                   proxy->shutdown();
                   proxyThread->quit();
               },
               Qt::QueuedConnection);
    if (!queued)
        qCWarning(lcPlayer) << "无法投递流转代理关闭任务，线程保持运行";

    if (!proxyThread->wait(3000)) {
        qCWarning(lcPlayer) << "流转代理线程未在 3 秒内退出，放弃强制终止";
        proxyThread->setParent(nullptr);
        m_proxyThread = nullptr;
        m_proxy = nullptr;
        return;
    }

    m_proxy = nullptr;
}

void PlaybackController::setPlaylist(PlaylistModel *playlist)
{
    if (m_playlist.data() == playlist)
        return;

    // 从视频列表切到音乐/搜索列表时，先把当前视频位置落盘再换模型。
    saveCurrentVideoPosition();
    m_pendingResumeMs = -1;
    m_pendingResumeIdentity.clear();

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

    // 切换视频（含自动下一集）前保存正在播放的那一条；音乐会被存储层忽略。
    saveCurrentVideoPosition();
    m_pendingResumeMs = -1;
    m_pendingResumeIdentity.clear();

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

    // 仅视频恢复进度；音乐按要求每次都从头播放。
    if (item.type == MediaType::Video) {
        const qint64 resume = VideoPositionStore::positionFor(item);
        if (resume > 0) {
            m_pendingResumeMs = resume;
            m_pendingResumeIdentity = mediaIdentity(item);
            m_resumeRetryCount = 0;
            qCInfo(lcPlayer) << "准备恢复视频进度" << item.displayName() << resume;
        }
    }

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
    // 关闭窗口、按停止都会走这里；必须在 QMediaPlayer 清零前读出最后位置。
    saveCurrentVideoPosition();
    m_pendingResumeMs = -1;
    m_pendingResumeIdentity.clear();
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

    // 用户手动拖到别处时，放弃尚未确认的续播任务，避免 seekableChanged 等信号再把位置拉回去。
    if (m_pendingResumeMs > 0 && qAbs(ms - m_pendingResumeMs) > 1500) {
        m_pendingResumeMs = -1;
        m_pendingResumeIdentity.clear();
        m_resumeRetryCount = 0;
    }

    // 预取窗口由 StreamProxy 统一管理（目标片起 target..target+9，4 片并发、
    // 每片 4 路 Range）；UI 只负责把“定位中”讲清楚，不在这里重复调度。
    // 这里只同步预取基准位置，避免位置跳到目标后立刻又预取目标片。
    m_lastPrefetchPosition = ms;
    m_player->setPosition(ms);

    // 目标片本身仍交给播放器/代理的多连接 Range 请求，避免重复占带宽。
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

QString PlaybackController::modeText(bool compact) const
{
    return modeDisplayText(m_mode, compact);
}

QString PlaybackController::modeDisplayText(Mode mode, bool compact)
{
    // 紧凑布局只缩短文案，模式含义必须由同一个 switch 决定，
    // 避免 MainWindow 在窄屏时把单曲/随机写回“列表循环”。
    switch (mode) {
    case Mode::ListLoop:
        return compact ? tr("循环") : tr("列表循环");
    case Mode::SingleLoop:
        return compact ? tr("单曲") : tr("单曲循环");
    case Mode::Random:
        return compact ? tr("随机") : tr("随机播放");
    }
    return compact ? tr("循环") : tr("列表循环");
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
        // 正在播的就是被删掉的那首，先保存位置，再停，避免继续指向已失效的下标
        saveCurrentVideoPosition();
        m_pendingResumeMs = -1;
        m_pendingResumeIdentity.clear();
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
    if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)
        applyPendingResume();

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

void PlaybackController::saveCurrentVideoPosition()
{
    if (!hasCurrentItem())
        return;

    const MediaItem item = currentItem();
    if (item.type != MediaType::Video)
        return;

    const qint64 position = m_player->position();
    if (position <= 0)
        return;

    // duration 为 0 表示时长尚未解析出来；位置仍然有效，允许先存。
    VideoPositionStore::save(item, position, m_player->duration());
}

void PlaybackController::applyPendingResume()
{
    if (m_pendingResumeMs <= 0 || !hasCurrentItem())
        return;

    const MediaItem item = currentItem();
    if (item.type != MediaType::Video
        || mediaIdentity(item) != m_pendingResumeIdentity) {
        m_pendingResumeMs = -1;
        m_pendingResumeIdentity.clear();
        m_resumeRetryCount = 0;
        return;
    }

    const qint64 target = m_pendingResumeMs;
    if (m_resumeRetryCount >= 4)
        return;
    ++m_resumeRetryCount;

    // 这里只发起定位，不清除待恢复任务；由 positionChanged 确认真正到达目标。
    // 播放器在 LoadedMedia 阶段可能接受 setPosition 但在 play() 启动时又重置为 0。
    qCInfo(lcPlayer) << "请求恢复视频进度" << item.displayName() << target
                     << "第" << m_resumeRetryCount << "次";
    seek(target);
}

qint64 PlaybackController::position() const
{
    return m_player->position();
}

qint64 PlaybackController::duration() const
{
    return m_player->duration();
}
