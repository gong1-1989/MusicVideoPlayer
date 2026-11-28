#include "core/PlaybackController.h"

#include "cache/MediaCache.h"
#include "net/HttpClient.h"
#include "net/MediaPageResolver.h"
#include "stream/StreamProxy.h"
#include "util/NetworkAddressPolicy.h"

#include <QAudioOutput>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <utility>
#include <algorithm>

Q_LOGGING_CATEGORY(lcPlayer, "mvp.player")

namespace {
/// 恢复进度的成功容差：seek 落点与目标相差 1.5 秒内即视为到位。
constexpr qint64 kResumeToleranceMs = 1500;

/// 恢复是否已到位：允许 seek 落点略早于目标，但一旦越过目标就算完成。
/// positionChanged 与 applyPendingResume 必须共用同一判定，否则一处已宣告成功、
/// 另一处还在轮询，同一次恢复会被处理两遍。
bool resumeTargetReached(qint64 position, qint64 target)
{
    return position + kResumeToleranceMs >= target;
}
}

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

    // 恢复进度不能按“调用次数”限次：Load/Buffered、seekableChanged 和
    // positionChanged 都会触发，HLS/代理源真正可 seek 之前就可能把次数用完。
    // 改为单次定时器轮询，直到位置确认到达或 15 秒超时。
    m_resumeTimer = new QTimer(this);
    m_resumeTimer->setSingleShot(true);
    connect(m_resumeTimer, &QTimer::timeout, this, &PlaybackController::applyPendingResume);

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
        // 这里以播放器真实上报的位置为准，确认到达目标前保留待恢复任务，
        // 并在时间窗内轮询，不能因为几次过早的 setPosition 就放弃。
        // 必须先等新媒体 Loaded/Buffered，且确实下发过一次 seek：
        // setSource() 期间播放器可能仍上报上一条/旧缓存的位置，提前判定
        // “已恢复”会清掉任务，随后新 source 从 0 开始播放。
        if (m_pendingResumeMs > 0 && m_mediaReady && m_sourcePlayRequested
            && m_resumeAttemptCount > 0) {
            const qint64 target = m_pendingResumeMs;
            if (resumeTargetReached(position, target)) {
                qCInfo(lcPlayer) << "视频进度已恢复" << currentItem().displayName()
                                 << position;
                clearPendingResume();
            } else {
                schedulePendingResume(500);
            }
        }

        if (!m_proxyReady || m_proxy == nullptr || !hasCurrentItem())
            return;
        const MediaItem modelItem = currentItem();
        const MediaItem item = m_itemResolver ? m_itemResolver(modelItem) : modelItem;
        // 换过备用线路后，预取必须跟随正在播放的那条；继续拿主线路地址
        // 会让代理去重试已经 403/HTML 的旧线路，拖慢实际播放。
        const QUrl playbackUrl = activePlaybackUrl(item);
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
                const MediaItem failedItem = currentItem();
                // 网页播放页解析出的签名直链可能已经过期。播放失败时先让
                // 页面缓存失效；用户重试或自动换线后会重新解析播放页，
                // 而不是在本次运行内一直复用同一个失效地址。
                if (m_pageResolver != nullptr)
                    m_pageResolver->invalidate(failedItem.url);
                qCWarning(lcPlayer) << "播放出错：" << text << "条目" << failedItem.displayName()
                                    << "源" << failedItem.sourceId << "地址"
                                    << HttpClient::redactUrl(preferredPlaybackUrl(failedItem));

                // 启动阶段失败先试备用线路。很多影视结果同时带 m3u8/mp4 多条
                // 线路，旧逻辑只选一条，主线路 403/404 就直接跳下一首；这里
                // 在真正报错前换线，只有全部线路都失败才让用户看到错误。
                if (!m_playAttemptStarted && tryNextFallback())
                    return;

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
            [this](const QUrl &upstream, const QString &contentType, int statusCode) {
                qCWarning(lcPlayer) << "音源不可用：" << HttpClient::redactUrl(upstream)
                                   << "状态" << statusCode << "类型" << contentType;
                // 只记录原因，不在这里 emit：QMediaPlayer 随后一定会发 errorOccurred，
                // 否则同一次失败会在界面弹两次。状态码和 Content-Type 一起展示，
                // 用户能区分“403 被源站拒绝”和“返回了网页/版权页”两种不可播。
                QString detail = contentType.isEmpty() ? tr("未知类型") : contentType;
                if (statusCode > 0)
                    detail = tr("HTTP %1，%2").arg(statusCode).arg(detail);
                m_unavailableReason =
                    tr("这个源没有返回可播放的音频/视频（%1；版权、付费或需要登录）")
                        .arg(detail);
            });
}

/// @brief 析构时异步关闭流转代理线程。
/// @note 不使用 BlockingQueuedConnection 从 GUI 线程等待代理线程：代理卡在网络
///       回调或析构里时会无限阻塞退出流程。这里把 shutdown 和 quit 投递到代理
///       线程自己的事件循环，最多等 3 秒。
PlaybackController::~PlaybackController()
{
    m_proxyReady = false;

    QThread *proxyThread = m_proxyThread;
    StreamProxy *proxy = m_proxy;
    if (proxyThread == nullptr || !proxyThread->isRunning()) {
        m_proxy = nullptr;
        return;
    }

    // 不能从 GUI 线程同步等待代理线程：代理一旦卡在网络回调或析构里，
    // 退出流程会无限等待。因此把 shutdown 和 quit 投递到代理线程自己的
    // 事件循环，最多等 3 秒。
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

QUrl PlaybackController::lastHlsVideoPlaylistUrl() const
{
    if (m_proxy == nullptr || !m_proxyReady)
        return QUrl();
    return m_proxy->lastPlaybackVideoPlaylist();
}

QUrl PlaybackController::lastHlsAudioPlaylistUrl() const
{
    if (m_proxy == nullptr || !m_proxyReady)
        return QUrl();
    return m_proxy->lastPlaybackAudioPlaylist();
}

/// @brief 让后台 HLS 缓存按分片登记同一个流转代理，和播放共用上游缓存。
/// @note 只在代理线程已启动时阻塞取一次代理 URL；注册本身很轻，分片按需
///       登记，未实际请求的分片不会产生上游连接。代理未就绪时返回空，
///       DownloadTask 会自动回退为原始分片地址。
QUrl PlaybackController::proxyUrlForHlsCache(
    const QUrl &upstream, const QHash<QString, QString> &headers,
    const QString &cachePath, const QString &cacheIdentity,
    const QUrl &sourceOrigin) const
{
    if (m_proxy == nullptr || !m_proxyReady || m_proxyThread == nullptr
        || !m_proxyThread->isRunning()) {
        return QUrl();
    }
    QUrl proxied;
    const bool invoked = QMetaObject::invokeMethod(
        m_proxy,
        [proxy = m_proxy, upstream, headers, cachePath, cacheIdentity, sourceOrigin,
         &proxied] {
            proxied = proxy->proxyUrlFor(upstream, headers, cachePath, cacheIdentity,
                                         sourceOrigin);
        },
        Qt::BlockingQueuedConnection);
    return invoked ? proxied : QUrl();
}

void PlaybackController::setMediaItemResolver(
    std::function<MediaItem(const MediaItem &)> resolver)
{
    m_itemResolver = std::move(resolver);
}

void PlaybackController::setPageMediaResolver(MediaPageResolver *resolver)
{
    m_pageResolver = resolver;
}

void PlaybackController::setPlaylist(PlaylistModel *playlist)
{
    if (m_playlist.data() == playlist)
        return;

    // 从视频列表切到音乐/搜索列表时，先把当前视频位置落盘再换模型。
    saveCurrentVideoPosition();
    clearPendingResume();

    // 换模型必须作废在途的网页播放页解析：否则回调里的 serial 仍然有效，
    // 会把旧列表的行号套到新列表上，用户切走后又突然开始播放另一首歌/另一集。
    ++m_pageResolveSerial;

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

QUrl PlaybackController::activePlaybackUrl(const MediaItem &item) const
{
    // 换过备用线路后，预取和拖动都必须跟随正在播放的那条；继续拿主线路
    // 地址会让代理去重试已经 403/HTML 的旧线路，拖慢实际播放。
    return m_activeUpstreamUrl.isValid() ? m_activeUpstreamUrl : preferredPlaybackUrl(item);
}

void PlaybackController::playRow(int row)
{
    startPlayback(row, false);
}

void PlaybackController::startPlayback(int row, bool autoAdvance)
{
    // 每次用户点播/自动切歌都更新序号，之前还没回来的播放页解析结果作废。
    const int serial = ++m_pageResolveSerial;
    if (tryResolvePageMedia(row, serial, autoAdvance))
        return;
    playRowInternal(row, autoAdvance);
}

/// @brief 播放前把网页播放页解析为真实媒体地址。
/// @return true 表示已发起异步解析，调用方不要再启动播放器。
bool PlaybackController::tryResolvePageMedia(int row, int serial, bool autoAdvance)
{
    if (m_pageResolver == nullptr || m_playlist == nullptr || row < 0
        || row >= m_playlist->size())
        return false;

    const MediaItem item = m_playlist->itemAt(row);
    const MediaItem requestItem = m_itemResolver ? m_itemResolver(item) : item;
    if (!MediaPageResolver::needsResolution(requestItem))
        return false;

    // 已经解析过的页面直接进入 playRowInternal；那里会把缓存地址替换上去。
    QUrl cached;
    if (m_pageResolver->cachedMediaUrl(requestItem.url, &cached))
        return false;

    qCInfo(lcPlayer) << "播放前解析网页播放页" << requestItem.displayName()
                     << HttpClient::redactUrl(requestItem.url);
    m_pageResolver->resolve(requestItem, [this, serial, row, autoAdvance](
                                                const MediaItem &resolved,
                                                const QString &error) {
        if (serial != m_pageResolveSerial)
            return;
        if (!error.isEmpty()) {
            qCWarning(lcPlayer) << "网页播放页解析失败" << error
                                << HttpClient::redactUrl(resolved.url);
            // 失败时不能继续把 HTML 当视频流交给播放器：先切到该行并清空
            // source，错误弹窗才能按“刚点的条目”匹配到，而不是沿用上一首。
            m_row = row;
            m_player->setSource(QUrl());
            m_player->stop();
            emit errorOccurred(tr("无法播放：%1").arg(error));
            return;
        }
        qCInfo(lcPlayer) << "网页播放页解析完成" << resolved.displayName()
                         << HttpClient::redactUrl(resolved.url);
        playRowInternal(row, autoAdvance);
    });
    return true;
}

/// @brief 启动失败时切到下一条备用线路。
/// @return 已排队重试返回 true；没有备用/已经出声/条目已变返回 false。
/// @note 已经播出声后不换线：网络抖动或解码错误换到另一条线路会表现为
///       “播到一半突然重播/换片源”，这类错误仍保留当前条目交给用户决定。
bool PlaybackController::tryNextFallback()
{
    if (m_playAttemptStarted || m_playlist == nullptr || m_row < 0
        || m_fallbackUrls.isEmpty() || m_fallbackIdentity.isEmpty())
        return false;

    const MediaItem current = m_playlist->itemAt(m_row);
    if (!current.isValid() || mediaIdentity(current) != m_fallbackIdentity) {
        m_fallbackUrls.clear();
        m_fallbackIdentity.clear();
        return false;
    }

    m_forcedNextUrl = m_fallbackUrls.takeFirst();
    qCInfo(lcPlayer) << "主线路启动失败，改用备用线路："
                     << HttpClient::redactUrl(m_forcedNextUrl)
                     << "剩余" << m_fallbackUrls.size() << "条"
                     << "条目" << current.displayName();
    // 异步重载，避免在 QMediaPlayer 的 errorOccurred 回调里同步 setSource
    // 造成播放器状态机重入；下一拍消费 m_forcedNextUrl。
    QTimer::singleShot(0, this, [this, row = m_row] { playRowInternal(row, true); });
    return true;
}

void PlaybackController::playRowInternal(int row, bool autoAdvance)
{
    if (!m_playlist || row < 0 || row >= m_playlist->size())
        return;

    // 每次换源都从 1× 开始；长按快进在结尾自动切下一首时不能把 2× 带过去。
    m_player->setPlaybackRate(1.0);

    // 切换视频（含自动下一集）前保存正在播放的那一条；音乐会被存储层忽略。
    saveCurrentVideoPosition();
    clearPendingResume();
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
        // 只 stop 会把上一条 source 留在播放器里；清空媒体，避免错误条目
        // 仍显示为“当前加载源”，也防止后续 signals 继续带着旧时长。
        m_player->setSource(QUrl());
        if (++m_consecutiveErrors < rowCount())
            QTimer::singleShot(0, this, &PlaybackController::next);
        else
            m_player->stop();
        return;
    }

    // 播放列表落盘时不保存 Cookie；播放这一份副本按 sourceId 从源配置
    // 恢复内存态凭据，模型本身仍保持脱敏内容。
    MediaItem playbackItem = m_itemResolver ? m_itemResolver(item) : item;

    // 网页播放页的解析结果只缓存在运行期，不写回播放列表；这里把副本的
    // 主地址替换成真实 m3u8/MP4，后续代理、备用线路和缓存都按直链处理。
    if (m_pageResolver != nullptr) {
        QUrl resolvedMedia;
        if (MediaPageResolver::needsResolution(playbackItem)
            && m_pageResolver->cachedMediaUrl(playbackItem.url, &resolvedMedia)) {
            playbackItem.url = resolvedMedia;
            playbackItem.alternateUrls.removeAll(resolvedMedia.toString());
        }
    }

    // 备用线路队列只为“当前条目”的一次启动流程服务：
    // - 新条目/用户重新点播：重建候选队列；
    // - 启动失败后的自动重试：只取上一条错误选出的地址，不重置队列。
    const QString playbackIdentity = mediaIdentity(playbackItem);
    const bool fallbackRetry = !m_forcedNextUrl.isEmpty() && row == m_row
                               && playbackIdentity == m_fallbackIdentity;
    QUrl playbackUrl;
    if (fallbackRetry) {
        playbackUrl = std::exchange(m_forcedNextUrl, QUrl());
    } else {
        m_forcedNextUrl = QUrl();
        m_fallbackIdentity = playbackIdentity;
        QVector<QUrl> candidates = playbackCandidateUrls(playbackItem);
        playbackUrl = candidates.isEmpty() ? preferredPlaybackUrl(playbackItem)
                                           : candidates.takeFirst();
        m_fallbackUrls = candidates;
    }

    m_row = row;
    m_unavailableReason.clear();
    m_lastPrefetchPosition = -1;

    // 仅视频恢复进度；进度就存在列表条目里，条目删除时进度自然消失。
    if (item.type == MediaType::Video && item.positionMs > 0) {
        m_pendingResumeMs = item.positionMs;
        m_pendingResumeIdentity = mediaIdentity(item);
        m_resumeAttemptCount = 0;
        m_resumeClock.start();
        qCInfo(lcPlayer) << "准备恢复视频进度" << item.displayName() << item.positionMs;
    }

    // 加载优先级：本地文件 > 完整本地缓存 > 断点续传缓存 > 网络。
    // 本地条目本来就在磁盘上，再缓存一份纯属浪费；只有在线条目（音乐/影视）
    // 才建立与播放列表条目一一对应的整段缓存。HLS 完整分片由 HlsCacheBuilder
    // 合并转成无后缀 MP4 后，也在这个分支优先按本地文件播放。
    // 换备用线路时跳过缓存的“完整/续传”分支：缓存路径按条目身份生成，
    // 主线路的分片/半截文件可能和备用线路格式不同，混用会把坏数据当成本地完整缓存。
    // 公网源返回的播放地址可能指向 127.0.0.1/192.168.x.x；缓存和流转代理
    // 都会真实回源，必须在进入代理前拒绝，避免本机成为 SSRF 跳板。
    // 用户自己配置的本地源（sourceOrigin 也是本地）仍允许本地媒体地址。
    if (playbackItem.online
        && !NetworkAddressPolicy::allowsMediaTarget(playbackUrl, playbackItem.sourceOrigin)) {
        const QString reason = tr("媒体地址指向本机或内网，已拒绝播放");
        qCWarning(lcPlayer) << reason << HttpClient::redactUrl(playbackUrl)
                            << "源站" << playbackItem.sourceOrigin.toString();
        emit errorOccurred(reason);
        m_player->setSource(QUrl());
        if (++m_consecutiveErrors < rowCount())
            QTimer::singleShot(0, this, &PlaybackController::next);
        else
            m_player->stop();
        return;
    }
    // 通过安全校验后才记录为本轮实际上游；后台缓存/下载都以它为准。
    // 备用线路切换会再次走到这里并覆盖主线路地址。
    m_activeUpstreamUrl = playbackItem.online ? playbackUrl : QUrl();
    QUrl source = playbackUrl;
    QString sourceKind = playbackItem.online ? QStringLiteral("在线代理")
                                             : QStringLiteral("本地文件");
    if (fallbackRetry)
        sourceKind = QStringLiteral("备用线路");
    const bool cacheable = !fallbackRetry && MediaCache::isCacheable(playbackItem);
    const QString completeCache =
        (playbackItem.online && !fallbackRetry)
            ? MediaCache::completeFileForItem(playbackItem)
            : QString();
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
             cachePath, identity = MediaCache::cacheIdentity(playbackItem),
             origin = playbackItem.sourceOrigin, &proxied] {
                proxied = proxy->proxyUrlFor(url, headers, cachePath, identity, origin);
            },
            Qt::BlockingQueuedConnection);
        if (invoked && proxied.isValid())
            source = proxied;
    }

    // 新 source 的首片还没就绪，此时任何 setPosition 都会被播放器丢弃，
    // 也会让代理提前取消正在传输的分片；等 Loaded/Buffered 再统一执行。
    // 必须在 setSource 之前清 ready：setSource 可能同步触发
    // positionChanged/seekableChanged，旧位置会被误判成“已恢复”。
    m_mediaReady = false;
    m_sourcePlayRequested = false;
    m_deferredSeekMs = -1;
    m_player->setSource(source);
    m_player->play();
    m_sourcePlayRequested = true;
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
    clearPendingResume();
    // 停止也要作废尚未返回的网页播放页解析；否则用户点了停止，
    // 几百毫秒后解析完成仍会调用 playRowInternal 把播放重新拉起来。
    ++m_pageResolveSerial;
    // 停止后旧 source 的延迟定位目标必须清掉，否则下一次加载会先跳到旧位置。
    m_deferredSeekMs = -1;
    m_consecutiveErrors = 0;
    // 停止会结束任何长按快进；速率必须回到 1×。
    m_player->setPlaybackRate(1.0);
    // 已经停止的条目不再保留备用线路队列，避免下一轮播放误用旧地址。
    m_fallbackUrls.clear();
    m_fallbackIdentity.clear();
    m_forcedNextUrl = QUrl();
    m_activeUpstreamUrl = QUrl();
    m_player->stop();
}

void PlaybackController::persistCurrentPosition()
{
    // 只写模型，不动播放器状态；退出流程在保存列表前调用。
    saveCurrentVideoPosition();
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
    // 自动下一集同样可能落到网页播放页，必须复用统一点播入口解析；
    // 这里保留 autoAdvance=true，连续失败计数才能继续累加并停止跳集。
    startPlayback(row, true);
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
    if (m_pendingResumeMs > 0 && qAbs(ms - m_pendingResumeMs) > kResumeToleranceMs)
        clearPendingResume();

    // 预取窗口由 StreamProxy 统一管理（目标片起 target..target+9，4 片并发、
    // 每片 4 路 Range）；UI 只负责把“定位中”讲清楚，不在这里重复调度。
    // 这里只同步预取基准位置，避免位置跳到目标后立刻又预取目标片。
    m_lastPrefetchPosition = ms;
    m_player->setPosition(ms);

    // 目标片本身仍交给播放器/代理的多连接 Range 请求，避免重复占带宽。
    // 预取必须使用 resolver 解析后的条目：列表里保存的是脱敏头，
    // 直接拿 currentItem().headers 会让 HLS 拖动后的分片请求缺少 Cookie 而 403。
    const MediaItem modelItem = currentItem();
    const MediaItem item = m_itemResolver ? m_itemResolver(modelItem) : modelItem;
    // 和 positionChanged 的预取共用同一判定：换过备用线路后必须跟随正在
    // 播放的上游地址，否则拖动会去预取已经 403/HTML 的主线路。
    const QUrl playbackUrl = activePlaybackUrl(item);
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

void PlaybackController::setVolumePercent(int percent)
{
    m_volume = qBound(0, percent, 100);
    // 听觉曲线：平方映射，避免滑块中段过响
    const float linear = static_cast<float>(m_volume) / 100.0f;
    m_audio->setVolume(linear * linear);
    emit volumePercentChanged(m_volume);
}

qreal PlaybackController::playbackRate() const
{
    return m_player->playbackRate();
}

void PlaybackController::setPlaybackRate(qreal rate)
{
    // 长按快进只允许受控倍速；异常值可能让音频设备输出失真或直接静音。
    m_player->setPlaybackRate(qBound(0.25, rate, 4.0));
}

qint64 PlaybackController::steppedSeekTarget(qint64 position, qint64 duration,
                                              qint64 deltaMs)
{
    if (duration <= 0)
        return position;
    const qint64 base = qBound<qint64>(0, position, duration);
    return qBound<qint64>(0, base + deltaMs, duration);
}

void PlaybackController::onPlaylistRowRemoved(int row)
{
    if (row < 0)
        return;

    if (m_row == row) {
        // 条目已经从模型里移除，进度属于被删条目，随条目一起丢弃；
        // 不能再读当前播放器位置写进后面那条，否则会把进度串到下一集。
        clearPendingResume();
        // 这一行对应的网页播放页解析也必须作废：行号在删除后已经属于下一条，
        // 继续回调会把解析结果播到别人头上。
        ++m_pageResolveSerial;
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
            const QString finishedIdentity = mediaIdentity(finishedItem);
            const int finishedRow = m_row;
            m_player->setPosition(qMax<qint64>(0, endPosition - 2000));
            QTimer::singleShot(300, this, [this, finishedRow, finishedIdentity] {
                // 300ms 内用户可能已切歌/删行；必须确认仍是同一条条目才继续播放，
                // 只检查 hasCurrentItem 会把新歌当旧歌重播。
                if (!hasCurrentItem() || m_row != finishedRow)
                    return;
                if (mediaIdentity(currentItem()) != finishedIdentity)
                    return;
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

    qint64 position = m_player->position();
    // 恢复还没确认到位就被切走时，播放器位置可能还是 0/旧 source 的值。
    // 此时必须保留用户原来的进度，不能用片头位置把它覆盖掉。
    if (m_pendingResumeMs > 0
        && mediaIdentity(item) == m_pendingResumeIdentity
        && position + kResumeToleranceMs < m_pendingResumeMs) {
        position = m_pendingResumeMs;
    }
    if (position <= 0)
        return;

    // 播放器可能还没上报时长；优先沿用条目里已知的时长，避免把有效 duration
    // 当 0 写入（某些源要等整段缓存完成才给 duration）。
    const qint64 duration = qMax(m_player->duration(), item.durationMs);
    // 两条来源都还没有时长时，position 可能是片头解码前的中间值；此时写入
    // 会把用户原有进度覆盖成不可信位置，等下一次有时长再保存。
    if (duration <= 0)
        return;

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

void PlaybackController::clearPendingResume()
{
    m_pendingResumeMs = -1;
    m_pendingResumeIdentity.clear();
    m_resumeAttemptCount = 0;
    if (m_resumeTimer != nullptr)
        m_resumeTimer->stop();
    m_resumeClock.invalidate();
}

void PlaybackController::schedulePendingResume(int delayMs)
{
    if (m_pendingResumeMs <= 0 || m_resumeTimer == nullptr)
        return;
    // positionChanged 在片头阶段可能每几十毫秒来一次；只允许一个在途定时器，
    // 否则每次 start() 都会把触发时间往后推，永远等不到执行。
    if (!m_resumeTimer->isActive())
        m_resumeTimer->start(delayMs);
}

void PlaybackController::applyPendingResume()
{
    if (m_pendingResumeMs <= 0 || !hasCurrentItem())
        return;

    const MediaItem item = currentItem();
    if (item.type != MediaType::Video
        || mediaIdentity(item) != m_pendingResumeIdentity) {
        clearPendingResume();
        return;
    }

    qint64 target = m_pendingResumeMs;
    const qint64 duration = qMax(m_player->duration(), item.durationMs);
    if (duration > 0 && target > duration - 1000)
        target = qMax<qint64>(0, duration - 1000);

    // 新媒体首片未就绪时 setPosition 会被丢弃，还会干扰 StreamProxy 的预取；
    // 这里只等待 Loaded/Buffered，不提前下发定位。另外 setSource 同步
    // 进入 Loaded/Buffered 时 play() 还没调用，此时 seek 会被 play() 重置。
    if (!m_mediaReady || !m_sourcePlayRequested) {
        schedulePendingResume(500);
        return;
    }
    if (!m_player->isSeekable() && m_player->mediaStatus() != QMediaPlayer::BufferedMedia) {
        schedulePendingResume(500);
        return;
    }

    // 位置已经接近或越过目标，不再反复 seek；但必须在“已下发过一次 seek”
    // 之后才成立。setSource() 阶段播放器可能上报旧 source 的位置，若提前
    // 判定恢复成功，新 source 会从 0 播放并覆盖真实进度。
    const qint64 position = m_player->position();
    if (m_resumeAttemptCount > 0 && resumeTargetReached(position, target)) {
        qCInfo(lcPlayer) << "视频进度已恢复" << item.displayName() << position;
        clearPendingResume();
        return;
    }

    if (m_resumeClock.isValid() && m_resumeClock.elapsed() > 15000) {
        qCWarning(lcPlayer) << "视频进度恢复超时，放弃并从头播放"
                            << item.displayName() << "目标" << target
                            << "当前" << position;
        clearPendingResume();
        return;
    }

    ++m_resumeAttemptCount;
    qCInfo(lcPlayer) << "请求恢复视频进度" << item.displayName() << target
                     << "第" << m_resumeAttemptCount << "次";
    seek(target);
    // 不在 seek() 里清任务：由 positionChanged 确认真正到达目标；
    // 若播放器再次把位置重置回 0，定时器会继续在 15 秒窗口内纠正。
    schedulePendingResume(1000);
}

qint64 PlaybackController::position() const
{
    return m_player->position();
}

qint64 PlaybackController::duration() const
{
    return m_player->duration();
}
