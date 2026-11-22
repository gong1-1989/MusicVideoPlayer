#include "core/PlaybackController.h"

#include "cache/MediaCache.h"
#include "net/HttpClient.h"
#include "stream/StreamProxy.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <utility>

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
        if (position > 0)
            m_lastKnownPosition = position;
        // 媒体已加载且位置在推进，说明这一轮确实播起来了：后续错误不能再
        // 被当成“加载失败”而自动切歌，同时清掉启动阶段的连续失败计数。
        if (!m_playAttemptStarted && position >= 1000
            && m_player->playbackState() != QMediaPlayer::StoppedState) {
            m_playAttemptStarted = true;
            m_consecutiveErrors = 0;
        }
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
        const MediaItem modelItem = currentItem();
        const MediaItem item = m_itemResolver ? m_itemResolver(modelItem) : modelItem;
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

                // 已经播到中途才出错（网络抖动、分片过期、解码失败），说明
                // 不是“这一条根本打不开”。此时自动 next 会表现为“没播完就跳歌”，
                // 因此停在当前曲目并保留错误提示，让用户自己决定重试或换歌。
                if (m_playAttemptStarted) {
                    qCWarning(lcPlayer) << "播放中途失败，保留当前曲目，不自动切歌";
                    m_playAttemptStarted = false;
                    m_player->stop();
                    return;
                }

                // 启动阶段失败才允许跳过；连续失败达到列表长度就停下，
                // 避免所有源都坏时无限循环。
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
        // 线程析构会 delete 仍属于该线程的 StreamProxy；这里让线程在真正
        // 退出后再自删，避免 PlaybackController 重建后遗留一个无人回收的
        // QThread 和代理对象。finished 信号按队列回到 GUI 线程处理。
        QObject::connect(proxyThread, &QThread::finished,
                         proxyThread, &QObject::deleteLater);
        m_proxyThread = nullptr;
        m_proxy = nullptr;
        return;
    }

    m_proxy = nullptr;
}

void PlaybackController::setMediaItemResolver(
    std::function<MediaItem(const MediaItem &)> resolver)
{
    m_itemResolver = std::move(resolver);
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
    // 新一轮播放从“尚未出声”开始；只有真正走到可播位置才会翻转，
    // 这样启动失败可以跳过，中途网络/解码错误不会冒充“播完了”。
    m_playAttemptStarted = false;
    m_lastKnownPosition = 0;
    m_earlyEndRecoveries = 0;

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

    // 播放列表落盘时不保存 Cookie；播放这一份副本按 sourceId 从源配置
    // 恢复内存态凭据，模型本身仍保持脱敏内容。
    const MediaItem playbackItem = m_itemResolver ? m_itemResolver(item) : item;

    m_row = row;
    m_unavailableReason.clear();
    m_lastPrefetchPosition = -1;

    // 仅视频恢复进度；进度就存在列表条目里，条目删除时进度自然消失。
    if (item.type == MediaType::Video && item.positionMs > 0) {
        m_pendingResumeMs = item.positionMs;
        m_pendingResumeIdentity = mediaIdentity(item);
        m_resumeRetryCount = 0;
        qCInfo(lcPlayer) << "准备恢复视频进度" << item.displayName() << item.positionMs;
    }

    // 加载优先级：本地文件 > 完整本地缓存 > 断点续传缓存 > 网络。
    // 本地条目本来就在磁盘上，再缓存一份纯属浪费；只有在线条目（音乐/影视）
    // 才建立与播放列表条目一一对应的整段缓存。HLS 完整分片由 HlsCacheBuilder
    // 合并转成无后缀 MP4 后，也在这个分支优先按本地文件播放。
    const QUrl playbackUrl = preferredPlaybackUrl(playbackItem);
    QUrl source = playbackUrl;
    QString sourceKind = playbackItem.online ? QStringLiteral("在线代理")
                                             : QStringLiteral("本地文件");
    const bool cacheable = MediaCache::isCacheable(playbackItem);
    const QString completeCache =
        playbackItem.online ? MediaCache::completeFileForItem(playbackItem) : QString();
    if (!completeCache.isEmpty()) {
        source = QUrl::fromLocalFile(completeCache);
        sourceKind = QStringLiteral("本地缓存");
    } else if (playbackItem.online && m_proxyReady && m_proxyThread->isRunning()) {
        const QString cachePath =
            cacheable ? MediaCache::filePathForItem(playbackItem) : QString();
        if (cacheable && !MediaCache::partialFileForItem(playbackItem).isEmpty())
            sourceKind = QStringLiteral("本地缓存续传");
        QUrl proxied;
        const bool invoked = QMetaObject::invokeMethod(
            m_proxy,
            [proxy = m_proxy, url = playbackUrl, headers = playbackItem.headers,
             cachePath, identity = MediaCache::cacheIdentity(playbackItem), &proxied] {
                proxied = proxy->proxyUrlFor(url, headers, cachePath, identity);
            },
            Qt::BlockingQueuedConnection);
        if (invoked && proxied.isValid())
            source = proxied;
    }

    m_player->setSource(source);
    // 新 source 的首片还没就绪，此时任何 setPosition 都会被播放器丢弃，
    // 也会让代理提前取消正在传输的分片；等 Loaded/Buffered 再统一执行。
    m_mediaReady = false;
    m_deferredSeekMs = -1;
    m_player->play();
    qCInfo(lcPlayer) << "开始播放" << row << item.displayName() << sourceKind
                     << HttpClient::redactUrl(source);
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
    if (!hasCurrentItem() || ms < 0)
        return;

    // 关键保护：播放器还没到 Loaded/Buffered 时不要下发 setPosition。
    // 流媒体首片仍在加载时连续点击进度条，会让 StreamProxy 反复取消预取，
    // FFmpeg 正在读的 Range 连接被 abort 后客户端会拿到 502 并崩溃。
    // 这里只记录用户最后一次目标，等媒体真正可定位时再执行一次。
    if (!m_mediaReady) {
        m_deferredSeekMs = ms;
        qCInfo(lcPlayer) << "媒体未就绪，暂存定位目标" << ms;
        return;
    }

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
        // 条目已经从模型里移除，进度属于被删条目，随条目一起丢弃；
        // 不能再读当前播放器位置写进后面那条，否则会把进度串到下一集。
        m_pendingResumeMs = -1;
        m_pendingResumeIdentity.clear();
        m_resumeRetryCount = 0;
        m_player->stop();
        m_player->setSource(QUrl());
        m_row = -1;
        return;
    }
    if (m_row > row)
        --m_row;
}

void PlaybackController::releaseStreamCacheForItem(const MediaItem &item)
{
    if (!m_proxyReady || m_proxy == nullptr || m_proxyThread == nullptr
        || !m_proxyThread->isRunning()) {
        return;
    }
    const QString path = MediaCache::filePathForItem(item);
    if (path.isEmpty())
        return;
    // 代理在独立线程；队列投递后由删除逻辑的 500ms 重试覆盖。
    // 不使用阻塞队列：代理可能正在写大分片，阻塞 GUI 没有收益。
    QMetaObject::invokeMethod(
        m_proxy,
        [proxy = m_proxy, path] {
            proxy->releaseCacheFile(path);
        },
        Qt::QueuedConnection);
}

void PlaybackController::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
        m_mediaReady = true;
        applyPendingResume();
        // 加载期间用户点过的最后一次定位只在这里执行一次，
        // 不再让每次点击都去打断代理正在传输的分片连接。
        if (m_deferredSeekMs >= 0) {
            const qint64 target = m_deferredSeekMs;
            m_deferredSeekMs = -1;
            seek(target);
        }
    } else if (status == QMediaPlayer::InvalidMedia) {
        m_mediaReady = false;
    }

    if (status != QMediaPlayer::EndOfMedia)
        return;

    // 源在中间断开/只返回了试听片段时，播放器也会报 EndOfMedia。
    // 先比对“最后位置”和已知总时长，明显没到结尾就原地续播，
    // 绝不能像正常播完一样切下一首（这正是用户看到的“2 分钟就切歌”）。
    const MediaItem finishedItem = currentItem();
    const qint64 playerDuration = m_player->duration();
    const qint64 expectedDuration = qMax(playerDuration, finishedItem.durationMs);
    const qint64 endPosition = qMax(m_player->position(), m_lastKnownPosition);
    if (expectedDuration > 0 && endPosition + 5000 < expectedDuration) {
        if (m_earlyEndRecoveries < 3) {
            ++m_earlyEndRecoveries;
            qCWarning(lcPlayer) << "提前结束，疑似源被截断" << finishedItem.displayName()
                                << "位置" << endPosition << "总时长" << expectedDuration
                                << "第" << m_earlyEndRecoveries << "次续播";
            m_player->setPosition(qMax<qint64>(0, endPosition - 2000));
            QTimer::singleShot(300, this, [this] {
                if (hasCurrentItem())
                    m_player->play();
            });
        } else {
            qCWarning(lcPlayer) << "多次原地续播仍提前结束，停止并保留当前曲目";
            m_player->stop();
        }
        return;
    }

    // 完整播完一首才算“这一轮源是好的”，清空连续失败计数。
    m_consecutiveErrors = 0;
    m_earlyEndRecoveries = 0;

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

    MediaItem item = currentItem();
    if (item.type != MediaType::Video)
        return;

    const qint64 position = m_player->position();
    if (position <= 0)
        return;

    const qint64 duration = m_player->duration();

    // 片头 3 秒内视为没看；片尾 3 秒内清掉旧进度，下次从头播放。
    const qint64 savedPosition =
        (position < 3000 || (duration > 0 && position >= duration - 3000)) ? 0 : position;

    // 用专用入口写进度：updateItem 是“搜索刷新”，按设计不碰 positionMs，
    // 之前把进度塞进 updateItem 会被静默丢弃，导致只有退出时才像保存了。
    if (m_playlist->updatePlaybackPosition(m_row, savedPosition, duration)) {
        qCInfo(lcPlayer) << "已写入列表视频进度" << item.displayName() << savedPosition;
        emit currentItemPositionUpdated(m_row);
    }
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
